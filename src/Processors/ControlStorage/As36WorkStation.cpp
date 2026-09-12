// The Advanced/36 control storage processor: the work-station family.
//
// Retained device actions completed by a real terminal response, the unit
// block resolvers, the control-plane state of the M36 display transfer, the
// controller's response post, and the console producers the monitor's
// experiments drive.  Everything here reads and writes guest control blocks
// the guest itself built; nothing invents a block it does not have.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <fmt/format.h>

#include "Devices/UnitBlock.h"
#include "Devices/WorkStationController.h"
#include "Devices/WorkStationIob.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Storage/Ebcdic.h"

namespace sim36::processors::controlstorage {

int workStationInputFieldDataOffset(machine::MachineState& m, int taskBlock, bool wsuReturnCopy)
{
    if (!wsuReturnCopy || !TaskBlock::isTaskBlock(m, taskBlock)) return 0;

    constexpr int kWsuFieldDataEnd = 0x17;
    int workBase = m.readAddr24(taskBlock + TaskBlock::kOffWorkBase);
    if (workBase <= 0 || workBase + kWsuFieldDataEnd >= m.backingBytes()) return 0;

    uint8_t oneOriginEnd = m.readByte(workBase + kWsuFieldDataEnd);
    return oneOriginEnd == 0 ? 0 : oneOriginEnd - 1;
}

using devices::UnitBlock;
using devices::WorkStationIob;
using devices::WorkStationSlot;

// ---- retained device actions ---------------------------------------------------------

bool As36ControlStorageProcessor::observePendingDeviceInput()
{
    int iob;
    return devices_.tryFindPendingInput(iob) && pendingDeviceAces_.count(iob) != 0;
}

bool As36ControlStorageProcessor::deliverWorkStationInputStatus(int unitBlock)
{
    return devices_.tryDeliverInputStatus(unitBlock);
}

bool As36ControlStorageProcessor::completePendingWorkStationInput()
{
    int iob;
    if (!devices_.tryCompletePendingInput(iob)) return false;

    // A Read Input Fields result may hold its parsed field bytes for
    // re-delivery onto the work-space block's OWN resident frame.  Key it to
    // the read's owner task now; it is consumed when the block is mapped
    // (SVC 2F action 4).
    std::optional<devices::DeviceSet::DeferredWorkStationInput> deferred = devices_.takeDeferredWorkStationInput();
    return postRetainedWorkStationCompletion(iob, deferred ? &*deferred : nullptr, "work-station device response");
}

bool As36ControlStorageProcessor::failPendingWorkStationOperation(int unit, const std::string& call)
{
    int iob;
    std::string what;
    if (!devices_.tryFailPendingOperationForUnit(unit, iob, what)) return false;
    trace_.csp("{}: retained {} IOB {:06X} completed with device-not-operational status", call, what, iob);
    return postRetainedWorkStationCompletion(iob, nullptr, call);
}

bool As36ControlStorageProcessor::deactivateWorkStationUnit(int unit) { return devices_.deactivateUnit(unit); }

// Complete a retained element: post the device's completion code into the
// mask, park the element on the owner's complete queue and post the owner if
// it waits.  An owner that is running keeps the element on its complete
// queue for its own release or wait code; the element is never released by
// the controller.  Registers are reloaded only when the MSP is PARKED: at a
// preemption point a task is executing and the reload would rewind it.
bool As36ControlStorageProcessor::postRetainedWorkStationCompletion(
    int iob, const devices::DeviceSet::DeferredWorkStationInput* deferred, const std::string& call)
{
    auto it = pendingDeviceAces_.find(iob);
    if (it == pendingDeviceAces_.end()) {
        trace_.csp("work-station IOB {:06X} completed but its retained ACE is absent", iob);
        return false;
    }
    int ace = it->second;
    pendingDeviceAces_.erase(it);

    int indicators = Ecm::isComplete(m_, iob) ? m_.readByte(iob + Ecm::kOffCompletion) & 0x0F : 4;
    aces_.post(ace, indicators);
    int target = m_.readAddr24(ace + ActionControlElement::kOffTaskBlock);
    if (deferred != nullptr && TaskBlock::isTaskBlock(m_, target)) {
        deferredWsInput_[target] = *deferred;
        trace_.csp("work-station Read Input Fields: {} field byte(s) held for task block {:04X} at work-space block "
                   "displacement {:03X}, to be delivered onto the block's resident frame when #WDDG's SVC 2F action 4 "
                   "maps it (readInputFields staging coherence)",
                   deferred->bytes.size(), target, deferred->blockDisplacement);
    }
    if (TaskBlock::isTaskBlock(m_, target)) {
        bool posted = completeToTask(ace, static_cast<uint8_t>(indicators), call);
        if (!posted)
            trace_.csp("work-station device response: task block {:04X} is not in an event wait; the completed element "
                       "stays on its complete queue for the task's own release/wait code (nupo0024 nuqstd, nupotkck "
                       "c18b5d50)",
                       target);
        if (posted && msp_ != nullptr && msp_->stopped()) {
            restoreRegisters(currentRequestBlock_);
            enableDispatching("work-station device response", "retained SVC-43 completion");
        }
        return posted;
    }

    aces_.release(ace);
    return false;
}

// ---- unit block resolution ---------------------------------------------------------------

// The block's own +12 unit field links it to a station; the scan is bounded
// to the system queue space and matches the full terminal signature
// (+5=80, +6=40, +10=C0) to avoid a stray eyecatcher.
int As36ControlStorageProcessor::resolveTubByUnit(int unitAddress)
{
    constexpr int kTerminalBytes = 192;
    for (int a = kSystemQueueSpace; a < heap_.high() - kTerminalBytes; a++) {
        if (m_.readHalf(a) != WorkStationIob::kUnitBlockEyecatcher) continue;
        if (m_.readByte(a + 5) != 0x80 || m_.readByte(a + 6) != 0x40 ||
            (m_.readByte(a + WorkStationIob::kOffClass) & WorkStationIob::kClassMask) != WorkStationIob::kClassWorkStation)
            continue;
        if (m_.readByte(a + 12) == (unitAddress & 0xFF)) return a;
    }
    return 0;
}

// Only among guest-published controller units: a configured block has the
// W-id/OC association (its +0x53 names an "OC" block whose key equals
// +0x4E); the bootstrap console block has neither.
int As36ControlStorageProcessor::resolveConfiguredTubByUnit(int unitAddress)
{
    int a = m_.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
    std::set<int> seen;
    for (int guard = 0; a != 0 && guard++ < 4096 && seen.insert(a).second;) {
        if (a < 0 || a + 0x56 >= m_.backingBytes()) return 0;
        int next = m_.readAddr24(a + 0x4B);
        if (m_.readHalf(a) != WorkStationIob::kUnitBlockEyecatcher) {
            a = next;
            continue;
        }
        if (m_.readByte(a + 5) != 0x80 || m_.readByte(a + 6) != 0x40 ||
            (m_.readByte(a + WorkStationIob::kOffClass) & WorkStationIob::kClassMask) != WorkStationIob::kClassWorkStation ||
            m_.readByte(a + 12) != (unitAddress & 0xFF)) {
            a = next;
            continue;
        }
        int oc = m_.readAddr24(a + 0x53);
        uint16_t key = m_.readHalf(a + 0x4E);
        if (key != 0 && oc != 0 && m_.readHalf(oc) == 0xD6C3 && m_.readHalf(oc + 2) == key) return a;
        a = next;
    }
    return 0;
}

// The controller's own selection: the first eligible terminal or printer
// block on the shared queue whose +0x0C is the requested unit.  Duplicate
// unit addresses are observable rather than silently resolved.
int As36ControlStorageProcessor::resolveWsEntryTubByUnit(int unitAddress)
{
    int at = m_.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
    std::set<int> seen;
    for (int guard = 0; at != 0 && guard++ < 4096 && seen.insert(at).second;) {
        if (at < 0 || at + 0x4E >= m_.backingBytes()) return 0;
        uint16_t eye = m_.readHalf(at);
        uint8_t cls = m_.readByte(at + WorkStationIob::kOffClass);
        if (m_.readByte(at + WorkStationIob::kOffUnitAddress) == (unitAddress & 0xFF) && (cls & 0x40) != 0 &&
            cls != WorkStationIob::kClassAlternate && (eye == WorkStationIob::kUnitBlockEyecatcher || eye == 0xD7E4))
            return at;
        at = m_.readAddr24(at + 0x4B);
    }
    return 0;
}

// ---- the M36 display transfer -------------------------------------------------------------

bool As36ControlStorageProcessor::flushDeferredCnfwsPowerOnAid()
{
    int tub = pendingCnfwsPowerOnTub_;
    if (tub == 0) return false;
    pendingCnfwsPowerOnTub_ = 0;
    trace_.csp("cnfws power-on AID for TU {:06X} delivered at the idle boundary", tub);
    deliverWorkStationControllerFunction(tub, 0xF7, true, true, "SVC 43 cnfws success tail (deferred)", false);
    return true;
}

bool As36ControlStorageProcessor::beginM36WorkStationTransfer(int tub, bool autoSignOn, bool controllerWorkPending,
                                                              const std::string& call, bool& pending)
{
    lastM36WorkStationTransferPostedGuestWork_ = false;
    pending = false;
    if (tub == 0 || m_.readHalf(tub) != WorkStationIob::kUnitBlockEyecatcher) return false;
    int oc = m_.readAddr24(tub + 0x53);
    uint16_t key = m_.readHalf(tub + 0x4E);
    if (key == 0 || oc == 0 || m_.readHalf(oc) != 0xD6C3 || m_.readHalf(oc + 2) != key) {
        trace_.csp("{}: TU {:06X} is not a configured #SVTUB workstation (key {:04X}, OC {:06X})", call, tub, key, oc);
        return false;
    }
    if (controllerWorkPending) {
        pendingTransferredWorkStationTub_ = tub;
        pendingTransferAutoSignOn_ = autoSignOn;
        trace_.csp("{}: NuFsEmXferMach bind for TU {:06X} pended behind work-station controller work (NuEmul+1118 analogue)",
                   call, tub);
        pending = true;
        return true;
    }
    completeM36WorkStationTransfer(tub, autoSignOn, call);
    return true;
}

// Bind the display: publish the transfer-new bit in the unit block (+0x87
// |= 08, &= ~04), clear the error-present bits at +0x13 when the previous
// terminal left them, record the activation and run the response tail.
void As36ControlStorageProcessor::completeM36WorkStationTransfer(int tub, bool autoSignOn, const std::string& call)
{
    transferredWorkStationTub_ = tub;
    transferredAutoSignOn_ = autoSignOn;
    transferredWorkStations_[tub] = autoSignOn;
    pendingTransferredWorkStationTub_ = 0;
    pendingTransferAutoSignOn_ = false;
    trace_.csp("{}: NuFsEmXferMach/NuWsIoAction(0) bound configured TU {:06X}, W-id {:04X}, OC {:06X}; AUTOSIGNON {}", call,
               tub, m_.readHalf(tub + 0x4E), m_.readAddr24(tub + 0x53), autoSignOn ? "*YES" : "*NO");

    uint8_t oldTransferState = m_.readByte(tub + 0x87);
    uint8_t newTransferState = static_cast<uint8_t>((oldTransferState | 0x08) & ~0x04);
    m_.writeByte(tub + 0x87, newTransferState);

    uint8_t oldStatus = m_.readByte(tub + 0x13);
    if ((oldStatus & 0x2F) != 0) {
        m_.writeByte(tub + 0x13, static_cast<uint8_t>(oldStatus & ~0x2F));
        trace_.csp("{}: returnDeviceToSSP cleared TU {:06X}+13 {:02X}->{:02X}", call, tub, oldStatus, oldStatus & ~0x2F);
    }
    trace_.csp("{}: NuPersistWs published TU {:06X}+87 {:02X}->{:02X} (set transfer-new 08, clear autoConfig 04)", call, tub,
               oldTransferState, newTransferState);

    int unit = m_.readByte(tub + WorkStationIob::kOffUnitAddress);
    devices_.recordAction0Activation(unit);
    lastM36WorkStationTransferPostedGuestWork_ = tryDeliverAction0ActivationStatus(call + " action-0 response", true);
}

// The common response tail after fresh activation: the selected unit block
// comes from the controller's queue scan, not from the socket, and the
// power-on response byte then follows the controller's order (+8E first,
// the internal condition second).
bool As36ControlStorageProcessor::tryDeliverAction0ActivationStatus(const std::string& call, bool resumeFromMonitor)
{
    int unit;
    if (!devices_.tryTakeAction0Activation(unit)) return false;

    int tub = resolveWsEntryTubByUnit(unit);
    int configuredTub = resolveConfiguredTubByUnit(unit);
    if (workStationDiagnosticObserver) workStationDiagnosticObserver("wsentry-first-match", unit, tub, configuredTub);
    if (tub == 0) {
        trace_.csp("{}: checkForInviteComplete selected unit {:02X} from native BA.20 status, but wsfstdub70 found no guest "
                   "QH50 TUB",
                   call, unit);
        return false;
    }

    trace_.csp("{}: action response checkForInviteComplete -> activation F7 for unit {:02X}, wsfstdub70 TUB {:06X}; no guest "
               "unit-FF IOB required",
               call, unit, tub);
    return deliverWorkStationControllerFunction(tub, 0xF7, true, true, call + " wspostcp", resumeFromMonitor);
}

// A physical work station reporting in: the native active object must
// already exist (mapped and auto-configured) before the activation is
// recorded and the response tail runs.
bool As36ControlStorageProcessor::reportWorkStationHriReady(int unit, const std::string& call)
{
    WorkStationSlot* slot = devices_.workStations().find(unit);
    if (slot == nullptr || slot->isPrinter || !slot->nativeActive() || !slot->internalRendererBound()) {
        trace_.csp("{}: HRI report for unit {:02X} has no mapped, auto-configured native workstation (NuWs+F8 null)", call,
                   unit);
        return false;
    }
    devices_.recordPowerOnActivation(unit, "000D0502/NuWsIoAction(20)");
    tryDeliverAction0ActivationStatus(call + " action-20 response", true);
    return true;
}

bool As36ControlStorageProcessor::completePendingM36WorkStationTransfer(const std::string& call)
{
    if (pendingTransferredWorkStationTub_ == 0) return false;
    completeM36WorkStationTransfer(pendingTransferredWorkStationTub_, pendingTransferAutoSignOn_, call);
    return true;
}

// The lent display has gone away: the unit block and its OC persist, the
// transfer binding does not, so the next client on this unit is a new lend.
bool As36ControlStorageProcessor::endM36WorkStationTransfer(int tub, const std::string& call)
{
    if (tub == 0 || transferredWorkStations_.erase(tub) == 0) return false;
    if (transferredWorkStationTub_ == tub) {
        transferredWorkStationTub_ = 0;
        transferredAutoSignOn_ = false;
    }
    trace_.csp("{}: allowXpfToUseDevice - TU {:06X} transfer binding released; the next session is a new TFRM36 lend", call,
               tub);
    return true;
}

bool As36ControlStorageProcessor::isM36WorkStationTransferReady(int tub, bool autoSignOn) const
{
    if (tub == 0) return false;
    auto it = transferredWorkStations_.find(tub);
    return it != transferredWorkStations_.end() && it->second == autoSignOn;
}

std::string As36ControlStorageProcessor::m36WorkStationTransferState() const
{
    if (pendingTransferredWorkStationTub_ != 0)
        return fmt::format("pending TU {:06X}, AUTOSIGNON(*{})", pendingTransferredWorkStationTub_,
                           pendingTransferAutoSignOn_ ? "YES" : "NO");
    if (transferredWorkStationTub_ != 0)
        return fmt::format("bound TU {:06X}, AUTOSIGNON(*{})", transferredWorkStationTub_,
                           transferredAutoSignOn_ ? "YES" : "NO");
    return "unbound";
}

bool As36ControlStorageProcessor::postPowerOnInternalCondition(const std::string& call)
{
    return postPowerOnInternalCondition(call, true);
}

// The internal condition 0x14 posted to the IPL/command task: the condition
// is stored in both ace+13 and ace+29, so the waiter's XR1 receives it; the
// saved XR2 at ace+16 is zero.  Deliberately not a device-present post.
bool As36ControlStorageProcessor::postPowerOnInternalCondition(const std::string& call, bool resumeFromMonitor)
{
    constexpr int kIplCommandTaskId = 0x0009;
    constexpr int kPowerOnInternalCondition = 0x000014;
    int tb = findTaskById(kIplCommandTaskId, 0);
    if (tb == 0) {
        trace_.csp("{}: nupoic00 target task id {:04X} is not on queue 39", call, kIplCommandTaskId);
        return false;
    }

    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, kPowerOnInternalCondition);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, 0);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsInternalCondition |
                                      ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, kPowerOnInternalCondition);
    trace_.csp("{}: nupoic00 ACE {:04X} -> task {:04X}; condition {:06X}, event 0000, flags C8, returned XR1 {:06X}, saved "
               "XR2 000000",
               call, ace, tb, kPowerOnInternalCondition, kPowerOnInternalCondition);
    bool ok = completeToTask(ace, 0, call);
    if (ok && resumeFromMonitor) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "power-on internal-condition resume");
    }
    return ok;
}

bool As36ControlStorageProcessor::deliverWorkStationControllerFunction(int tub, uint8_t function, bool storeByte,
                                                                      bool postInterrupt, const std::string& call,
                                                                      bool resumeFromMonitor)
{
    if (tub == 0 || m_.readHalf(tub) != WorkStationIob::kUnitBlockEyecatcher) {
        trace_.csp("{}: {:06X} is not a terminal unit block", call, tub);
        return false;
    }
    if (storeByte) {
        m_.writeByte(tub + UnitBlock::kOffConfigured, function);
        trace_.csp("{}: wspostcp controller response {:02X} -> TU {:06X}+8E", call, function, tub);
    }
    return !postInterrupt || postPowerOnInternalCondition(call + " nupoic00", resumeFromMonitor);
}

bool As36ControlStorageProcessor::setWorkStationOcActive(int tub, bool active, const std::string& call)
{
    if (tub == 0 || m_.readHalf(tub) != WorkStationIob::kUnitBlockEyecatcher) return false;
    int oc = m_.readAddr24(tub + 0x53);
    if (oc == 0 || m_.readHalf(oc) != 0xD6C3) return false;
    uint8_t before = m_.readByte(oc + 0x0C);
    uint8_t after = active ? static_cast<uint8_t>(before | 0x80) : static_cast<uint8_t>(before & ~0x80);
    m_.writeByte(oc + 0x0C, after);
    trace_.csp("{}: TU {:06X} OC {:06X}+0C {:02X} -> {:02X} ({})", call, tub, oc, before, after,
               active ? "active" : "inactive");
    return true;
}

int As36ControlStorageProcessor::transferredOrRequestedTub(int requested)
{
    if (requested != 0 && m_.readHalf(requested) == WorkStationIob::kUnitBlockEyecatcher) return requested;
    if (transferredWorkStationTub_ != 0 && m_.readHalf(transferredWorkStationTub_) == WorkStationIob::kUnitBlockEyecatcher)
        return transferredWorkStationTub_;
    return consoleUnitBlock();
}

// ---- device-present and console producers ------------------------------------------------------

// A device raised an attention: set the device-present marker (+117 bit
// 0x10, the bit the sign-on gates test) on the terminal's unit block and
// post the owning task.
bool As36ControlStorageProcessor::raiseDeviceAttention(int unitBlock, int taskBlock, const std::string& call)
{
    resetWorkStationDeviceStatus();
    constexpr int kPresentMarkerOffset = 117;
    constexpr uint8_t kNeedsSignOn = 0x10;
    if (unitBlock != 0 && m_.readHalf(unitBlock) == WorkStationIob::kUnitBlockEyecatcher) {
        int marker = unitBlock + kPresentMarkerOffset;
        m_.writeByte(marker, static_cast<uint8_t>(m_.readByte(marker) | kNeedsSignOn));
        trace_.csp("{}: device-present marker (+117 bit 0x10) set on terminal unit block {:06X}", call, unitBlock);
    }
    return postDevicePresent(taskBlock, unitBlock, call);
}

bool As36ControlStorageProcessor::postDevicePresent(int taskBlock, int unitBlock, const std::string& call)
{
    if (!TaskBlock::isTaskBlock(m_, taskBlock)) return false;

    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, unitBlock);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, taskBlock);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, 0);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    trace_.csp("{}: device-present ACE {:04X} -> task {:04X} (unit block {:06X})", call, ace, taskBlock, unitBlock);
    bool ok = completeToTask(ace, 0, call);
    if (ok) {
        // A real post is followed by the supervisor call's close, which
        // reloads the now-dispatched task's registers; invoked from the
        // monitor there is no such close.
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "device-present resume");
    }
    return ok;
}

// The inbound work-station completion wakes the IPL/command task (id 9)
// where it waits with event type 0x29; the post drives the whole
// post-sign-on chain.
bool As36ControlStorageProcessor::postWorkStationPresent(int unitBlock, const std::string& call)
{
    constexpr int kWorkStationTaskId = 0x0009;
    constexpr uint8_t kWorkStationPresentEvent = 0x29;

    int tb = findTaskById(kWorkStationTaskId, 0);
    if (tb == 0) {
        trace_.csp("{}: the IPL/command task (id {:04X}, 0F00) is not on queue 39 - cannot post the work-station completion",
                   call, kWorkStationTaskId);
        return false;
    }

    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, unitBlock);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, kWorkStationPresentEvent);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    trace_.csp("{}: work-station completion ACE {:04X} -> task {:04X} (0F00), event type {:02X}, unit block {:06X}", call, ace,
               tb, kWorkStationPresentEvent, unitBlock);

    bool ok = completeToTask(ace, 0, call);
    if (ok) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "work-station-present resume");
    }
    return ok;
}

bool As36ControlStorageProcessor::postConsoleSignOn(int unitBlock, const std::string& call)
{
    return postRouterElement(kConsoleOwnerTaskId, cfg_.consoleSignOnRouterKey, unitBlock, call);
}

// Copy len bytes where both displacements name the field's LAST byte.
void As36ControlStorageProcessor::jcbCopyEndingAt(int jcb, int dst, int tub, int src, int len)
{
    for (int i = 0; i < len; i++) m_.writeByte(jcb + dst - i, m_.readByte(tub + src - i));
}

// Monitor experiment, inert on every default path: build the interactive
// job's control block the way the sign-on module does (assign 256 bytes,
// zero it, copy the console-sourced fields, set the "built" bit, link it
// into the console's unit block) and then stamp the class-init gates the
// unmodelled job-region path would have armed.  Labelled experimental in
// its own trace.
bool As36ControlStorageProcessor::synthesizeJcb(int tubAddress, const std::string& call)
{
    int tub = consoleUnitBlock();
    if (tub == 0)
        tub = tubAddress != 0 && m_.readHalf(tubAddress) == WorkStationIob::kUnitBlockEyecatcher ? tubAddress : 0;
    if (tub == 0) {
        trace_.csp("{}: no published console TUB (\"TU\" block at guest 0x092A) - cannot synthesize the JCB", call);
        return false;
    }

    constexpr int kTubJobPointer = 0x63;

    int existing = m_.readAddr24(tub + kTubJobPointer);
    if (existing != 0) {
        trace_.csp("{}: console TUB {:06X} already links a JCB at {:06X} (TUB+0x63..0x65 non-null); #CPON's build is "
                   "idempotent - relinking into the job tasks only",
                   call, tub, existing);
        experimentLinkJcbIntoTasks(existing, call);
        return true;
    }

    constexpr int kJcbBytes = 0x100;
    int jcb = heap_.allocate(kJcbBytes);
    if (jcb == 0) {
        trace_.csp("{}: system queue space exhausted - cannot assign the 256-byte JCB (#CPON 0x14B5 SVC 06 equivalent)",
                   call);
        return false;
    }
    for (int i = 0; i < kJcbBytes; i++) m_.writeByte(jcb + i, 0);

    jcbCopyEndingAt(jcb, 0x23, tub, 0x14, 4);
    jcbCopyEndingAt(jcb, 0x24, tub, 0x06, 1);
    jcbCopyEndingAt(jcb, 0x2D, tub, 0x3A, 2);
    uint8_t b39 = static_cast<uint8_t>(m_.readByte(tub + 0x02) & 0x07);
    m_.writeByte(jcb + 0x39, b39);
    jcbCopyEndingAt(jcb, 0x0C, tub, 0x90, 8);

    m_.writeByte(jcb + 0x35, static_cast<uint8_t>(m_.readByte(jcb + 0x35) | 0x20));

    m_.writeAddr24(tub + kTubJobPointer, jcb);

    m_.writeByte(jcb + 0x35, static_cast<uint8_t>(m_.readByte(jcb + 0x35) | 0x40));
    m_.writeByte(jcb + 0x36, 0x88);

    trace_.csp("{}: EXPERIMENTAL - synthesized #CPON-style JCB at guest {:06X} (256B, zero-formatted, fields copied from "
               "console TUB {:06X}); JCB+0x35=0x{:02X} (bit 0x20 #CPON, bit 0x40 #CLSS map gate EXPERIMENTAL), JCB+0x36=0x88 "
               "(#CI* settled, EXPERIMENTAL); linked at TUB+0x63..0x65. UNFAITHFUL - #CPON was never dispatched "
               "(docs/s36/job-initiator.md).",
               call, jcb, tub, m_.readByte(jcb + 0x35));

    experimentLinkJcbIntoTasks(jcb, call);
    return true;
}

void As36ControlStorageProcessor::experimentLinkJcbIntoTasks(int jcb, const std::string& call)
{
    int at = m_.readAddr24(GuestLowStorage::queueHeader(39));
    int linked = 0;
    for (int guard = 0; at != 0 && guard < 4096; guard++) {
        if (m_.readHalf(at) != GuestLowStorage::kEyeTaskBlock) break;
        if (m_.readAddr24(at + TaskBlock::kOffJobControlBlock) == 0) {
            m_.writeAddr24(at + TaskBlock::kOffJobControlBlock, jcb);
            linked++;
        }
        at = m_.readAddr24(at + TaskBlock::kOffQueue39Link);
    }
    if (linked > 0)
        trace_.csp("{}: EXPERIMENTAL - linked JCB {:06X} into {} task(s)' tb+0x15..0x17 (#SVAT 0x10EB copy; UNFAITHFUL)", call,
                   jcb, linked);
}

bool As36ControlStorageProcessor::postConsoleCommandStatement(int tubAddress, const std::string& call)
{
    int tub = tubAddress != 0 && m_.readHalf(tubAddress) == WorkStationIob::kUnitBlockEyecatcher ? tubAddress
                                                                                                 : consoleUnitBlock();
    return postRouterElement(kConsoleOwnerTaskId, kConsoleCommandStatementCode, tub, call);
}

bool As36ControlStorageProcessor::postConsoleSignOnStatement(int tubAddress, const std::string& call)
{
    int key = transferredOrRequestedTub(tubAddress);
    if (key == 0) {
        trace_.csp("{}: no published console TUB (\"TU\" block at guest 0x092A) - cannot build the sign-on statement element",
                   call);
        return false;
    }
    return postRouterElement(kConsoleOwnerTaskId, key, key, call);
}

// Counterfactual experiment: stamp the paired console request state that
// selects the short sign-on route.  Retained for explicit experiments only.
bool As36ControlStorageProcessor::postConsoleSignOnRequest(int tubAddress, const std::string& call)
{
    int tub = transferredOrRequestedTub(tubAddress);
    if (tub == 0) {
        trace_.csp("{}: no published console TUB (\"TU\" block) - cannot stamp the sign-on request state", call);
        return false;
    }
    m_.writeByte(tub + 0x8D, 0xF1);
    m_.writeByte(tub + 0x81, static_cast<uint8_t>(m_.readByte(tub + 0x81) | 0x08));
    trace_.csp("{}: console TUB {:06X} request state stamped - +0x8D=F1, +0x81 bit 0x08 (inbound sign-on request; drives "
               "#CPRT past its 0x1058 exit to #CPTS)",
               call, tub);
    return true;
}

// Counterfactual probe for the unit block's +0x79 bit 0x20; no normal path
// calls it.
bool As36ControlStorageProcessor::postConsoleDeviceAttach(int tubAddress, const std::string& call)
{
    int tub = transferredOrRequestedTub(tubAddress);
    if (tub == 0) {
        trace_.csp("{}: no published console TUB (\"TU\" block) - cannot model the console work-station device attach", call);
        return false;
    }
    constexpr int kAttachStatusByte = 0x79;
    constexpr uint8_t kAttachComplete = 0x20;
    uint8_t v = m_.readByte(tub + kAttachStatusByte);
    if ((v & kAttachComplete) != 0) {
        trace_.csp("{}: console TUB {:06X} device-attach status +0x79 bit 0x20 already set", call, tub);
        return true;
    }
    m_.writeByte(tub + kAttachStatusByte, static_cast<uint8_t>(v | kAttachComplete));
    trace_.csp("{}: COUNTERFACTUAL console TUB {:06X} status +0x79 |= 0x20; normal successful #WDDA clears this bit and "
               "skips its 0x200B setter. This probe selects #SVAT's alternate #ICDB arm only. "
               "docs/s36/qh112-legitimate-producers.md",
               call, tub);
    return true;
}

// The console data-base element the console family would enqueue at
// attach, so the cleanup's key search on queue 50 finds it: a 'T'-class
// element keyed by the console unit block, spliced in as the block's
// immediate predecessor where the block's own forward link rejoins the live
// chain.  Monitor-gated; nothing on the default path calls it.
bool As36ControlStorageProcessor::postConsoleDbElement(int tubAddress, const std::string& call)
{
    int tub = consoleUnitBlock();
    if (tub == 0)
        tub = tubAddress != 0 && m_.readHalf(tubAddress) == WorkStationIob::kUnitBlockEyecatcher ? tubAddress : 0;
    if (tub == 0) {
        trace_.csp("{}: no published console TUB (\"TU\" block) - cannot build the console-DB element", call);
        return false;
    }

    constexpr int kQ50 = 50;
    constexpr int kKeyField = 0x4B;
    constexpr int kSbField = 0x3C;
    constexpr int kAreaField = 0x11;
    constexpr int kElementBytes = 160;

    int head50 = m_.readAddr24(GuestLowStorage::queueHeader(kQ50));
    for (int at = head50, steps = 0; at != 0 && steps < 64; steps++) {
        if (m_.readAddr24(at + kKeyField) == tub) {
            trace_.csp("{}: console-DB element {:06X} already on queue 50 keyed by TUB {:06X}", call, at, tub);
            return true;
        }
        at = m_.readAddr24(at + kKeyField);
    }

    int sb = heap_.allocate(64);
    if (sb == 0) {
        trace_.csp("{}: heap exhausted (SB)", call);
        return false;
    }
    for (int i = 0; i < 64; i++) m_.writeByte(sb + i, 0);
    m_.writeHalf(sb + StorageBlock::kOffEyecatcher, GuestLowStorage::kEyeSystemBlock);
    m_.writeHalf(sb + StorageBlock::kOffSizePages, 1);

    int area = heap_.allocate(64);
    if (area == 0) {
        trace_.csp("{}: heap exhausted (area)", call);
        return false;
    }
    for (int i = 0; i < 64; i++) m_.writeByte(area + i, 0);

    int e = heap_.allocate(kElementBytes);
    if (e == 0) {
        trace_.csp("{}: heap exhausted (element)", call);
        return false;
    }
    for (int i = 0; i < kElementBytes; i++) m_.writeByte(e + i, 0);

    m_.writeByte(e + 0x00, 0xE3);
    m_.writeAddr24(e + kKeyField, tub);
    m_.writeAddr24(e + kSbField, sb);
    m_.writeAddr24(e + kAreaField, area);

    // The cleanup reads the storage block and the area off the unit block
    // itself (rightmost-byte displacements 0x3C and 0x13), so both live on
    // the unit block as well; the area is a displacement into the block's
    // work space with a small non-zero length at +0x15.
    m_.writeAddr24(tub + (kSbField - 2), sb);
    m_.writeAddr24(tub + 0x11, 0);
    m_.writeByte(tub + 0x15, 0x40);

    int headField = GuestLowStorage::queueHeader(kQ50);
    int x = m_.readAddr24(tub + kKeyField);

    int spliceField = 0;
    {
        int prevField = headField;
        int at = m_.readAddr24(prevField);
        std::set<int> seen;
        while (at != 0 && seen.insert(at).second) {
            int next = m_.readAddr24(at + kKeyField);
            if (next == tub) {
                trace_.csp("{}: console-DB element {:06X} already threaded before the TUB {:06X} on queue 50", call, at, tub);
                return true;
            }
            if (at == x) spliceField = prevField;
            prevField = at + kKeyField;
            at = next;
        }
    }

    if (x != 0 && spliceField == 0) {
        trace_.csp("{}: the console TUB {:06X} forward link X={:06X} is not a live queue-50 node - cannot thread the TUB "
                   "acyclically; unmodeled topology. Element {:06X} (SB {:06X}, area {:06X}) built but not spliced. "
                   "docs/s36/console-db-grounding.md",
                   call, tub, x, e, sb, area);
        return false;
    }

    if (spliceField != 0) {
        m_.writeAddr24(spliceField, e);
    } else {
        int prevField = headField, at = m_.readAddr24(prevField);
        std::set<int> seen;
        while (at != 0 && seen.insert(at).second) {
            prevField = at + kKeyField;
            at = m_.readAddr24(prevField);
        }
        m_.writeAddr24(prevField, e);
    }

    trace_.csp("{}: console-DB element {:06X} spliced before the console TUB {:06X} on queue 50 (... element -> TUB -> {:06X} "
               "...) - +0=E3, +0x4B..4D={:06X} (TUB key/chain), +0x3C={:06X} (SB), +0x13 area={:06X}. #ICDB@0x1047 SVC 1B "
               "finds it as the TUB's predecessor. docs/s36/console-db-grounding.md",
               call, e, tub, x, tub, sb, area);

    int consoleHead = m_.readAddr24(headField);
    if (consoleHead != 0 && m_.readAddr24(consoleHead + kConsoleActiveSessionField) == tub)
        trace_.csp("{}: NOTE - queue-50 head {:06X} names the console TUB {:06X} through +0x91 (expected: the timing model "
                   "keeps the active-session pointer aliasing the TUB so #SVAT/#ICDB resolve it; MSIPL's phase-3 walk "
                   "already ran, so no element<->TUB cycle forms). docs/s36/icdb-build-model.md",
                   call, consoleHead, tub);

    m_.writeByte(tub + 0x79, static_cast<uint8_t>(m_.readByte(tub + 0x79) | 0x20));
    m_.writeByte(0x08B7, static_cast<uint8_t>(m_.readByte(0x08B7) | 0x04));
    trace_.csp("{}: console-command-env arm stand-in - set [0x08B7] |= 0x04 (the #CIML 0x101D gate; makes #CIML run #CI* so "
               "JCB+0x35.40 sets and #CLSS passes) and TUB {:06X}+0x79 |= 0x20 (device-attach gate). "
               "docs/s36/ciml-gate-is-08b7-04.md",
               call, tub);
    return true;
}

bool As36ControlStorageProcessor::postRouterElement(int taskId, int routingKey, int unitBlock, const std::string& call)
{
    resetWorkStationDeviceStatus();
    int tb = findTaskById(taskId, 0);
    if (tb == 0) {
        trace_.csp("{}: task id {:04X} not on queue 39", call, taskId);
        return false;
    }
    if (unitBlock != 0 && m_.readHalf(unitBlock) == WorkStationIob::kUnitBlockEyecatcher)
        m_.writeByte(unitBlock + 117, static_cast<uint8_t>(m_.readByte(unitBlock + 117) | 0x10));
    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, unitBlock);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, 0);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, routingKey);
    trace_.csp("{}: router element ACE {:04X} -> task {:04X}, routing key {:06X}, unit {:06X}", call, ace, tb, routingKey,
               unitBlock);
    bool ok = completeToTask(ace, 0, call);
    if (ok) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "router element resume");
    }
    return ok;
}

// The guest-visible half of a native SSP call request: the command router
// receives XR2 pointing at a CF/9F00 request whose +24 names a second,
// producer-owned request block and whose +48 names the eight-byte program.
// The allocation is retained, as the native action retains it.
bool As36ControlStorageProcessor::postNativeSspCall(const std::string& program, int requestBlock, const std::string& call,
                                                    int& cf)
{
    cf = 0;
    constexpr int kNativeAllocationBytes = 142;
    constexpr int kCfBias = 16;
    constexpr int kCfPayload = 48;
    constexpr int kRouterTaskId = 0x0009;
    constexpr int kRouterKey = 0x00001C;

    int tb = findTaskById(kRouterTaskId, 0);
    if (tb == 0) {
        trace_.csp("{}: NuCallSSP target task id {:04X} is not on queue 39", call, kRouterTaskId);
        return false;
    }

    std::string name = program;
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (name.size() > 8) name = name.substr(0, 8);
    while (name.size() < 8) name += ' ';
    std::vector<uint8_t> ebcdicName = storage::Ebcdic::fromAscii(name);

    int allocation = heap_.allocate(kNativeAllocationBytes);
    if (allocation == 0) return false;
    for (int i = 0; i < 128; i++) m_.writeByte(allocation + i, 0);

    cf = allocation + kCfBias;
    m_.writeHalf(cf + 0, 0xC3C6);
    m_.writeHalf(cf + 2, 0x9F00);
    m_.writeHalf(cf + 6, 0x00FF);
    m_.writeByte(cf + 18, 0xC0);
    m_.writeAddr24(cf + 21, cf + kCfPayload);
    m_.writeAddr24(cf + 24, requestBlock & 0xFFFFFF);
    m_.writeByte(cf + kCfPayload, 8);
    for (int i = 0; i < 8; i++) m_.writeByte(cf + kCfPayload + 1 + i, ebcdicName[static_cast<std::size_t>(i)]);
    m_.writeByte(cf + kCfPayload + 9, 0x80);

    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, kRouterKey);
    m_.writeAddr24(ace + ActionControlElement::kOffXr2, cf);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, 0);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsInternalCondition |
                                      ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, kRouterKey);

    trace_.csp("{}: NuCallSSP CF {:06X} ('{}', request {:06X}) via ACE {:04X} -> task {:04X}; XR1 00001C, XR2 {:06X}, event "
               "0000, flags C8",
               call, cf, name, requestBlock & 0xFFFFFF, ace, tb, cf);
    bool ok = completeToTask(ace, 0, call, true);
    if (ok) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "native SSP call resume");
    }
    return ok;
}

bool As36ControlStorageProcessor::postNativeS36ConsoleCall(uint16_t messageId, uint8_t line410, uint8_t line408,
                                                           uint8_t tag0, uint8_t tag1, const std::string& call,
                                                           int& request, int& cf)
{
    request = heap_.allocate(32);
    cf = 0;
    if (request == 0) return false;
    for (int i = 0; i < 32; i++) m_.writeByte(request + i, 0);
    m_.writeByte(request + 0, tag0);
    m_.writeByte(request + 1, tag1);
    m_.writeByte(request + 3, 0x20);
    m_.writeByte(request + 4, 0x01);
    m_.writeHalf(request + 5, 0xD3C9);
    m_.writeHalf(request + 7, 0xC340);
    m_.writeHalf(request + 9, messageId);
    m_.writeByte(request + 11, line410);
    m_.writeByte(request + 16, line408);
    m_.writeAddr24(request + 17, request + 32);
    m_.writeHalf(request + 30, messageId);
    trace_.csp("{}: NuS36ConsoleMsg request {:06X}: message {:04X}, line+410 {:02X}, line+408 {:02X}, tags {:02X}/{:02X}; "
               "LI/C, tail {:06X}",
               call, request, messageId, line410, line408, tag0, tag1, request + 32);
    return postNativeSspCall("#CLSG", request, call, cf);
}

// Experimental job-task wake: a specific-wait completion keyed on the
// waiter's own saved XR1.  The shape is the guest contract; the producer is
// synthetic.
bool As36ControlStorageProcessor::wakeJobTaskSelfEvent(int taskId, const std::string& call)
{
    int tb = findTaskById(taskId, 0);
    if (tb == 0) {
        trace_.csp("{}: task id {:04X} not on queue 39", call, taskId);
        return false;
    }
    if ((m_.readByte(tb + TaskBlock::kOffStat2) & 0x80) == 0) {
        trace_.csp("{}: task {:04X} is not in an event wait (tb+5 bit 0x80 clear) - nothing to wake", call, tb);
        return false;
    }
    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (rb == 0) {
        trace_.csp("{}: task {:04X} has no request block", call, tb);
        return false;
    }
    uint8_t q = m_.readByte(rb + RequestBlock::kOffQByte);
    int key = RequestBlock::readXr1Field(m_, rb);
    trace_.csp("{}: EXPERIMENTAL job-task wake - task {:04X} rb {:04X} Q {:02X} (multiple-wait bit {}); posting specific-wait "
               "element keyed on its saved XR1 {:06X}",
               call, tb, rb, q, (q & 0x08) != 0 ? "SET - not a specific wait, wake may not match" : "clear", key);
    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, 0);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, key);
    bool ok = completeToTask(ace, 0, call);
    if (ok) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "job-task wake resume");
    }
    return ok;
}

bool As36ControlStorageProcessor::postTypedEventToTask(int taskBlock, uint16_t eventType, int unitBlock,
                                                       const std::string& call)
{
    if (!TaskBlock::isTaskBlock(m_, taskBlock)) {
        trace_.csp("{}: {:06X} is not a task block", call, taskBlock);
        return false;
    }
    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, unitBlock);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, taskBlock);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, eventType);
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, unitBlock);
    trace_.csp("{}: typed ACE {:04X} -> task {:04X}, event type {:04X}, unit {:06X}", call, ace, taskBlock, eventType,
               unitBlock);
    bool ok = completeToTask(ace, 0, call);
    if (ok) {
        restoreRegisters(currentRequestBlock_);
        enableDispatching(call, "typed-event resume");
    }
    return ok;
}

// Install a 32-byte message-session "OC" block on queue header 53 with the
// given key and the alternate-key marker, as the unit-block builder does
// from an alternate-key station record.  Idempotent.
int As36ControlStorageProcessor::installMessageSessionOc(int key, const std::string& call)
{
    int header = GuestLowStorage::queueHeader(53);
    int at = m_.readAddr24(header), guard = 0;
    while (at != 0 && guard++ < 256) {
        if (m_.readHalf(at) == 0xD6C3 && m_.readHalf(at + 2) == static_cast<uint16_t>(key)) {
            trace_.csp("{}: OC block keyed {:04X} already on QH53 at {:06X}", call, key, at);
            return at;
        }
        at = m_.readAddr24(at + 5);
    }
    int oc = heap_.allocate(32);
    if (oc == 0) {
        trace_.csp("{}: heap cannot supply 32B for the OC block", call);
        return 0;
    }
    for (int i = 0; i < 32; i++) m_.writeByte(oc + i, 0);
    m_.writeHalf(oc + 0, 0xD6C3);
    m_.writeHalf(oc + 2, static_cast<uint16_t>(key));
    m_.writeByte(oc + 0x0E, 0x80);
    queueOperation(header, oc, 7, 0x40);
    trace_.csp("{}: installed 32B OC block at {:06X} keyed {:04X} (+0x0E=0x80) on QH53 (header {:04X}); #CPSC SVC 1B @0x1D62 "
               "will now FIND it -> 0x196D branch",
               call, oc, key, header);
    return oc;
}

void As36ControlStorageProcessor::resetWorkStationDeviceStatus()
{
    wsDeviceStatusPending_ = 0;
    wsDeviceStatusDelivered_ = 0;
    wsPresentByTask_.clear();
}

int As36ControlStorageProcessor::publishedConsoleUnitBlock()
{
    int ub = consoleUnitBlock();
    if (ub != 0) return ub;
    int q49 = m_.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kConsoleUnitBlockQueue));
    if (q49 != 0 && m_.readHalf(q49) == WorkStationIob::kUnitBlockEyecatcher) return q49;
    return 0;
}

// Diagnostic message injector: the completed element hands the router XR1 =
// ace+29 (the type), XR2 = ace+16 (parameter 1) and WR6 = ace+22
// (parameter 2).
bool As36ControlStorageProcessor::postConsoleMessage(int taskBlock, int type, int p1, int p2, const std::string& call)
{
    if (!TaskBlock::isTaskBlock(m_, taskBlock)) return false;
    int ace = aces_.allocate();
    if (ace == 0) return false;
    m_.writeHalf(ace + ActionControlElement::kOffEyecatcher, ActionControlElement::kEyecatcher);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    m_.writeAddr24(ace + ActionControlElement::kOffXr1, type & 0xFFFFFF);
    m_.writeAddr24(ace + ActionControlElement::kOffXr2, p1 & 0xFFFFFF);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, taskBlock);
    m_.writeHalf(ace + ActionControlElement::kOffEventType, static_cast<uint16_t>(p2));
    m_.writeByte(ace + ActionControlElement::kOffFlags,
                 static_cast<uint8_t>(ActionControlElement::kFlagsBase | ActionControlElement::kFlagsMultipleWait));
    m_.writeAddr24(ace + ActionControlElement::kOffXr1Copy, type & 0xFFFFFF);
    trace_.csp("{}: injected console message ACE {:04X} -> task {:04X} (type {:X}, p1 {:06X}, p2 {:04X}); nuevt delivers "
               "XR1=type, XR2=p1, WR6=p2",
               call, ace, taskBlock, type, p1, p2);
    return completeToTask(ace, 0, call);
}

// ---- the deferred Read Input Fields result ---------------------------------------------------------

// Deliver a held Read Input Fields result onto the resident frame of the
// work-space block a map has just named, so the write and the module's read
// through its own map share one frame.  Only a system work-space block
// (type < 128) carries the staged work page.
void As36ControlStorageProcessor::deliverDeferredWorkStationInput(int taskBlock, int block, uint8_t type)
{
    auto it = deferredWsInput_.find(taskBlock);
    if (it == deferredWsInput_.end()) return;
    if (type >= ControlBlock::kTypeTaskOwned) return;

    int blockDisplacement = it->second.blockDisplacement;
    int fieldDataOffset = workStationInputFieldDataOffset(m_, taskBlock, it->second.wsuReturnCopy);
    int disp = blockDisplacement + fieldDataOffset;
    int page = disp >> machine::MachineState::kPageShift;
    int offset = disp & (machine::MachineState::kPageBytes - 1);

    const std::vector<int>* resident = workSpaceResidentPages(block, "deferred WS input", page, 1);
    if (resident == nullptr || page >= static_cast<int>(resident->size()) || (*resident)[static_cast<std::size_t>(page)] == 0) {
        trace_.csp("SVC 2F: deferred Read Input Fields result for task {:04X} not delivered - block {:06X} page {} has no "
                   "resident frame; the A7 staging copy stands",
                   taskBlock, block, page);
        deferredWsInput_.erase(it);
        return;
    }

    int frame = (*resident)[static_cast<std::size_t>(page)];
    int dest = frame + offset;
    int copied = static_cast<int>(it->second.bytes.size());
    if (dest < 0 || dest + copied > m_.backingBytes()) {
        deferredWsInput_.erase(it);
        return;
    }
    for (int n = 0; n < copied; n++) m_.writeByte(dest + n, it->second.bytes[static_cast<std::size_t>(n)]);
    deferredWsInput_.erase(it);
    trace_.csp("SVC 2F: delivered {} Read Input Fields byte(s) onto work-space block {:06X} resident frame {:06X}+{:03X} = "
               "real {:06X} (record base {:03X} + WSU field-data offset {:02X}; readInputFields staging coherence: the "
               "frame #WDDG's D418 MVC reads)",
               copied, block, frame, offset, dest, blockDisplacement, fieldDataOffset);
}

}  // namespace sim36::processors::controlstorage
