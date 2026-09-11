// The machine's device complement.  Milestone 2 carries the fixed disk; the
// diskette, tape and work station controller arrive with milestones 4 and 7.
#pragma once

#include "Devices/VirtualFixedDisk.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Storage/DiskBackend.h"

namespace sim36::devices {

class DeviceSet {
public:
    DeviceSet(machine::MachineState& m, storage::DiskBackend& volume, monitor::Tracer& trace)
        : disk(m, volume, trace) {}

    VirtualFixedDisk disk;

    void resetPendingIo() {}
};

}  // namespace sim36::devices
