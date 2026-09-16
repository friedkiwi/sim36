// One work station controller with a port x address grid, which is what the
// hardware, the configuration and the machine's own object model all are;
// the operator display renders the same thing as 8 ports by 7 addresses.
//
// This models the controller SSP talks to, not the terminal a user sits at.
// The terminal is behind IWorkStationBackend.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Devices/DeviceCodes.h"
#include "Devices/IWorkStationBackend.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"

namespace sim36::devices {

// One (port, address) slot on the controller, and whatever terminal is bound
// to it.  The machine's model is a persistent slot with sessions attached
// and detached, not a device created and destroyed, so a disconnected
// station is an idle slot, never a missing one.
class WorkStationSlot {
public:
    // Six bytes: the configuration reader writes them as a word plus a
    // halfword out of the device object and the configurer writes the same
    // six back into it, so the record is the controller's whole idea of a
    // station.
    static constexpr int kConfigurationRecordBytes = 6;

    // Record byte 1, bits 0x30: (byte1 & 0x30) == 0x20 is a printer,
    // anything else a display.
    static constexpr int kRecordDeviceClass = 1;
    static constexpr uint8_t kDeviceClassMask = 0x30;
    static constexpr uint8_t kDeviceClassPrinter = 0x20;

    // Record byte 3, the device TYPE byte, which the device-type dispatch
    // switches on and SSP's own table matches against its entry byte 4.
    // This is NOT the configured `device-code`: that is SC21-9052's
    // operator-facing code, a label on a signature; DeviceCodes turns it
    // into this byte and into byte 1.  A 3180 Model 2 is device code 11 and
    // device type byte 0x20.
    static constexpr int kRecordDeviceCode = 3;

    // A display or console slot.
    WorkStationSlot(int port, int address, const std::string& deviceCode, std::unique_ptr<IWorkStationBackend> backend,
                    bool signOnAtIpl);
    // A printer slot.
    WorkStationSlot(int port, int address, const std::string& deviceCode, std::unique_ptr<IPrinterBackend> printer);

    const int port;
    const int address;
    const bool isPrinter;
    // True when the host acquired this physical display before SSP work
    // station configuration.
    const bool signOnAtIpl;
    // SC21-9052's device code as configured: the label, not a byte.
    const std::string deviceCode;
    // The two record bytes that code resolves to, from SSP's own table.
    const DeviceCodes::Entry device;

    // The host end.  Exactly one of these two is non-null, and which one is
    // what `isPrinter` reports.
    IWorkStationBackend* backend() const { return backend_.get(); }
    IPrinterBackend* printer() const { return printer_.get(); }

    // Set by command 81.  The controller constructs the active station object
    // and schedules the null-transfer action 0 which installs the generic
    // internal renderer.  Native state only.
    bool internalRendererBound() const { return internalRendererBound_; }
    bool transferRendererBound() const { return transferRendererBound_; }
    // Set by Configure New Work Stations, the way the configurer writes 0xF7
    // to the unit block's +142.
    bool configured() const { return configured_; }
    // Whether the station's native active object exists.  Command 81 and
    // renderer ownership are later, separate transitions.
    bool nativeActive() const { return nativeActive_; }
    // Exact command-82 eligibility: a device is reported only when its native
    // active object exists.
    bool activeForReadCurrentConfiguration() const { return nativeActive_; }

    // The display behind this slot was switched off after having been bound:
    // the native active object is null again, as it is after the session
    // ends.  Until the next action-0 bind every command to the unit is
    // rejected with the "device not attached" control field.
    bool displayDetached = false;

    // Guest address of this station's unit block once SSP has built one and
    // named it in an IOB.  Zero until then: the emulator learns it, it does
    // not choose it.
    int unitBlockAddress = 0;

    // The host end's name, whichever kind this slot is.
    std::string backendName() const { return isPrinter ? printer_->name() : backend_->name(); }
    bool backendAttached() const { return isPrinter ? printer_->attached() : backend_->attached(); }
    int unitAddress() const;

    // The six-byte record Read Current Configuration echoes back: the
    // configurer stores the record SSP supplied and the reader hands that
    // stored copy back.  Before command 81 there is nothing to echo; a
    // caller that asks anyway gets the synthesized form (bytes 0, 1 and 3
    // sourced; bytes 2, 4 and 5 zero, an absence of a claim rather than a
    // guess) so it still gets a well-formed record.
    std::vector<uint8_t> configurationRecord() const;

    // The configurer stores SSP's record verbatim and only then activates
    // and adds to the available list.
    void configure(const std::vector<uint8_t>& record);
    void bindTransferRenderer();
    // The IPL activates NOTHING: every station's hardware resource
    // indicator is null at IPL, so `signOnAtIpl` records operator intent
    // without manufacturing a native active object, which is command 81's
    // job alone.
    void activateAtIpl(bool acquiredHri);
    void restoreNativeState(bool nativeActive, bool configured, bool internalRenderer, bool transferRenderer);
    std::string toString() const;

private:
    std::unique_ptr<IWorkStationBackend> backend_;
    std::unique_ptr<IPrinterBackend> printer_;
    bool internalRendererBound_ = false;
    bool transferRendererBound_ = false;
    bool configured_ = false;
    bool nativeActive_ = false;
    bool hasConfigurationRecord_ = false;
    std::vector<uint8_t> configurationRecord_;
};

class WorkStationController {
public:
    // The encoded twinax address space exposed by this emulator.  Address 7
    // is reserved, so eight ports provide 8 * 7 = 56 usable positions.
    static constexpr int kPortCount = 8;
    static constexpr int kAddressesPerPort = 7;
    static constexpr int kAddressableDevices = kPortCount * kAddressesPerPort;

    // The native configuration reader walks indexes 0..63 and the configurer
    // refuses a request longer than this controller-command limit.  This is
    // distinct from the 56 addresses that can actually be encoded and that
    // CNFIGSSP is told are available.
    static constexpr int kMaxDevices = 64;

    // The unit address is one byte: bits 0x70 the port, 0x07 the address,
    // 0x88 reserved.  The reader masks every record's first byte with 0x77,
    // the configurer tests the incoming address's 0x88 bits, and the twinax
    // base command format carries a THREE-bit station address, so port 0
    // address 0 is 00: the console's unit address on the volume's unit
    // definition table.
    static int unitAddress(int port, int address) { return ((port & 0x07) << 4) | (address & 0x07); }
    static int portOf(int unitAddress) { return (unitAddress >> 4) & 0x07; }
    static int addressOf(int unitAddress) { return unitAddress & 0x07; }

    explicit WorkStationController(monitor::Tracer& trace) : trace_(trace) {}

    void attach(std::unique_ptr<WorkStationSlot> slot);

    // Slots in the same address order as the machine's configuration reader:
    // controller index first, then the 8-by-7 port/address grid.
    std::vector<WorkStationSlot*> slots() const;
    int count() const { return static_cast<int>(byUnitAddress_.size()); }
    WorkStationSlot* find(int unitAddress) const;

    // Read Current Configuration: entries = (length - 1) / 6; for index
    // 0..63, each attached device writes six bytes with the first masked by
    // 0x77; the list is terminated with 0xFF.  Gated on auto-configuration:
    // with it off the answer is the terminator alone, not an error.  Returns
    // the number of bytes written.
    int readCurrentConfiguration(std::vector<uint8_t>& buffer, int length, bool autoConfigEnabled);

    // Configure New Work Stations: the same six-byte records in the other
    // direction.  Reason codes: 5 more records than the device maximum, 2 a
    // zero or non-multiple-of-6 length, 3 the address's 0x88 bits are wrong,
    // 4 that address already has a native active object.  Returns 0 on
    // success or the reason code, which the caller stores at IOB+0x18.
    int configureNewWorkStations(const std::vector<uint8_t>& buffer, int length);

    // Result metadata for the configurer's successful-completion tail: the
    // machine remembers whether this request configured station address zero
    // and, only then, emits the power-on aid after the normal completion.
    bool lastConfigureIncludedZeroAddress() const { return lastConfigureIncludedZeroAddress_; }

private:
    static std::string hex(const std::vector<uint8_t>& b);

    monitor::Tracer& trace_;
    std::map<int, std::unique_ptr<WorkStationSlot>> byUnitAddress_;
    bool lastConfigureIncludedZeroAddress_ = false;
};

}  // namespace sim36::devices
