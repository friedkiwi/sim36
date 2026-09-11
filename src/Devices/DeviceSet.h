// The machine's device complement and the device half of the delayed SVC
// path.  Milestone 4 carries the fixed disk and the declines for the
// devices the volume's unit definition table declares but the emulator
// does not model; the work station controller arrives with milestone 6, the
// diskette and tape drives with milestone 7.
#pragma once

#include <cstdint>

#include "Devices/VirtualFixedDisk.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"
#include "Storage/DiskBackend.h"

namespace sim36::devices {

class DeviceSet {
public:
    // The byte the machine's own decline writes into the guest structure:
    // "not in this configuration".
    static constexpr uint8_t kNotInThisConfiguration = 0x20;
    static constexpr int kOffDeviceStatus = 12;
    // The completion code for a device that is not available.
    static constexpr int kDeviceNotAvailable = 3;

    DeviceSet(machine::MachineState& m, storage::DiskBackend& volume, monitor::Tracer& trace)
        : disk(m, volume, trace), m_(m), trace_(trace) {}

    VirtualFixedDisk disk;

    // The device half of SVC 40-48: resolve the IOB the way every device
    // address is resolved (real unless bit 0x800000 is set, then translated
    // through the task's ATRs) and dispatch on the R-byte.
    bool deviceSvc(processors::controlstorage::SvcRequest& req);

    // Is a request against this IOB still outstanding (a response the
    // device could not produce synchronously)?  Nothing is retained until
    // the work station controller exists.
    bool isPending(int) const { return false; }

    void resetPendingIo() {}

    // How many requests have been answered "device unavailable", and how
    // many data storage controller calls have been declined the way the
    // machine's own supervisor declines them.
    long long unmodelledRequests() const { return unmodelledRequests_; }
    long long dataStorageControllerRequests() const { return dataStorageControllerRequests_; }

private:
    bool resolveBuffer(int bufferField, int& addr);
    bool declineUnmodelled(int iob, uint8_t r, const char* what);
    bool declineNoDataStorageController(int iob);

    machine::MachineState& m_;
    monitor::Tracer& trace_;
    long long unmodelledRequests_ = 0;
    long long dataStorageControllerRequests_ = 0;
};

}  // namespace sim36::devices
