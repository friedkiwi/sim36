// Everything wired together: one object the monitor can inspect.
#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Devices/VirtualPrinter.h"
#include "Devices/VirtualWorkstation.h"
#include "Host/StationBackend.h"
#include "Host/StationMultiplexer.h"
#include "Machine/MachineState.h"
#include "Machine/Scheduler.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Storage/DiskBackend.h"
#include "Storage/Vtoc.h"
#include "Monitor/MachineSnapshot.h"

namespace sim36::monitor { class MachineSnapshot; }

namespace sim36::machine {

class Machine {
public:
    // Constructs the machine from a validated definition.  Throws
    // std::runtime_error (a host layer) when the volume cannot be opened or
    // the model needs a microcode control storage processor.  The session
    // normally supplies persistent host backends so listeners and clients
    // exist before and across machine lifetimes; without them the machine
    // owns its own.
    using SessionBackends = std::map<std::string, std::unique_ptr<host::StationBackend>>;
    explicit Machine(const configuration::EmulatorConfig& cfg, const SessionBackends* sessionBackends = nullptr);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Mutable: `listener-auto-signon` changes the running machine's policy.
    configuration::EmulatorConfig config;
    MachineState state;
    Scheduler scheduler;
    monitor::Tracer trace;

    storage::DiskBackend& diskBackend() { return *disk_; }
    devices::DeviceSet& devices() { return *devices_; }
    processors::controlstorage::IControlStorageProcessor& controlStorage() { return *csp_; }
    // The native model, for the monitor commands that reach past the
    // architected interface (the current request block, the ACE pool).
    processors::controlstorage::As36ControlStorageProcessor& nativeControlStorage() { return *csp_; }
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

    // The guest-facing station models, in configuration order.
    std::vector<std::unique_ptr<devices::VirtualWorkstation>>& stations() { return stations_; }
    std::vector<std::unique_ptr<devices::VirtualPrinter>>& printers() { return printers_; }
    devices::VirtualWorkstation* findStation(const std::string& id);
    devices::VirtualPrinter* findPrinter(const std::string& id);

    // Open the per-station listeners a machine-owned backend has; with the
    // multiplexer on, the per-station display listeners stay closed.
    void startListeners();
    // What the station multiplexer asks of a machine: every display station
    // in W order.
    std::vector<host::MultiplexStationView> multiplexStations();

    // The native-event doorbell: socket and session threads ring it, and the
    // driver loop parked in its idle wait wakes to consume their latched
    // state on the guest thread.
    void signalNativeEvent();
    // False when the wait timed out; a negative count waits indefinitely.
    bool waitForNativeEvent(int milliseconds);

    // Power-on order, as IBM performs it: the control processor comes up
    // first (stage A), then performs the main storage IPL (stage B).
    void reset();

    // Restore the volatile state a checkpoint carries into this freshly
    // constructed machine.  False with the reason when the record does not
    // fit this machine; nothing is restored piecemeal beyond that point.
    bool restoreCheckpoint(const monitor::MachineSnapshot::RuntimeState& s, std::string& failure);

private:
    std::unique_ptr<storage::DiskBackend> disk_;
    std::unique_ptr<devices::DeviceSet> devices_;
    std::unique_ptr<processors::controlstorage::As36ControlStorageProcessor> csp_;
    std::vector<storage::VtocEntry> systemVtoc_, userVtoc_;
    bool vtocsRead_ = false;
    std::vector<std::unique_ptr<devices::VirtualWorkstation>> stations_;
    std::vector<std::unique_ptr<devices::VirtualPrinter>> printers_;
    std::mutex nativeEventGate_;
    std::condition_variable nativeEvent_;
    bool nativeEventSet_ = false;
};

}  // namespace sim36::machine
