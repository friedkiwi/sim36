// Everything wired together: one object the monitor can inspect.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Machine/MachineState.h"
#include "Machine/Scheduler.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Storage/DiskBackend.h"
#include "Storage/Vtoc.h"

namespace sim36::machine {

class Machine {
public:
    // Constructs the machine from a validated definition.  Throws
    // std::runtime_error (a host layer) when the volume cannot be opened or
    // the model needs a microcode control storage processor.
    explicit Machine(const configuration::EmulatorConfig& cfg);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    const configuration::EmulatorConfig config;
    MachineState state;
    Scheduler scheduler;
    monitor::Tracer trace;

    storage::DiskBackend& diskBackend() { return *disk_; }
    devices::DeviceSet& devices() { return *devices_; }
    processors::controlstorage::IControlStorageProcessor& controlStorage() { return *csp_; }
    // The MSP is reached THROUGH the control processor that owns it; this
    // forwarder exists for the monitor's convenience.
    processors::MainStorageProcessor& msp() { return csp_->mainStorage(); }

    bool vtocsRead() const { return vtocsRead_; }
    const std::vector<storage::VtocEntry>& systemVtoc() const { return systemVtoc_; }
    const std::vector<storage::VtocEntry>& userVtoc() const { return userVtoc_; }
    void readVtocs();
    // Match ignoring case and any leading '#', which the system-area labels
    // do not store literally.
    const storage::VtocEntry* find(const std::string& name) const;

    std::vector<std::string> mediaLines() const;

    // Power-on order, as IBM performs it: the control processor comes up
    // first (stage A), then performs the main storage IPL (stage B).
    void reset();

private:
    std::unique_ptr<storage::DiskBackend> disk_;
    std::unique_ptr<devices::DeviceSet> devices_;
    std::unique_ptr<processors::controlstorage::As36ControlStorageProcessor> csp_;
    std::vector<storage::VtocEntry> systemVtoc_, userVtoc_;
    bool vtocsRead_ = false;
};

}  // namespace sim36::machine
