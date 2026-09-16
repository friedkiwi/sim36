#include "Devices/DeviceSet.h"

#include <algorithm>

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Devices/UnitBlock.h"
#include "Devices/WorkStationActions.h"
#include "Devices/WorkStationIob.h"
#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::devices {

using processors::controlstorage::Ecm;
using processors::controlstorage::RequestBlock;

namespace {

struct SavedScreenRecord {
    int bodyOffset = 0;
    int bodyLength = 0;
    uint8_t readMode = 0;
};

// SSP normally submits command 12 for this record, but #WDDG also passes the
// same saved-screen envelope to command 27 while unwinding an overlaid panel.
// The 22-byte prefix belongs to SSP; only the terminal-returned 0412 body is
// valid in an RFC 1205 Restore Screen record.
bool decodeSavedScreenRecord(const uint8_t* data, int offset, int length, SavedScreenRecord& record)
{
    constexpr int kPrefixLength = 22;
    if (length < kPrefixLength + 2 || data[offset] != 0x04 || data[offset + 1] != 0x12) return false;

    int allocation = (data[offset + 2] << 8) | data[offset + 3];
    int imageLength = (data[offset + 20] << 8) | data[offset + 21];
    int bodyLength = imageLength + 2;
    uint8_t mode = data[offset + 18];
    if (allocation > length || kPrefixLength + bodyLength > allocation || kPrefixLength + bodyLength > length ||
        (mode != 0 && mode != 1 && mode != 0x20 && mode != 0x21) || data[offset + kPrefixLength] != 0x04 ||
        data[offset + kPrefixLength + 1] != 0x12)
        return false;

    record.bodyOffset = offset + kPrefixLength;
    record.bodyLength = bodyLength;
    record.readMode = mode;
    return true;
}

}  // namespace

DeviceSet::DeviceSet(machine::MachineState& m, storage::DiskBackend& volume, monitor::Tracer& trace)
    : disk(m, volume, trace), diskette(m, trace), tape(m, trace), m_(m), trace_(trace), workStations_(trace)
{
}

void DeviceSet::addStation(VirtualWorkstation& ws)
{
    stations_.set(ws.id(), &ws);
    workStations_.attach(std::make_unique<WorkStationSlot>(ws.port(), ws.address(), ws.deviceCode(),
                                                           std::make_unique<VirtualWorkstationBackend>(ws), ws.signOnAtIpl()));
}

void DeviceSet::addPrinter(VirtualPrinter& p)
{
    printers_.set(p.id(), &p);
    workStations_.attach(std::make_unique<WorkStationSlot>(p.port(), p.address(), p.deviceCode(),
                                                           std::make_unique<VirtualPrinterBackend>(p)));
}

std::vector<VirtualWorkstation*> DeviceSet::stations()
{
    std::vector<VirtualWorkstation*> out;
    for (auto& item : stations_.items()) out.push_back(*item.second);
    return out;
}

std::vector<VirtualPrinter*> DeviceSet::printers()
{
    std::vector<VirtualPrinter*> out;
    for (auto& item : printers_.items()) out.push_back(*item.second);
    return out;
}

bool DeviceSet::isPending(int iob) const
{
    return pendingInputReads_.contains(iob) || pendingC1Completions_.contains(iob) || pendingPutWithInvites_.contains(iob) ||
           pendingScreenSaves_.contains(iob) || pendingControllerInvites_.count(iob) != 0;
}

void DeviceSet::resetPendingIo()
{
    pendingInputReads_.clear();
    lastDeferredInput_.reset();
    inputStagingPages_.clear();
    inputResponseStatus_.clear();
    pendingC1Completions_.clear();
    pendingPutWithInvites_.clear();
    pendingScreenSaves_.clear();
    pendingControllerInvites_.clear();
    pendingAction0ActivationUnits_.clear();
    action0ActivatedUnits_.clear();
}

void DeviceSet::activateIplWorkStations()
{
    for (WorkStationSlot* slot : workStations_.slots()) slot->activateAtIpl(slot->signOnAtIpl);
}

DeviceSet::PendingCheckpoint DeviceSet::capturePendingCheckpoint() const
{
    PendingCheckpoint s;
    for (const auto& key : pendingInputReads_.keys()) {
        const PendingInputRead& p = *pendingInputReads_.find(key);
        s.inputReadPairs.push_back(key);
        s.inputReadPairs.push_back(p.slot->unitAddress());
        s.inputReadPairs.push_back(p.command);
        s.inputReadPairs.push_back(p.bufferField);
        s.inputReadPairs.push_back(p.capacity);
        s.inputReadPairs.push_back(p.stagingBlockDisplacement);
        s.inputReadPairs.push_back(static_cast<int>(p.destination.size()));
        s.inputReadPairs.insert(s.inputReadPairs.end(), p.destination.begin(), p.destination.end());
    }
    for (const auto& key : inputStagingPages_.keys()) {
        s.inputStagingPairs.push_back(key);
        s.inputStagingPairs.push_back(*inputStagingPages_.find(key));
    }
    for (const auto& key : inputResponseStatus_.keys()) {
        const auto& status = *inputResponseStatus_.find(key);
        s.inputResponseStatus.push_back(key);
        s.inputResponseStatus.push_back(static_cast<int>(status.size()));
        for (uint8_t b : status) s.inputResponseStatus.push_back(b);
    }
    for (const auto& key : pendingC1Completions_.keys()) {
        s.pendingC1Pairs.push_back(key);
        s.pendingC1Pairs.push_back(*pendingC1Completions_.find(key));
    }
    for (WorkStationSlot* slot : workStations_.slots()) {
        if (slot->nativeActive()) s.nativeActiveUnits.push_back(slot->unitAddress());
        if (slot->configured()) s.configuredUnits.push_back(slot->unitAddress());
        if (slot->internalRendererBound()) s.internalRendererUnits.push_back(slot->unitAddress());
        if (slot->transferRendererBound()) s.transferRendererUnits.push_back(slot->unitAddress());
    }
    s.controllerInvites.assign(pendingControllerInvites_.begin(), pendingControllerInvites_.end());
    s.pendingActivationUnits.assign(pendingAction0ActivationUnits_.begin(), pendingAction0ActivationUnits_.end());
    s.activatedUnits.assign(action0ActivatedUnits_.begin(), action0ActivatedUnits_.end());
    return s;
}

bool DeviceSet::restorePendingCheckpoint(const PendingCheckpoint& s, std::string& failure)
{
    resetPendingIo();
    failure.clear();
    const auto& pairs = s.inputReadPairs;
    for (std::size_t i = 0; i < pairs.size();) {
        if (i + 7 > pairs.size()) {
            failure = "truncated pending workstation read checkpoint";
            return false;
        }
        int count = pairs[i + 6];
        if (count < 0 || i + 7 + static_cast<std::size_t>(count) > pairs.size()) {
            failure = "invalid captured buffer in pending workstation read checkpoint";
            return false;
        }
        WorkStationSlot* slot = workStations_.find(pairs[i + 1]);
        if (slot == nullptr) {
            failure = "checkpoint pending read names absent workstation unit";
            return false;
        }
        PendingInputRead p;
        p.slot = slot;
        p.command = static_cast<uint8_t>(pairs[i + 2]);
        p.bufferField = pairs[i + 3];
        p.capacity = pairs[i + 4];
        p.stagingBlockDisplacement = pairs[i + 5];
        p.destination.assign(pairs.begin() + static_cast<std::ptrdiff_t>(i) + 7,
                             pairs.begin() + static_cast<std::ptrdiff_t>(i) + 7 + count);
        pendingInputReads_.set(pairs[i], std::move(p));
        i += 7 + static_cast<std::size_t>(count);
    }
    if (s.inputStagingPairs.size() % 2 != 0) {
        failure = "invalid workstation input staging checkpoint";
        return false;
    }
    for (std::size_t i = 0; i < s.inputStagingPairs.size(); i += 2)
        inputStagingPages_.set(s.inputStagingPairs[i], s.inputStagingPairs[i + 1]);
    const auto& rs = s.inputResponseStatus;
    for (std::size_t i = 0; i < rs.size();) {
        if (i + 2 > rs.size() || rs[i + 1] != 12 || i + 14 > rs.size()) {
            failure = "invalid workstation response-status checkpoint";
            return false;
        }
        std::vector<uint8_t> status(12);
        for (int n = 0; n < 12; n++) status[static_cast<std::size_t>(n)] = static_cast<uint8_t>(rs[i + 2 + static_cast<std::size_t>(n)]);
        inputResponseStatus_.set(rs[i], std::move(status));
        i += 14;
    }
    if (s.pendingC1Pairs.size() % 2 != 0) {
        failure = "invalid pending workstation C1 checkpoint";
        return false;
    }
    for (std::size_t i = 0; i < s.pendingC1Pairs.size(); i += 2)
        pendingC1Completions_.set(s.pendingC1Pairs[i], s.pendingC1Pairs[i + 1]);
    for (int x : s.controllerInvites) pendingControllerInvites_.insert(x);
    for (int x : s.pendingActivationUnits) pendingAction0ActivationUnits_.insert(x);
    for (int x : s.activatedUnits) action0ActivatedUnits_.insert(x);
    std::set<int> nativeActive(s.nativeActiveUnits.begin(), s.nativeActiveUnits.end());
    std::set<int> configured(s.configuredUnits.begin(), s.configuredUnits.end());
    std::set<int> internalRenderers(s.internalRendererUnits.begin(), s.internalRendererUnits.end());
    std::set<int> transferRenderers(s.transferRendererUnits.begin(), s.transferRendererUnits.end());
    for (WorkStationSlot* slot : workStations_.slots())
        slot->restoreNativeState(nativeActive.count(slot->unitAddress()) != 0, configured.count(slot->unitAddress()) != 0,
                                 internalRenderers.count(slot->unitAddress()) != 0,
                                 transferRenderers.count(slot->unitAddress()) != 0);
    return true;
}

int DeviceSet::countPendingPutWithInvitesForUnit(int unitAddress) const
{
    int count = 0;
    for (const auto& key : pendingPutWithInvites_.keys())
        if ((*pendingPutWithInvites_.find(key))->unitAddress() == (unitAddress & 0xFF)) count++;
    return count;
}

bool DeviceSet::hasPendingInputForUnit(int unitAddress) const
{
    const int unit = unitAddress & 0xFF;
    for (const auto& key : pendingInputReads_.keys()) {
        const PendingInputRead* pending = pendingInputReads_.find(key);
        if (pending != nullptr && pending->slot != nullptr && pending->slot->unitAddress() == unit &&
            pending->slot->backend()->pendingInput() > 0)
            return true;
    }
    for (const auto& key : pendingPutWithInvites_.keys()) {
        WorkStationSlot* const* pending = pendingPutWithInvites_.find(key);
        if (pending != nullptr && *pending != nullptr && (*pending)->unitAddress() == unit &&
            (*pending)->backend()->inviteResponsePending())
            return true;
    }
    for (const auto& key : pendingC1Completions_.keys()) {
        const int* pendingUnit = pendingC1Completions_.find(key);
        WorkStationSlot* slot = pendingUnit == nullptr ? nullptr : workStations_.find(*pendingUnit);
        if (pendingUnit != nullptr && *pendingUnit == unit && slot != nullptr && slot->backend()->pendingInput() > 0)
            return true;
    }
    return false;
}

void DeviceSet::recordAction0Activation(int unitAddress) { recordPowerOnActivation(unitAddress, "NuWsIoAction(0)"); }

void DeviceSet::recordPowerOnActivation(int unitAddress, const std::string& producer)
{
    int unit = unitAddress & 0xFF;
    WorkStationSlot* bound = workStations_.find(unit);
    if (bound != nullptr) bound->displayDetached = false;
    if (action0ActivatedUnits_.insert(unit).second) {
        pendingAction0ActivationUnits_.insert(unit);
        trace_.ws("  {} powerOn/activate: unit {:02X} has one-shot NuActiveWs+BA.20 status; no guest state changed", producer,
                  unit);
    } else {
        trace_.ws("  {} powerOn: unit {:02X} is already active; NuDsp5250::activate supplies no new BA.20 status", producer,
                  unit);
    }
}

bool DeviceSet::deactivateUnit(int unitAddress)
{
    int unit = unitAddress & 0xFF;
    WorkStationSlot* slot = workStations_.find(unit);
    if (slot != nullptr) slot->displayDetached = true;
    pendingAction0ActivationUnits_.erase(unit);
    if (action0ActivatedUnits_.erase(unit) == 0) return false;
    trace_.ws("  unit {:02X} powered off: native active display deactivated; the next powerOn/activate supplies a new "
              "NuActiveWs+BA.20 status",
              unit);
    return true;
}

bool DeviceSet::tryTakeAction0Activation(int& unitAddress)
{
    unitAddress = 0;
    if (pendingAction0ActivationUnits_.empty()) return false;
    unitAddress = *pendingAction0ActivationUnits_.begin();
    pendingAction0ActivationUnits_.erase(pendingAction0ActivationUnits_.begin());
    trace_.ws("  NuWsIoAction response -> checkForInviteComplete: consumes unit {:02X} NuActiveWs+BA.20 without a guest "
              "unit-FF IOB predicate",
              unitAddress);
    return true;
}

bool DeviceSet::tryFindPendingInput(int& iob)
{
    for (auto& p : pendingScreenSaves_.items()) {
        if (p.second->slot->backend()->pendingSaveScreens() > 0) {
            iob = p.first;
            return true;
        }
    }
    for (auto& p : pendingInputReads_.items()) {
        if (p.second->slot->backend()->pendingInput() > 0) {
            iob = p.first;
            trace_.ws("  pending Read Input Fields IOB {:06X} has a device record; retaining it until the recovered WSCF/AID "
                      "status mapping is implemented",
                      iob);
            return true;
        }
    }
    for (auto& p : pendingPutWithInvites_.items()) {
        if ((*p.second)->backend()->inviteResponsePending()) {
            iob = p.first;
            return true;
        }
    }
    iob = 0;
    return false;
}

// The device builds the action status as flag, AID, cursor; the response
// path places it in the control field; the status save then copies twelve
// bytes to the unit block's +0x13..+0x1E.  The later Read Input Fields call
// deliberately clears its action AID fields, so this earlier status delivery
// is not optional and must be one-shot.  When the unit block is not in class
// C0 the output-open path also copies the AID to +0x8D, the legitimate
// producer of the command processor's request byte.
bool DeviceSet::tryDeliverInputStatus(int unitBlock)
{
    if (unitBlock == 0 || m_.readHalf(unitBlock) != WorkStationIob::kUnitBlockEyecatcher) return false;

    int unit = m_.readByte(unitBlock + WorkStationIob::kOffUnitAddress);
    WorkStationSlot* slot = workStations_.find(unit);
    if (slot == nullptr || slot->isPrinter) return false;

    uint16_t cursor;
    uint8_t aid;
    if (!slot->backend()->tryTakeInputStatus(cursor, aid)) return false;

    constexpr int kOffActionStatus = 0x13;   // decimal 19: the action status image
    // Unit block decimal +124: bit 0x10 is the one the display manager's
    // gate tests: "the device is holding input field data that the guest
    // has not read yet".
    constexpr int kOffInputRead = 0x7C;
    constexpr uint8_t kInputFieldsAvailable = 0x10;

    // The Work Station Status Flag (SY31-9004-3, ERAP field 70-525): bit 0
    // (0x80) command complete, bit 1 (0x40) modify data tag, bit 3 (0x10)
    // attention identifier present.  The accepted-AID return is not an
    // ordinary action-complete status and does not carry 0x80; bit 0x40
    // belongs to a Read Input Fields action and to nothing else.  The
    // action status is cleared at every new operation, so the previous
    // image is not an accumulator.
    constexpr uint8_t kModifyDataTag = 0x40;
    constexpr uint8_t kAidPresent = 0x10;
    uint8_t flag = kAidPresent;
    std::vector<uint8_t> responseStatus(12, 0);
    responseStatus[0] = flag;
    responseStatus[1] = aid;
    responseStatus[2] = static_cast<uint8_t>(cursor >> 8);
    responseStatus[3] = static_cast<uint8_t>(cursor);
    for (int n = 0; n < 12; n++) m_.writeByte(unitBlock + kOffActionStatus + n, responseStatus[static_cast<std::size_t>(n)]);
    inputResponseStatus_.set(unit, responseStatus);
    uint8_t unitClass = m_.readByte(unitBlock + WorkStationIob::kOffClass);
    if (unitClass != 0xC0) {
        // The arm taken when the unit block's class is not C0: two stores.
        // The AID goes to +0x8D, and +0x7C bit 0x10 is opened by a
        // put-with-invite response (0x40 clear) and closed by a Read Input
        // Fields that returned fields (0x40 set).  The display manager
        // requires it: for a unit-block-addressed request the only evidence
        // it accepts that there is input to fetch is this bit.
        m_.writeByte(unitBlock + 0x8D, aid);
        uint8_t inputAvailable = m_.readByte(unitBlock + kOffInputRead);
        inputAvailable = (flag & kModifyDataTag) != 0 ? static_cast<uint8_t>(inputAvailable & ~kInputFieldsAvailable)
                                                       : static_cast<uint8_t>(inputAvailable | kInputFieldsAvailable);
        m_.writeByte(unitBlock + kOffInputRead, inputAvailable);
    }
    trace_.ws("  input-ready response for unit {:02X}: real AID {:02X}, cursor {:04X} -> TUB {:06X}+13..16 through "
              "processStatus/action/WSCF/wsavstat; wsopend {}; field record retained for guest Read Input Fields",
              unit, aid, cursor, unitBlock,
              unitClass != 0xC0
                  ? fmt::format("copies WSCF+03 AID to TUB+8D and sets TU+7C.10 = {:02X} (class {:02X})",
                                m_.readByte(unitBlock + kOffInputRead), unitClass)
                  : std::string("does not copy WSCF+03 to TUB+8D while class is C0"));
    return true;
}

// The terminal behind a unit was switched off while the controller still
// owned a guest request against it.  Ending the session tears the native
// display down; from then on every command to that unit is rejected with
// the "device not attached" control field (+2 = 0x28, +6/+7 = 02 03), which
// is the decoded shape of the same condition and is used for the retained
// operation as well.  There is no watchdog, no poll and no invite timeout: a
// station that stops answering stays outstanding until something tears it
// down, which is what a socket close is here.
bool DeviceSet::tryFailPendingOperationForUnit(int unitAddress, int& failedIob, std::string& what)
{
    unitAddress &= 0xFF;
    failedIob = 0;
    what.clear();
    for (auto& p : pendingPutWithInvites_.items()) {
        if ((*p.second)->unitAddress() != unitAddress) continue;
        failedIob = p.first;
        what = "PUT-with-invite (A7)";
        pendingPutWithInvites_.erase(p.first);
        break;
    }
    if (failedIob == 0)
        for (auto& p : pendingInputReads_.items()) {
            if (p.second->slot->unitAddress() != unitAddress) continue;
            failedIob = p.first;
            what = fmt::format("Read Input Fields (command {:02X})", p.second->command);
            pendingInputReads_.erase(p.first);
            break;
        }
    if (failedIob == 0)
        for (auto& p : pendingC1Completions_.items()) {
            if (*p.second != unitAddress) continue;
            failedIob = p.first;
            what = "class-C1 response wait";
            pendingC1Completions_.erase(p.first);
            break;
        }
    if (failedIob == 0)
        for (auto& p : pendingScreenSaves_.items()) {
            if (p.second->slot->unitAddress() != unitAddress) continue;
            failedIob = p.first;
            what = "Save Screen";
            pendingScreenSaves_.erase(p.first);
            break;
        }
    if (failedIob == 0) return false;

    inputResponseStatus_.erase(unitAddress);
    inputStagingPages_.erase(unitAddress);
    writeDeviceNotAttached(failedIob);
    trace_.ws("  unit {:02X} powered off: retained {} IOB {:06X} FAILED - TU+13=28, +17/18=02 03 (device not attached), "
              "class C0, completion 41",
              unitAddress, what, failedIob);
    return true;
}

// The rejection as the status save lands it in the unit block: echoed
// opcode at +12, 0x28 at +13, program status 02/03 at +17..18, the rest of
// the status area clear, class back to C0, ECM completion 41.
void DeviceSet::writeDeviceNotAttached(int iob)
{
    int tub = WorkStationIob::resolveUnitBlock(m_, iob);
    if (tub > 0 && m_.readHalf(tub) == WorkStationIob::kUnitBlockEyecatcher) {
        m_.writeByte(tub + 0x12, m_.readByte(tub + 0x0B));   // echoed opcode
        m_.writeByte(tub + 0x13, 0x28);                       // error status, program class
        for (int n = 0x14; n <= 0x1E; n++) m_.writeByte(tub + n, 0);
        m_.writeByte(tub + 0x17, 0x02);                       // program status 02/03:
        m_.writeByte(tub + 0x18, 0x03);                       // device not attached
        m_.writeByte(tub + WorkStationIob::kOffClass, static_cast<uint8_t>(WorkStationIob::kClassWorkStation));
    }
    IoBlock::complete(m_, iob, 1);
}

// Complete one retained Read Input Fields whose real response has arrived.
// Commands 42 and ordinary 32 copy fields after the three-byte cursor/AID
// prefix; the special 0x18xx form of command 32 emits its six-EE/length
// prefix; command 22 remains retained because its cursor/SBA relocation
// contains two undecoded operations.
bool DeviceSet::tryCompletePendingInput(int& completedIob)
{
    completedIob = 0;
    for (auto& p : pendingScreenSaves_.items()) {
        if (p.second->slot->backend()->pendingSaveScreens() == 0) continue;
        completeScreenSave(p.first, *p.second);
        pendingScreenSaves_.erase(p.first);
        completedIob = p.first;
        return true;
    }
    for (auto& p : pendingC1Completions_.items()) {
        const std::vector<uint8_t>* status = inputResponseStatus_.find(*p.second);
        if (status == nullptr) continue;
        int unit = *p.second;
        applyC1Response(p.first, *status);
        inputResponseStatus_.erase(unit);
        pendingC1Completions_.erase(p.first);
        IoBlock::complete(m_, p.first, 0);
        completedIob = p.first;
        trace_.ws("  C1 response wait COMPLETE on real terminal response: IOB {:06X}, unit {:02X}; wscmdend/wsavstat "
                  "imported one WSCF",
                  p.first, unit);
        return true;
    }
    // The put-with-invite's action survives until this real device response.
    // Complete its original SVC-43 element now, but do not discard the
    // record: the status has been reported and the device's retained-input
    // slot now owns the field data for a following Read Input Fields.
    // Removing it from the wire queue is essential: one response completes
    // exactly one A7.
    for (auto& p : pendingPutWithInvites_.items()) {
        WorkStationSlot* slot = *p.second;
        if (!slot->backend()->tryCompleteInviteResponse()) continue;
        // Keep the control field through this action's following C1 pass.
        // Completing the retained A7 and importing its response status are
        // two guest-visible phases of the same controller operation.  A new
        // C0 A7 clears stale status in beginOutputRequest(), so preserving it
        // here cannot leak the AID into an unrelated display operation.
        IoBlock::complete(m_, p.first, 0);
        pendingPutWithInvites_.erase(p.first);
        completedIob = p.first;
        trace_.ws("  PUT-with-invite COMPLETE on real terminal response: IOB {:06X}, unit {:02X}; response retained in device "
                  "state for guest Read Input Fields",
                  p.first, slot->unitAddress());
        return true;
    }
    int selected = 0;
    PendingInputRead* pending = nullptr;
    for (auto& p : pendingInputReads_.items()) {
        if (p.second->slot->backend()->pendingInput() == 0) continue;
        int cmd = p.second->command;
        if (cmd != 0x32 && cmd != 0x42) {
            trace_.ws("  pending Read Input Fields IOB {:06X} command {:02X}: response retained; command 22 cursor/SBA "
                      "relocation is not fully decoded",
                      p.first, cmd);
            continue;
        }
        selected = p.first;
        pending = p.second;
        break;
    }
    if (selected == 0) return false;

    std::vector<uint8_t> raw;
    int cmdByte = pending->command;
    if (!pending->slot->backend()->tryTakeInputFields(static_cast<uint8_t>(cmdByte), raw) || raw.size() < 3) {
        trace_.ws("  Read Input Fields IOB {:06X}: terminal response shorter than cursor[2]+AID; not completed", selected);
        return false;
    }

    const std::vector<int>& destination = pending->destination;
    int requested = pending->capacity;
    if (static_cast<int>(destination.size()) < requested) {
        trace_.ws("  Read Input Fields IOB {:06X}: captured destination of {} byte(s) is invalid", selected, requested);
        IoBlock::complete(m_, selected, 4);
        pendingInputReads_.erase(selected);
        completedIob = selected;
        return true;
    }

    int available = static_cast<int>(raw.size()) - 3;
    int copied;
    if (cmdByte == 0x32 && (requested & 0xFF00) == 0x1800) {
        copied = std::min(available, std::max(0, requested - 8));
        for (int n = 0; n < 6 && n < requested; n++) m_.writeByte(destination[static_cast<std::size_t>(n)], 0xEE);
        if (requested >= 8) {
            m_.writeByte(destination[6], static_cast<uint8_t>(available >> 8));
            m_.writeByte(destination[7], static_cast<uint8_t>(available));
            writeCaptured(destination, 8, raw.data(), 3, copied);
        }
    } else {
        copied = std::min(available, requested);
        writeCaptured(destination, 0, raw.data(), 3, copied);
        // Also hold the parsed bytes for delivery onto the work-space
        // block's own logical pages when the display manager maps that
        // block. The immediate copy above belongs only to this read action's
        // issue-time IOB destination; the earlier A7 PUT page is provenance,
        // not an alias for either destination.
        if (pending->stagingBlockDisplacement >= 0 && copied > 0) {
            DeferredWorkStationInput held;
            held.bytes.assign(raw.begin() + 3, raw.begin() + 3 + copied);
            held.blockDisplacement = pending->stagingBlockDisplacement;
            lastDeferredInput_ = std::move(held);
        }
    }

    // Read Input Fields is a second action with a second control field
    // response: it starts from a cleared action status, sets the modify data
    // tag when field bytes are present, then clears AID-present and the AID
    // byte; the common successful-response arm adds command-complete.  The
    // status save replaces the unit block's +13..1E and retires +7C.10.  It
    // does not replace +8D here: that copy exists only on the non-C0 arm,
    // and the display manager consumes and clears it itself.
    int readUnitBlock = WorkStationIob::resolveUnitBlock(m_, selected);
    if (readUnitBlock > 0 && m_.readHalf(readUnitBlock) == WorkStationIob::kUnitBlockEyecatcher) {
        constexpr int kOffStatus = 0x13;
        constexpr int kOffInputRead = 0x7C;
        constexpr uint8_t kCommandComplete = 0x80;
        constexpr uint8_t kModifyDataTag = 0x40;
        constexpr uint8_t kInputFieldsAvailable = 0x10;
        uint8_t readStatus = static_cast<uint8_t>(kCommandComplete | (available > 0 ? kModifyDataTag : 0));
        m_.writeByte(readUnitBlock + kOffStatus, readStatus);
        m_.writeByte(readUnitBlock + kOffStatus + 1, 0);   // the AID byte cleared
        m_.writeByte(readUnitBlock + kOffStatus + 2, raw[0]);
        m_.writeByte(readUnitBlock + kOffStatus + 3, raw[1]);
        for (int n = 4; n < 12; n++) m_.writeByte(readUnitBlock + kOffStatus + n, 0);
        m_.writeByte(readUnitBlock + kOffInputRead,
                     static_cast<uint8_t>(m_.readByte(readUnitBlock + kOffInputRead) & ~kInputFieldsAvailable));
        trace_.ws("  Read Input Fields status: TUB {:06X}+13={:02X}, +14 AID cleared, +8D preserved for #WDDG consumption, "
                  "cursor={:02X}{:02X}, +7C.10 retired",
                  readUnitBlock, readStatus, raw[0], raw[1]);
    }

    // The device stores no returned byte count in the guest IOB/TUB.
    // The first captured address is taken before the pending read (which
    // owns the captured buffer) is released.
    const int firstDestination = destination.empty() ? 0 : destination[0];
    IoBlock::complete(m_, selected, 0);
    pendingInputReads_.erase(selected);
    completedIob = selected;
    trace_.ws("  Read Input Fields COMPLETE: IOB {:06X}, command {:02X}, copied {} real field byte(s) from response+3 to guest "
              "{:06X}; AID was already reported through TUB status and no returned length was invented",
              selected, cmdByte, copied, firstDestination);
    return true;
}

std::optional<DeviceSet::DeferredWorkStationInput> DeviceSet::takeDeferredWorkStationInput()
{
    std::optional<DeferredWorkStationInput> held = std::move(lastDeferredInput_);
    lastDeferredInput_.reset();
    return held;
}

void DeviceSet::completeScreenSave(int iob, PendingScreenSave& pending)
{
    std::vector<uint8_t> body;
    if (!pending.slot->backend()->tryTakeSaveScreen(body) || body.size() < 2 || body[0] != 0x04 || body[1] != 0x12) {
        trace_.ws("  Save Screen IOB {:06X}: opcode-04 response does not begin 04 12; device error, no keyboard status was "
                  "produced",
                  iob);
        IoBlock::complete(m_, iob, 4);
        return;
    }

    int screenImageLength = static_cast<int>(body.size()) - 2;
    int allocation = (screenImageLength + 0x119) & 0xFF00;
    int requested = pending.capacity;
    const std::vector<int>& destination = pending.destination;
    if (allocation > requested || static_cast<int>(destination.size()) < allocation) {
        trace_.ws("  Save Screen IOB {:06X}: terminal image {} byte(s) requires allocation {}, guest supplied {}; device error "
                  "without truncation",
                  iob, screenImageLength, allocation, requested);
        IoBlock::complete(m_, iob, 4);
        return;
    }

    writeCapturedHalf(destination, 0, 0x0412);
    writeCapturedHalf(destination, 2, static_cast<uint16_t>(allocation));
    m_.writeByte(destination[18], pending.savedReadMode);
    writeCapturedHalf(destination, 20, static_cast<uint16_t>(screenImageLength));
    writeCaptured(destination, 22, body.data(), 0, static_cast<int>(body.size()));
    IoBlock::complete(m_, iob, 0);
    trace_.ws("  Save Screen COMPLETE: IOB {:06X}, real opcode-04/0412 terminal image {} byte(s) -> guest {:06X}, allocation "
              "{}, saved mode {:02X}",
              iob, screenImageLength, destination[0], allocation, pending.savedReadMode);
}

bool DeviceSet::deviceSvc(processors::controlstorage::SvcRequest& req)
{
    // The IOB pointer arrives exactly the way every device address does: the
    // raw XR1 prefix:reg pair, resolved at use, real unless bit 0x800000 is
    // set, then translated through the task's ATRs.  Phase 1 really does
    // hand the supervisor 801E26, so the resolution is load-bearing.
    int iobField = RequestBlock::readXr1Field(m_, req.requestBlock);
    int iob;
    if (!resolveBuffer(iobField, iob)) {
        trace_.diskIo("SVC {:02X}: IOB address {:06X} is task-translated and its page is not mapped - refused (nusvpta would "
                      "fault here too)",
                      req.r, iobField);
        return false;
    }
    switch (req.r) {
        case 0x40: return disk.execute(iob, req.q);
        case 0x41: return diskette.execute(iob, req.q);
        case 0x42:
        case 0x43: return workStationIoch(iob, req.r);
        // Declared by the volume's unit definition table, not modelled here:
        // from the guest's side these devices EXIST; what is missing is the
        // model of them.  SVC 44, Data Communications IOCH (SA21-9436
        // 3-138): a delayed, privileged device call whose IOB names one of
        // the comm lines; the machine constructs a per-protocol line action
        // driving a live line to a remote endpoint, a backend this emulator
        // does not have and cannot fake.  So it is DECLINED, not guessed.
        case 0x44: return declineUnmodelled(iob, req.r, "data communications (2609 CMN01/CMN02 SNA/BSC/async lines)");
        // SVC 45, Diskette Data Compression (SA21-9436 3-138): the whole SVC
        // IS the hardware data-compression assist, a genuinely unmodelled
        // host device; the machine's own default arm for an unconfigured
        // device class writes the same 0x20 this decline does.
        case 0x45: return declineUnmodelled(iob, req.r, "diskette data compression assist (5360 hardware feature)");
        case 0x46: return tape.execute(iob, req.q);
        case 0x48: return declineNoDataStorageController(iob);
        default:
            // Not in the UDT either: a genuine hole rather than an absent
            // device, so this one still stops the machine.
            trace_.diskIo("SVC {:02X}: no device class routed, and the volume's UDT does not declare one either", req.r);
            m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
            return false;
    }
}

// Answer for a device the machine has but this emulator does not model.  It
// ANSWERS rather than refusing: a device that cannot service a request posts
// a status, it does not halt the processor.  Phase 1 polls the diskette
// during every disk IPL and carries on when the drive is empty.  Every one
// is traced loudly and counted.
bool DeviceSet::declineUnmodelled(int iob, uint8_t r, const char* what)
{
    unmodelledRequests_++;
    int command = m_.readByte(iob + IoBlock::kOffCommand);
    int modifier = m_.readByte(iob + IoBlock::kOffCommandModifier);
    m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
    IoBlock::complete(m_, iob, kDeviceNotAvailable);
    trace_.diskIo("SVC {:02X} iob={:06X} cmd={:02X}/{:02X}: {} is declared by the UDT but NOT MODELLED - answered "
                  "unavailable (iob+{} = {:02X}) rather than refused, so the guest decides. Request {} of its kind. "
                  "docs/s36/device-io.md",
                  r, iob, command, modifier, what, kOffDeviceStatus, kNotInThisConfiguration, unmodelledRequests_);
    return true;
}

// SVC 48, DSC I/O (SA21-9436 3-140): "starts or stops SMF in the data
// storage controller, reads the SMF counters ... and sends information to
// the data storage controller during an initial program load."  The data
// storage controller is 5360/5362 hardware; the Advanced/36 has none, and
// its own supervisor stores the "not in this configuration" byte and
// completes without entering a device handler.  The faithful behaviour is
// to write the identical byte and complete.
bool DeviceSet::declineNoDataStorageController(int iob)
{
    dataStorageControllerRequests_++;
    int command = m_.readByte(iob + IoBlock::kOffCommand);
    int modifier = m_.readByte(iob + IoBlock::kOffCommandModifier);
    m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
    IoBlock::complete(m_, iob, kDeviceNotAvailable);
    trace_.diskIo("SVC 48 iob={:06X} cmd={:02X}/{:02X}: DSC I/O - the A/36 has no data storage controller. nusvc's own "
                  "R=48 arm (c18e3d74) writes device status {:02X} = not-in-this-configuration and completes with no "
                  "device call; this matches it byte for byte. Request {} of its kind. docs/s36/svc-reference.md (R=48)",
                  iob, command, modifier, kNotInThisConfiguration, dataStorageControllerRequests_);
    return true;
}

bool DeviceSet::workStationIoch(int iob, uint8_t r)
{
    // XR1 may point at an IOB or, when it carries EBCDIC "TU", at a unit
    // block.  Three routines branch on exactly this and it changes which
    // bytes mean what, so it is the first thing to establish.
    bool xr1IsUnitBlock = WorkStationIob::isUnitBlock(m_, iob);
    int unitBlock = WorkStationIob::resolveUnitBlock(m_, iob);

    int cmd = WorkStationIob::command(m_, iob);
    int cls = WorkStationIob::classByte(m_, iob);
    int unit = WorkStationIob::unitAddress(m_, iob);

    trace_.ws("SVC {:02X} {}={:06X} command={:02X} ({}) class={:02X} unit={:02X} (port {} address {}) unit-block={:06X}", r,
              xr1IsUnitBlock ? "tub" : "iob", iob, cmd, WorkStationIob::commandName(cmd), cls, unit,
              WorkStationController::portOf(unit), WorkStationController::addressOf(unit), unitBlock);

    // Finding the controller comes before anything else in both SVC arms,
    // and a miss is error 58 rather than a completion code.
    if (workStations_.count() == 0) {
        trace_.ws("  no work station controller configured - nusvc reports error 58 here");
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    // SVC 43 alone looks at the command byte before entering the request
    // path; SVC 42 goes straight to the print request path.
    if (r == 0x43) {
        // Commands 81/82 are CONTROLLER configuration operations.  Their IOB
        // is the published bootstrap/system-console unit block, while their
        // records describe zero or more configured stations; the machine
        // never associates that issuer block with record/unit 0.
        if (cmd == WorkStationIob::kCmdReadCurrentConfiguration) return readCurrentConfiguration(iob);
        if (cmd == WorkStationIob::kCmdConfigureNewWorkStations) return configureNewWorkStations(iob);
    }

    // Ordinary station I/O does carry a station's own unit block: SSP stores
    // the grid address at TU+12 and the supervisor uses the same byte to find
    // the controller, so this is the first point where the association is
    // established by a guest request.
    if (unitBlock != 0) recordUnitBlock(unit, unitBlock);

    return request(iob, r, cmd, unitBlock);
}

void DeviceSet::recordUnitBlock(int unitAddress, int unitBlock)
{
    WorkStationSlot* slot = workStations_.find(unitAddress);
    if (slot == nullptr) return;
    if (slot->unitBlockAddress == unitBlock) return;
    if (slot->unitBlockAddress != 0 && slot->unitBlockAddress != unitBlock)
        trace_.ws("  unit {:02X}: unit block MOVED {:06X} -> {:06X} - a rebuilt TUB, or two blocks claiming one station",
                  unitAddress, slot->unitBlockAddress, unitBlock);
    else
        trace_.ws("  unit {:02X}: unit block {:06X} recorded for station {}.{}", unitAddress, unitBlock, slot->port,
                  slot->address);
    slot->unitBlockAddress = unitBlock;

    for (auto& item : stations_.items()) {
        VirtualWorkstation* ws = *item.second;
        if (ws->port() == slot->port && ws->address() == slot->address) ws->tubAddress = unitBlock;
    }
    for (auto& item : printers_.items()) {
        VirtualPrinter* pr = *item.second;
        if (pr->port() == slot->port && pr->address() == slot->address) pr->pubAddress = unitBlock;
    }
}

// The print request path's contribution, a validation and a hand-off: it
// checks the class byte's high nibble, queues the element and returns.  The
// command itself is decoded later, asynchronously, by the action.
bool DeviceSet::request(int iob, uint8_t r, int cmd, int unitBlock)
{
    if (!WorkStationIob::isWorkStationClass(m_, iob)) {
        trace_.ws("  class byte {:02X} is not 0xCn - wsprintr rejects this through wsifptr -> nuerr 58 without looking at "
                  "the command",
                  WorkStationIob::classByte(m_, iob));
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (unitBlock != 0 && !UnitBlock::isWellFormed(m_, unitBlock))
        trace_.ws("  unit block {:06X} has no \"TU\" eyecatcher - SSP builds these in MSIPL phase 2 (#SVTUB); the emulator "
                  "only reads them",
                  unitBlock);
    else if (unitBlock != 0)
        trace_.ws("  unit block {:06X}: signed-on={} (SSP sets +7 bit 80; no SLIC routine does)", unitBlock,
                  UnitBlock::isSignedOn(m_, unitBlock) ? "True" : "False");

    // The controller looks at the UNIT ADDRESS before the command byte: FF
    // goes to the all-stations invite and never reaches the command switch.
    int unitAddress = WorkStationIob::unitAddress(m_, iob);
    if (unitAddress == WorkStationActions::kInviteUnitAddress) return invite(iob);

    WorkStationSlot* slot = workStations_.find(unitAddress);
    if (slot == nullptr) {
        trace_.ws("  unit {:02X} is not attached to this controller", unitAddress);
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    if (slot->displayDetached) {
        // The active object was cleared by the session's end, so the command
        // is answered with the not-attached control field instead of being
        // dispatched, until the next action-0 bind re-activates its display.
        trace_.ws("  unit {:02X}: display detached (powered off) - wscmd rejects command {:02X} with +13=28 +17/18=02 03 "
                  "(device not attached), completion 41",
                  unitAddress, WorkStationIob::command(m_, iob));
        writeDeviceNotAttached(iob);
        return true;
    }

    // SA21-9436 calls 42 "printer requests" and 43 "display station
    // requests", and that is the CALLER convention, not the dispatch.  A
    // mismatch is worth saying out loud and is not worth refusing: the unit
    // address in the IOB is what selects the device, on the machine and here.
    if ((r == 0x42) != slot->isPrinter)
        trace_.ws("  note: SVC {:02X} is SA21-9436's {} entry point but unit {:02X} is a {}. The unit address selects the "
                  "device, not the SVC number - `wsdvc`'s default arm is `wsprintr`, which is where `42` goes directly",
                  r, r == 0x42 ? "printer" : "display station", WorkStationIob::unitAddress(m_, iob),
                  slot->isPrinter ? "printer" : "display");

    WorkStationActions::Arm arm;
    if (!WorkStationActions::tryLookup(cmd, arm)) {
        // The controller's own answer to a byte it does not know is a log
        // entry and no device call at all.  Refusing here is the same
        // outcome, said out loud.
        trace_.ws("  command {:02X} ({}) REFUSED: it is not one of NuActiveCtl::wscmd's decoded commands ({}). wscmd sends an "
                  "unknown byte to NuEmul::vlogNote (c18bea24) and calls no device method. Note that SA21-9436 chapter 11's "
                  "`21` Output Data and `41` Get Printer Status are NOT in the decoded set - the operations are there as `27` "
                  "and `47` - and nothing here maps one onto the other. docs/s36/workstation-ioch.md",
                  cmd, WorkStationIob::commandName(cmd), WorkStationActions::knownCommands());
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    // The two tables are independent decodes, so the model says when they
    // disagree rather than trusting either alone.
    WsAction viaCode = WorkStationActions::actionOf(arm.code);
    trace_.ws("  command {:02X} -> wscmd {} -> action code {} -> {}", cmd, arm.site, arm.code, wsActionName(viaCode));
    if (viaCode != arm.action) {
        trace_.ws("  the two decoded tables disagree: wscmd says {}, executeRequest's jump table says {} - refusing",
                  wsActionName(arm.action), wsActionName(viaCode));
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    bool phaseResult = false;
    switch (arm.action) {
        case WsAction::Put:
            if (!beginOutputRequest(iob, false, phaseResult)) return phaseResult;
            return outputData(iob, *slot, false);

        case WsAction::PutWithInvite:
            if (!beginOutputRequest(iob, true, phaseResult)) return phaseResult;
            return outputData(iob, *slot, true);

        case WsAction::Clear: return clear(iob, *slot);
        case WsAction::CancelInvite: return cancelInvite(iob, *slot);
        case WsAction::ReadInputFields: return readInputFields(iob, *slot);
        case WsAction::SaveScreen: return saveScreen(iob, *slot);
        case WsAction::RestoreScreen: return restoreScreen(iob, *slot);

        default:
            // The action is decoded; what a host seam can do about it is not
            // the same question.  A display's message-waiting light and a
            // printer's status need state this model does not hold, and
            // inventing an answer would look like a device that did
            // something it did not.
            trace_.ws("  action {} is decoded but not implemented: no backend operation corresponds to it here, and a "
                      "synthesised answer would look like a device that did something it did not. "
                      "docs/s36/workstation-ioch.md",
                      wsActionName(arm.action));
            IoBlock::complete(m_, iob, 4);
            return false;
    }
}

// The output-open path accepts an initial output action only when the full
// class byte is C0.  It masks the command with 0x7F and handles both 27 and
// A7 in the same outer arm, but the C0 -> C1 store is guarded by the exact
// A7 comparison; an ordinary 27 remains C0.  A later C1 call is therefore
// an A7 response/completion phase, not a second device action.
bool DeviceSet::beginOutputRequest(int iob, bool withInvite, bool& phaseResult)
{
    int cls = WorkStationIob::classByte(m_, iob);
    phaseResult = false;
    if (cls == WorkStationIob::kClassWorkStation) {
        // Around the class store the output-open path masks three bits out
        // of the unit block: +6 &= ~0x04, +6 &= ~0x80 and, immediately after
        // the C1 store, +9 &= ~0x80.  The last one is what takes away the
        // +9 bit the display manager sets.
        int ub = iob;
        m_.writeByte(ub + 6, static_cast<uint8_t>(m_.readByte(ub + 6) & ~0x04));
        m_.writeByte(ub + 6, static_cast<uint8_t>(m_.readByte(ub + 6) & ~0x80));
        m_.writeByte(ub + 9, static_cast<uint8_t>(m_.readByte(ub + 9) & ~0x80));
        if (withInvite) {
            // Only A7 advances the class.  Controller ownership remains
            // visible until the physical AID response completes it, and the
            // new action's status area is cleared here: until this next A7
            // begins, the preceding accepted-input control field remains
            // current and SSP may import it on more than one C1 pass.
            inputResponseStatus_.erase(WorkStationIob::unitAddress(m_, iob));
            m_.writeByte(iob + WorkStationIob::kOffClass, static_cast<uint8_t>(WorkStationIob::kClassWorkStation + 1));
            trace_.ws("  wsopend A7 initial phase: class C0 -> C1");
        } else {
            // Promoting a 27 made the display manager dispatch an output
            // completion as if it carried an AID.
            trace_.ws("  wsopend 27 completion: class remains C0");
        }
        return true;
    }

    if (cls == WorkStationIob::kClassWorkStation + 1) {
        int unit = WorkStationIob::unitAddress(m_, iob);
        const std::vector<uint8_t>* responseStatus = inputResponseStatus_.find(unit);
        if (responseStatus != nullptr) {
            applyC1Response(iob, *responseStatus);
            inputResponseStatus_.erase(unit);
            trace_.ws("  class C1 response completion -> wssmfrt; wscmdend/wsavstat imported and consumed one input WSCF");
            IoBlock::complete(m_, iob, 0);
            phaseResult = true;
            return false;
        }
        uint8_t oldCompletion = m_.readByte(iob + Ecm::kOffCompletion);
        m_.writeByte(iob + Ecm::kOffCompletion, Ecm::arm(oldCompletion));
        pendingC1Completions_.set(iob, unit);
        // At the local-controller seam a keyboard-restoring PUT and the
        // following controller wait are separate operations.  TN5250 needs
        // the equivalent RFC 1205 Invite record: a strict client will display
        // the restored keyboard but will not transmit an AID until the host
        // has reversed the flow direction.  The research terminal used to
        // accept that AID without an Invite, masking this missing adapter
        // transition.  This does not complete the action or invent input; the
        // retained C1 IOB still waits for a terminal-produced response WSCF.
        WorkStationSlot* slot = workStations_.find(unit);
        if (slot != nullptr && !slot->isPrinter) slot->backend()->setInputEnabled(true);
        trace_.ws("  class C1 response wait PENDING: no return WSCF for unit {:02X}; RFC Invite armed, retained IOB "
                  "{:06X}/SVC-43 ACE until a real terminal response",
                  unit, iob);
        phaseResult = true;
        return false;
    }

    trace_.ws("  output command in unsupported controller phase {:02X}; wsopend starts an action only at exact class C0", cls);
    IoBlock::complete(m_, iob, 4);
    return false;
}

// Unit address FF: an all-stations request that conditions every display
// slot to talk; a printer answers rather than volunteers and has no invite.
// The controller's invite builds per-device actions; when none produces
// status the controller action is saved and only its transient fields are
// cleared, so the original guest IOB and element stay owned.
bool DeviceSet::invite(int iob)
{
    if (pendingControllerInvites_.count(iob) != 0) {
        trace_.ws("  unit address FF Invite IOB {:06X} remains owned by NuActiveCtl::invit", iob);
        return true;
    }

    int invited = 0;
    for (WorkStationSlot* s : workStations_.slots()) {
        if (s->isPrinter) continue;
        s->backend()->setInputEnabled(true);
        invited++;
    }
    trace_.ws("  unit address FF -> NuActiveCtl::invit (dcdwscf c18be7c8): {} display slot(s) invited; IOB {:06X} and its ACE "
              "remain owned by the controller",
              invited, iob);
    pendingControllerInvites_.insert(iob);
    return true;
}

// Command 40, action code 12, the device's clear.  On a printer that is
// Clear Printer, and the honest host action is to end the job: RFC 2877
// section 10.3's null print record is what tells a client there is no more
// data, and a job that is never ended never prints.  On a display it is a
// screen operation this seam does not have, so it is refused.
bool DeviceSet::clear(int iob, WorkStationSlot& slot)
{
    if (!slot.isPrinter) {
        trace_.ws("  Clear on a display slot: Nudev5250::clear is a screen operation and IWorkStationBackend has no equivalent "
                  "- refusing rather than guessing");
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    bool ok = slot.printer()->endJob();
    trace_.ws("  Clear Printer -> {} end of job: {}", slot.backendName(),
              ok ? "null print record sent" : "not sent - no session attached");
    IoBlock::complete(m_, iob, ok ? 0 : 4);
    return ok;
}

// Command C3, action code 14: the exact inverse of the invite on the one
// slot the IOB names.
bool DeviceSet::cancelInvite(int iob, WorkStationSlot& slot)
{
    if (slot.isPrinter) {
        trace_.ws("  Cancel Invite on a printer slot: a printer is never invited (IPrinterBackend has no SetInputEnabled) - "
                  "refusing");
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    slot.backend()->setInputEnabled(false);
    trace_.ws("  Cancel Invite -> {} will not report input", slot.backendName());
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Read Input Fields (22/32/42).  The device contract parses AID, cursor and
// orders from retained device input, compacts field data, updates the
// action status, and only then completes the guest read; the controller
// retains the request until a real terminal record has been parsed.
bool DeviceSet::readInputFields(int iob, WorkStationSlot& slot)
{
    if (slot.isPrinter) {
        trace_.ws("  Read Input Fields on a printer slot: a printer has no input to read (IPrinterBackend has no TryTakeInput) "
                  "- refusing");
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (pendingInputReads_.contains(iob)) {
        trace_.ws("  Read Input Fields: IOB {:06X} is already owned by the controller", iob);
        return true;
    }

    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    std::vector<int> destination;
    int capacity = WorkStationIob::length(m_, iob);
    if (!captureBuffer(bufferField, capacity, true, destination)) {
        trace_.ws("  Read Input Fields: issue-time buffer {:06X}+{} is invalid", bufferField, capacity);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    int stagingBlockDisplacement = -1;
    const int* stagingDestination = nullptr;
    if (WorkStationIob::command(m_, iob) == 0x42 && (stagingDestination = inputStagingPages_.find(slot.unitAddress())) != nullptr &&
        *stagingDestination >= 0) {
        // The IOB names the display manager's final requester destination.
        // Its return copy reads from the A7 driver's mapped work page at the
        // allocation selected by TUB+0x43..0x45, a three-byte translated
        // pointer loaded before its SVC 2F.  Retaining only the page silently
        // worked for one allocation and lost the other.
        int unitBlock = WorkStationIob::resolveUnitBlock(m_, iob);
        int workspace = unitBlock > 0 ? m_.readAddr24(unitBlock + UnitBlock::kOffInputWorkspacePointer) : 0;
        if ((workspace & IoBlock::kDataBufferTranslated) != 0) {
            // The deferred copy addresses the whole work-space BLOCK, not
            // the A7 output buffer's captured real page.  Those are distinct
            // address domains: in the failing three-session case the TUB
            // selected logical workspace page 2 while the retained PUT page
            // was physical workspace page 1.  Writing the reply to that PUT
            // page corrupted another session's workspace before the correct
            // deferred replay occurred.  The IOB's issue-time destination
            // remains the immediate controller destination; only the replay
            // uses this logical block displacement.
            stagingBlockDisplacement = workspace & ~IoBlock::kDataBufferTranslated;
        }
        trace_.ws("  Read Input Fields: guest TUB {:06X}+0x43 workspace {:06X} retains logical block displacement {:04X}; "
                  "A7 PUT page {:06X} is provenance only and is not an input destination",
                  unitBlock, workspace, stagingBlockDisplacement, *stagingDestination);
    }

    // The device-path address is resolved into the element at SVC issue
    // time and the action retains the pointer and capacity, so the guest is
    // free to reuse the TU/IOB fields while the action is pending.
    PendingInputRead pending;
    pending.slot = &slot;
    pending.command = static_cast<uint8_t>(WorkStationIob::command(m_, iob));
    pending.bufferField = bufferField;
    pending.destination = destination;
    pending.capacity = capacity;
    pending.stagingBlockDisplacement = stagingBlockDisplacement;
    pendingInputReads_.set(iob, std::move(pending));
    trace_.ws("  Read Input Fields PENDING: controller retained IOB {:06X} for unit {:02X}, issue-time buffer {:06X} -> "
              "{:06X}, capacity {}; no ECM completion is posted until a real terminal record has been parsed. Raw RFC-1205 "
              "pass-through is forbidden: NuDsp5250 strips cursor/AID for command 42 and reports AID through controller "
              "status.",
              iob, slot.unitAddress(), bufferField, destination.empty() ? 0 : destination[0], capacity);
    return true;
}

bool DeviceSet::saveScreen(int iob, WorkStationSlot& slot)
{
    if (slot.isPrinter) {
        trace_.ws("  Save Screen on a printer slot is invalid");
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    if (pendingScreenSaves_.contains(iob)) return true;

    uint8_t mode = 0;
    for (auto& item : stations_.items()) {
        VirtualWorkstation* station = *item.second;
        if (station->port() == slot.port && station->address() == slot.address) {
            mode = station->savedReadMode();
            break;
        }
    }

    // The action retains a native pointer and length, not the original
    // task-relative 24-bit IOB field: resolve it while the submitting task's
    // ATRs are current, since the asynchronous completion may run under a
    // different task.
    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    std::vector<int> destination;
    int capacity = WorkStationIob::length(m_, iob);
    if (capacity < 24 || !captureBuffer(bufferField, capacity, true, destination)) {
        trace_.ws("  Save Screen IOB {:06X}: invalid action buffer {:06X}+{}", iob, bufferField, capacity);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (!slot.backend()->beginSaveScreen()) {
        trace_.ws("  Save Screen: RFC-1205 request was not sent to {}", slot.backendName());
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    // The guest commonly reuses a TU/IOB whose preceding action left ECM+6
    // at 0x50.  A NEW asynchronous action has been accepted, so it is no
    // longer complete: the post arm reduces the byte to (TU[6] & ~0x02) &
    // 0x0F, the whole high nibble, which is still safe for the one bit the
    // event wait reads.
    uint8_t oldCompletion = m_.readByte(iob + Ecm::kOffCompletion);
    uint8_t armedCompletion = Ecm::arm(oldCompletion);
    m_.writeByte(iob + Ecm::kOffCompletion, armedCompletion);
    PendingScreenSave pending;
    pending.slot = &slot;
    pending.savedReadMode = mode;
    pending.destination = destination;
    pending.capacity = capacity;
    pendingScreenSaves_.set(iob, std::move(pending));
    trace_.ws("  Save Screen PENDING: ECM+6 {:02X}->{:02X}, controller retained IOB {:06X} for unit {:02X}; awaiting a real "
              "opcode-04/0412 terminal image, saved read mode {:02X}",
              oldCompletion, armedCompletion, iob, slot.unitAddress(), mode);
    return true;
}

bool DeviceSet::restoreScreen(int iob, WorkStationSlot& slot)
{
    if (slot.isPrinter) {
        trace_.ws("  Restore Screen on a printer slot is invalid");
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    std::vector<int> source;
    int supplied = WorkStationIob::length(m_, iob);
    if (supplied < 24 || !captureBuffer(bufferField, supplied, false, source)) {
        trace_.ws("  Restore Screen IOB {:06X}: invalid save-record buffer/length", iob);
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    std::vector<uint8_t> saved(static_cast<std::size_t>(supplied));
    readCaptured(source, 0, saved.data(), 0, supplied);
    SavedScreenRecord record;
    if (!decodeSavedScreenRecord(saved.data(), 0, supplied, record)) {
        int allocation = supplied >= 4 ? (saved[2] << 8) | saved[3] : 0;
        int imageLength = supplied >= 22 ? (saved[20] << 8) | saved[21] : 0;
        uint8_t mode = supplied >= 19 ? saved[18] : 0;
        trace_.ws("  Restore Screen IOB {:06X}: malformed SSP save record (allocation {}, image {}, mode {:02X})", iob,
                  allocation, imageLength, mode);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (!slot.backend()->restoreScreen(saved.data(), record.bodyOffset, record.bodyLength) ||
        (record.readMode != 0 && !slot.backend()->resumeSavedReadMode(record.readMode))) {
        trace_.ws("  Restore Screen IOB {:06X}: terminal restore/read-mode request failed", iob);
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    IoBlock::complete(m_, iob, 0);
    trace_.ws("  Restore Screen COMPLETE: IOB {:06X}, sent terminal-returned 0412 image ({} byte(s)), restored mode {:02X}", iob,
              record.bodyLength - 2, record.readMode);
    return true;
}

std::string DeviceSet::hexPreview(const std::vector<uint8_t>& d, int len, int max)
{
    int n = std::min(len, std::min(static_cast<int>(d.size()), max));
    std::string s;
    for (int i = 0; i < n; i++) s += fmt::format("{:02X} ", d[static_cast<std::size_t>(i)]);
    if (len > n) s += "...";
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// EBCDIC (cp037-ish) preview of the printable range; non-printables -> '.'.
std::string DeviceSet::ebcdicPreview(const std::vector<uint8_t>& d, int len, int max)
{
    const char* lo = "abcdefghi";
    const char* jr = "jklmnopqr";
    const char* sz = "stuvwxyz";
    const char* LO = "ABCDEFGHI";
    const char* JR = "JKLMNOPQR";
    const char* SZ = "STUVWXYZ";
    int n = std::min(len, std::min(static_cast<int>(d.size()), max));
    std::string s;
    for (int i = 0; i < n; i++) {
        uint8_t b = d[static_cast<std::size_t>(i)];
        char c;
        if (b == 0x40) c = ' ';
        else if (b >= 0x81 && b <= 0x89) c = lo[b - 0x81];
        else if (b >= 0x91 && b <= 0x99) c = jr[b - 0x91];
        else if (b >= 0xA2 && b <= 0xA9) c = sz[b - 0xA2];
        else if (b >= 0xC1 && b <= 0xC9) c = LO[b - 0xC1];
        else if (b >= 0xD1 && b <= 0xD9) c = JR[b - 0xD1];
        else if (b >= 0xE2 && b <= 0xE9) c = SZ[b - 0xE2];
        else if (b >= 0xF0 && b <= 0xF9) c = static_cast<char>('0' + (b - 0xF0));
        else if (b == 0x4B) c = '.';
        else if (b == 0x6B) c = ',';
        else if (b == 0x7D) c = '\'';
        else if (b == 0x5B) c = '$';
        else if (b == 0x50) c = '&';
        else if (b == 0x60) c = '-';
        else c = '.';
        s += c;
    }
    if (len > n) s += "...";
    return s;
}

// 0x27 put and 0xA7 putWithInvite.  The bytes are passed by reference and
// not interpreted in transit (SA21-9436 5-50); +0x0D..0x0F is the data
// address for every device SVC; +0x10 is its length, INFERRED from the two
// configuration commands whose buffer handling is decoded.  A zero length
// is refused rather than sent as nothing.
bool DeviceSet::outputData(int iob, WorkStationSlot& slot, bool withInvite)
{
    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    int length = WorkStationIob::length(m_, iob);

    if (length == 0) {
        trace_.ws("  Output Data with length 0 at +0x10 - refusing rather than sending nothing. +0x10 as the data stream length "
                  "is INFERRED from rdcnf and cnfws, which are the only two commands whose buffer handling is decoded. "
                  "docs/s36/workstation-ioch.md");
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    std::vector<int> source;
    if (!captureBuffer(bufferField, length, false, source)) {
        trace_.ws("  data stream {:06X} + {} is not fully addressable", bufferField, length);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    std::vector<uint8_t> data(static_cast<std::size_t>(length));
    readCaptured(source, 0, data.data(), 0, length);
    int src = source.empty() ? 0 : source[0];

    // Output provenance: WHO wrote this, WHERE from, and WHAT, so the
    // producing task, member and statement can be found.
    if (trace_.outputOn()) {
        int iar = m_.msp.iar;
        int cmd = WorkStationIob::command(m_, iob);
        int cls = WorkStationIob::classByte(m_, iob);
        std::string producer = m_.stateDescriber ? m_.stateDescriber() : std::string("member unavailable");
        trace_.output("WS write: {} len={} guest={:06X}  <- IAR {:04X}; {}; iob={:06X} cmd={:02X} class={:02X} unit={:02X}",
                      withInvite ? "PUT+INVITE" : "PUT", length, src, iar, producer, iob, cmd, cls, slot.unitAddress());
        trace_.output("  hex : {}", hexPreview(data, length, 96));
        trace_.output("  text: {}", ebcdicPreview(data, length, 96));
        bool has04 = std::find(data.begin(), data.end(), static_cast<uint8_t>(0x04)) != data.end();
        bool has11 = std::find(data.begin(), data.end(), static_cast<uint8_t>(0x11)) != data.end();
        bool looks5250 = length > 1 && (data[0] == 0x04 || (has04 && has11));
        trace_.output("  form: {}", looks5250 ? "has 5250 orders (ESC 04 / WTD-SBA 11) - renderable"
                                              : "inner WSDM payload, not a standalone 5250 body; SLIC Display::writeOnlyMessage "
                                                "is the candidate WTD/WCC/SBA renderer");
    }

    // The one place the two seams part: a display gets an RFC 1205 display
    // record and a printer an RFC 2877 section 10 print record.
    bool sent;
    SavedScreenRecord saved;
    bool putCarriesSavedScreen = !slot.isPrinter && !withInvite &&
                                  decodeSavedScreenRecord(data.data(), 0, length, saved);
    if (slot.isPrinter) {
        sent = slot.printer()->sendDataStream(data.data(), 0, length);
    } else if (putCarriesSavedScreen) {
        sent = slot.backend()->restoreScreen(data.data(), saved.bodyOffset, saved.bodyLength) &&
               (saved.readMode == 0 || slot.backend()->resumeSavedReadMode(saved.readMode));
        trace_.ws("  put carried an SSP saved-screen envelope: sent embedded {}-byte 0412 body as RFC 1205 Restore Screen, "
                  "restored mode {:02X}",
                  saved.bodyLength, saved.readMode);
    } else {
        sent = withInvite ? slot.backend()->putWithInvite(data.data(), 0, length) : slot.backend()->put(data.data(), 0, length);
    }

    trace_.ws("  {}: {} bytes from guest {:06X} to {} {}", withInvite ? "put with invite" : "put", length, src,
              slot.isPrinter ? "printer" : "station", slot.backendName());

    if (!sent)
        trace_.ws("  device operation was not accepted by backend; guest completion is still immediate in this increment");

    if (withInvite && !slot.isPrinter && sent) {
        if ((bufferField & 0x800000) != 0) {
            // The architecture's translated pages are 2 KB.  The panel can
            // start partway into its work page while its input staging
            // begins at that same page's first byte; both values come from
            // the guest request.
            int page = bufferField & ~0x7FF;
            int realPage = src - (bufferField & 0x7FF);
            inputStagingPages_.set(slot.unitAddress(), realPage);
            trace_.ws("  PUT-with-invite input staging page retained from guest buffer: {:06X} -> real {:06X}", page, realPage);
        }
        // A reused TUB/IOB commonly still has 0x40 from the previous action.
        // A7 is now outstanding, so arm the mask: the whole high nibble
        // goes, not just the complete bit.
        uint8_t oldCompletion = m_.readByte(iob + Ecm::kOffCompletion);
        uint8_t armedCompletion = Ecm::arm(oldCompletion);
        m_.writeByte(iob + Ecm::kOffCompletion, armedCompletion);
        pendingPutWithInvites_.set(iob, &slot);
        trace_.ws("  PUT-with-invite PENDING: ECM+6 {:02X}->{:02X}, retained IOB {:06X}/SVC-43 ACE until a real terminal "
                  "response; no output completion is posted at send time",
                  oldCompletion, armedCompletion, iob);
        return true;
    }

    IoBlock::complete(m_, iob, 0);
    return true;
}

void DeviceSet::applyC1Response(int iob, const std::vector<uint8_t>& responseStatus)
{
    int tub = WorkStationIob::resolveUnitBlock(m_, iob);
    if (tub <= 0 || m_.readHalf(tub) != WorkStationIob::kUnitBlockEyecatcher || responseStatus.size() != 12) return;
    for (int n = 0; n < 12; n++) m_.writeByte(tub + 0x13 + n, responseStatus[static_cast<std::size_t>(n)]);
    m_.writeByte(tub + 0x8D, responseStatus[1]);
}

bool DeviceSet::readCurrentConfiguration(int iob)
{
    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    int length = WorkStationIob::length(m_, iob);

    int capacity = length > 0 ? (length - 1) / WorkStationSlot::kConfigurationRecordBytes : 0;
    int outputLength = capacity * WorkStationSlot::kConfigurationRecordBytes + 1;
    std::vector<int> destination;
    if (!captureBuffer(bufferField, outputLength, true, destination)) {
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    trace_.ws("  Read Current Configuration into {:06X}, {} byte(s) = room for {} record(s)", bufferField, length, capacity);
    std::vector<uint8_t> output(static_cast<std::size_t>(outputLength), 0);
    int written = workStations_.readCurrentConfiguration(output, length, autoConfigEnabled);
    writeCaptured(destination, 0, output.data(), 0, written);
    trace_.ws("  {} byte(s) written, list terminated with FF", written);

    IoBlock::complete(m_, iob, 0);
    return true;
}

bool DeviceSet::configureNewWorkStations(int iob)
{
    int bufferField = m_.readAddr24(iob + WorkStationIob::kOffDataBuffer);
    int length = WorkStationIob::length(m_, iob);

    std::vector<int> source;
    if (!captureBuffer(bufferField, length, false, source)) {
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    trace_.ws("  Configure New Work Stations from {:06X}, {} byte(s)", bufferField, length);
    std::vector<uint8_t> input(static_cast<std::size_t>(length), 0);
    readCaptured(source, 0, input.data(), 0, length);
    int reason = workStations_.configureNewWorkStations(input, length);
    if (reason != 0) {
        // The configuration error routine's guest-visible status fields, and
        // the end state its post leaves: completion bit 80 cleared, status
        // bit 01 set, ECM+6 = 41.  The caller owns the already-built element
        // and performs that post, so the synchronous representation here is
        // the exact end state of the decoded path, not a generic failure.
        m_.writeByte(iob + WorkStationIob::kOffConfigureError, static_cast<uint8_t>(reason));
        m_.writeByte(iob + 0x13, 0x28);
        m_.writeByte(iob + 0x17, 0x05);
        m_.writeByte(iob + 0x2A, 0x87);
        m_.writeByte(iob + 0x2C, static_cast<uint8_t>(m_.readByte(iob + 0x2C) | 0x01));
        trace_.ws("  rejected: cnerr reason {}; iob+13=28 +17=05 +18={:02X} +2A=87 +2C|=01; nupd/nupdpiob completion 41 and "
                  "nuset 2D",
                  reason, reason);
        IoBlock::complete(m_, iob, 1);
        // The controller rejected the command, but SVC 43 itself was
        // serviced: the error routine posts the caller's element and
        // returns.  A false result would mean "unimplemented SVC" to the CSP
        // and halt before the guest can inspect completion 41.
        return true;
    }

    IoBlock::complete(m_, iob, 0);
    return true;
}

// Same field and same flag as the disk's; the device-path rule resolves it
// for every device SVC, not only 40.
bool DeviceSet::captureBuffer(int bufferField, int length, bool forWrite, std::vector<int>& addresses)
{
    addresses.clear();
    if (length < 0) return false;
    std::vector<int> captured(static_cast<std::size_t>(length));
    bool translated = (bufferField & IoBlock::kDataBufferTranslated) != 0;
    for (int n = 0; n < length; n++) {
        int field = translated ? IoBlock::kDataBufferTranslated | ((bufferField + n) & 0xFFFF) : bufferField + n;
        int real;
        if (!m_.resolveGuest24(field, forWrite, real) || real < 0 || real >= m_.backingBytes()) {
            trace_.ws("  buffer {:06X}+{} is not mapped for {}", bufferField, n, forWrite ? "write" : "read");
            return false;
        }
        captured[static_cast<std::size_t>(n)] = real;
    }
    addresses = std::move(captured);
    if (translated && length != 0) {
        int boundaries = 1;
        for (int n = 1; n < length; n++)
            if (addresses[static_cast<std::size_t>(n)] != addresses[static_cast<std::size_t>(n) - 1] + 1) boundaries++;
        trace_.ws("  buffer {:06X}+{} captured through task translation -> real {:06X}, {} physical span(s)", bufferField, length,
                  addresses[0], boundaries);
    }
    return true;
}

void DeviceSet::readCaptured(const std::vector<int>& source, int sourceOffset, uint8_t* destination, int destinationOffset,
                             int length)
{
    for (int n = 0; n < length; n++)
        destination[destinationOffset + n] = m_.readByte(source[static_cast<std::size_t>(sourceOffset + n)]);
}

void DeviceSet::writeCaptured(const std::vector<int>& destination, int destinationOffset, const uint8_t* source,
                              int sourceOffset, int length)
{
    for (int n = 0; n < length; n++)
        m_.writeByte(destination[static_cast<std::size_t>(destinationOffset + n)], source[sourceOffset + n]);
}

int DeviceSet::readCapturedHalf(const std::vector<int>& source, int offset)
{
    return (m_.readByte(source[static_cast<std::size_t>(offset)]) << 8) | m_.readByte(source[static_cast<std::size_t>(offset) + 1]);
}

void DeviceSet::writeCapturedHalf(const std::vector<int>& destination, int offset, uint16_t value)
{
    m_.writeByte(destination[static_cast<std::size_t>(offset)], static_cast<uint8_t>(value >> 8));
    m_.writeByte(destination[static_cast<std::size_t>(offset) + 1], static_cast<uint8_t>(value));
}

bool DeviceSet::resolveBuffer(int bufferField, int& addr)
{
    if (!m_.resolveGuest24(bufferField, true, addr)) {
        trace_.ws("  buffer {:06X} is task-translated and its page is not mapped", bufferField);
        return false;
    }
    if ((bufferField & IoBlock::kDataBufferTranslated) != 0)
        trace_.ws("  buffer {:06X} is task-translated -> real {:06X}", bufferField, addr);
    return true;
}

std::string DeviceSet::commandName(int cmd) { return WorkStationIob::commandName(cmd); }

}  // namespace sim36::devices
