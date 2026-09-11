#include "Devices/DeviceSet.h"

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::devices {

using processors::controlstorage::RequestBlock;

bool DeviceSet::deviceSvc(processors::controlstorage::SvcRequest& req)
{
    // The IOB pointer arrives the way every device address does: the raw
    // XR1 prefix:reg pair, resolved at use.  Phase 1 really does hand the
    // supervisor 801E26, so the resolution is load-bearing.
    int iobField = RequestBlock::readXr1Field(m_, req.requestBlock);
    int iob;
    if (!resolveBuffer(iobField, iob)) {
        trace_.diskIo("SVC {:02X}: IOB address {:06X} is task-translated and its page is not mapped - refused (nusvpta "
                      "would fault here too)",
                      req.r, iobField);
        return false;
    }
    switch (req.r) {
        case 0x40: return disk.execute(iob, req.q);
        case 0x41:
            trace_.diskIo("SVC 41: the diskette drive is not ported yet (milestone 7)");
            return false;
        case 0x42:
        case 0x43:
            trace_.ws("SVC {:02X}: the work station controller is not ported yet (milestone 6)", req.r);
            return false;
        // Declared by the volume's unit definition table, not modelled here.
        // From the guest's side these devices EXIST; what is missing is the
        // model of them, so they get the machine's own "not in this
        // configuration" answer rather than a refusal.
        case 0x44:
            return declineUnmodelled(iob, req.r, "data communications (2609 CMN01/CMN02 SNA/BSC/async lines)");
        case 0x45:
            return declineUnmodelled(iob, req.r, "diskette data compression assist (5360 hardware feature)");
        case 0x46:
            trace_.diskIo("SVC 46: the tape drive is not ported yet (milestone 7)");
            return false;
        case 0x48: return declineNoDataStorageController(iob);
        default:
            // Not in the UDT either: a genuine hole rather than an absent
            // device, so this one still stops the machine.
            trace_.diskIo("SVC {:02X}: no device class routed, and the volume's UDT does not declare one either", req.r);
            m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
            return false;
    }
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

// Answer for a device the machine has but this emulator does not model.  It
// ANSWERS rather than refusing: a device that cannot service a request
// posts a status, it does not halt the processor.  Every one is traced and
// counted.
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

}  // namespace sim36::devices
