#include "Devices/WorkStationController.h"

#include <fmt/format.h>

namespace sim36::devices {

namespace {

DeviceCodes::Entry lookupDevice(const std::string& code)
{
    DeviceCodes::Entry entry;
    DeviceCodes::tryLookup(code, entry);
    return entry;
}

}  // namespace

WorkStationSlot::WorkStationSlot(int port_, int address_, const std::string& deviceCode_,
                                 std::unique_ptr<IWorkStationBackend> backend, bool signOnAtIpl_)
    : port(port_), address(address_), isPrinter(false), signOnAtIpl(signOnAtIpl_), deviceCode(deviceCode_),
      device(lookupDevice(deviceCode_)), backend_(std::move(backend))
{
}

WorkStationSlot::WorkStationSlot(int port_, int address_, const std::string& deviceCode_,
                                 std::unique_ptr<IPrinterBackend> printer)
    : port(port_), address(address_), isPrinter(true), signOnAtIpl(false), deviceCode(deviceCode_),
      device(lookupDevice(deviceCode_)), printer_(std::move(printer))
{
}

int WorkStationSlot::unitAddress() const { return WorkStationController::unitAddress(port, address); }

std::vector<uint8_t> WorkStationSlot::configurationRecord() const
{
    if (hasConfigurationRecord_) return configurationRecord_;

    std::vector<uint8_t> rec(kConfigurationRecordBytes, 0);
    rec[0] = static_cast<uint8_t>(unitAddress());
    rec[kRecordDeviceClass] = device.family;
    rec[kRecordDeviceCode] = device.type;
    return rec;
}

void WorkStationSlot::configure(const std::vector<uint8_t>& record)
{
    if (record.size() == kConfigurationRecordBytes) {
        configurationRecord_ = record;
        hasConfigurationRecord_ = true;
    }
    nativeActive_ = true;
    configured_ = true;
    if (!isPrinter) internalRendererBound_ = true;
}

void WorkStationSlot::bindTransferRenderer()
{
    if (!isPrinter) transferRendererBound_ = true;
}

void WorkStationSlot::activateAtIpl(bool)
{
    nativeActive_ = false;
    configured_ = false;
    internalRendererBound_ = false;
    transferRendererBound_ = false;
}

void WorkStationSlot::restoreNativeState(bool nativeActive, bool configured, bool internalRenderer, bool transferRenderer)
{
    nativeActive_ = nativeActive;
    configured_ = configured;
    internalRendererBound_ = !isPrinter && internalRenderer;
    transferRendererBound_ = !isPrinter && transferRenderer;
}

std::string WorkStationSlot::toString() const
{
    return fmt::format("{}.{} unit {:02X} {} code {} ({:02X} {:02X}){}{}{}{}{}", port, address, unitAddress(),
                       isPrinter ? "printer" : "display", deviceCode, device.family, device.type,
                       signOnAtIpl ? " IPL-owned" : "", nativeActive_ ? " active" : "", configured_ ? " configured" : "",
                       internalRendererBound_ ? " internal-renderer" : "", transferRendererBound_ ? " transfer-renderer" : "");
}

void WorkStationController::attach(std::unique_ptr<WorkStationSlot> slot)
{
    int unit = slot->unitAddress();
    byUnitAddress_[unit] = std::move(slot);
}

std::vector<WorkStationSlot*> WorkStationController::slots() const
{
    std::vector<WorkStationSlot*> ordered;
    for (int port = 0; port < kPortCount; port++)
        for (int address = 0; address < kAddressesPerPort; address++) {
            WorkStationSlot* slot = find(unitAddress(port, address));
            if (slot != nullptr) ordered.push_back(slot);
        }
    return ordered;
}

WorkStationSlot* WorkStationController::find(int unitAddress) const
{
    auto it = byUnitAddress_.find(unitAddress);
    return it == byUnitAddress_.end() ? nullptr : it->second.get();
}

int WorkStationController::readCurrentConfiguration(std::vector<uint8_t>& buffer, int length,
                                                    bool autoConfigEnabled)
{
    int capacity = length > 0 ? (length - 1) / WorkStationSlot::kConfigurationRecordBytes : 0;
    int written = 0;

    if (autoConfigEnabled) {
        for (WorkStationSlot* slot : slots()) {
            // The reader does not enumerate the persistent station map: it
            // skips an entry only when the native active object is null.
            // This is independent of command-81 configuration and of TCP
            // ownership.
            if (!slot->nativeActive()) {
                trace_.ws("  unit {:02X}: NuWs+F8 null; omitted by rdcnf", slot->unitAddress());
                continue;
            }
            if (written >= capacity) break;
            std::vector<uint8_t> rec = slot->configurationRecord();
            // The reader masks the first byte on the way out: it is the
            // controller stripping its own bits, which is where the machine
            // does it.
            rec[0] &= 0x77;
            std::copy(rec.begin(), rec.end(), buffer.begin() + written * WorkStationSlot::kConfigurationRecordBytes);
            trace_.ws("  entry {}: {}", written, hex(rec));
            written++;
        }
    } else {
        trace_.ws("  auto-configuration is disabled - rdcnf emits the terminator alone");
    }

    buffer[static_cast<std::size_t>(written * WorkStationSlot::kConfigurationRecordBytes)] = 0xFF;
    return written * WorkStationSlot::kConfigurationRecordBytes + 1;
}

int WorkStationController::configureNewWorkStations(const std::vector<uint8_t>& buffer, int length)
{
    lastConfigureIncludedZeroAddress_ = false;
    // The configurer computes TWO different counts from the length and they
    // are not interchangeable.  The capacity test uses (length - 1) / 6 and
    // falls into reason 5 only when strictly greater than the device
    // maximum, which admits maxDevices + 1 records.  Transcribed as written.
    int capacity = length > 0 ? (length - 1) / WorkStationSlot::kConfigurationRecordBytes : 0;
    if (capacity > kMaxDevices) return 5;

    // The LOOP is a plain remaining >= 6 walk, so the record count is
    // length / 6 with no bias, and reason 2 is exactly "length is zero, or
    // not a whole number of records": a single record is a length of 6.
    if (length == 0) return 2;
    if (length % WorkStationSlot::kConfigurationRecordBytes != 0) return 2;
    int records = length / WorkStationSlot::kConfigurationRecordBytes;

    for (int i = 0; i < records; i++) {
        auto begin = buffer.begin() + i * WorkStationSlot::kConfigurationRecordBytes;
        std::vector<uint8_t> rec(begin, begin + WorkStationSlot::kConfigurationRecordBytes);

        // The configurer checks the address's reserved bits before anything
        // else and gives up on the whole request, mid-list, when one is
        // wrong: the loop is purely count-driven and never looks for the
        // 0xFF.  The machine ORs in a controller byte before masking with
        // 0x88; nothing here has a controller byte to supply, so the mask is
        // applied to the record alone, a NARROWER test than the machine's.
        int unit = rec[0];
        if ((unit & 0x88) != 0) {
            trace_.ws("  record {}: {} - byte 0 = {:02X} has the 0x88 bits the controller reserves, cnerr reason 3 "
                      "(c183ad90)",
                      i, hex(rec), unit);
            return 3;
        }

        WorkStationSlot* slot = find(unit);
        // The configurer does not test whether this station has previously
        // accepted a command-81 record: it reports reason 4 when the NATIVE
        // ACTIVE object is non-null.
        if (slot != nullptr && slot->nativeActive()) {
            trace_.ws("  record {}: unit {:02X} already has NuWs+F8 native active; cnerr reason 4 (c183ade4..c183ae1c)",
                      i, unit);
            return 4;
        }
        if (slot == nullptr) {
            // The machine creates a station object for an address it does
            // not know.  This model cannot: a slot with no backend is a
            // station with nothing behind it, and inventing one would report
            // a terminal the configuration never declared.
            trace_.ws("  record {}: unit {:02X} (port {} address {}) is not configured in etc/s36refemu.conf - cnfws would "
                      "call NuMach::createNuWs here, which needs a backend this emulator has no way to conjure",
                      i, unit, portOf(unit), addressOf(unit));
            continue;
        }

        slot->configure(rec);
        if (unit == 0) lastConfigureIncludedZeroAddress_ = true;
        trace_.ws("  record {}: unit {:02X} configured, {}; {}", i, unit, hex(rec),
                  slot->isPrinter ? "printer device" : "null-transfer action 0 -> internal NuDsp5250 renderer");
    }
    return 0;
}

std::string WorkStationController::hex(const std::vector<uint8_t>& b)
{
    std::string s;
    for (uint8_t x : b) {
        if (!s.empty()) s += ' ';
        s += fmt::format("{:02X}", x);
    }
    return s;
}

}  // namespace sim36::devices
