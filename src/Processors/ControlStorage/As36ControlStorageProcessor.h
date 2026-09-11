// The Advanced/36 control storage processor: the architected contract in
// native code, with no CSP hardware and no microcode.
//
// Milestone 2 carries the shell: construction, the model name and the stage
// A bring-up line.  Stage B (guest low storage, phase 1, the initial task)
// is milestone 4 and is refused by name until then; the SVC families are
// milestone 5.
#pragma once

#include <memory>
#include <string>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"
#include "Processors/MainStorageProcessor.h"
#include "Storage/DiskBackend.h"

namespace sim36::processors::controlstorage {

class As36ControlStorageProcessor : public IControlStorageProcessor {
public:
    // Phase 1 is the boot record plus the 15 sectors after it, read as one
    // flat 4 KB control storage transient.  0-based, so 8191.
    static constexpr int kPhase1Sector = 8191;
    static constexpr int kPhase1Sectors = 16;
    // Where phase 1 lands: the boot record carries it at +0x0F and every IPL
    // member's directory link field is 1000.
    static constexpr int kPhase1LoadAddress = 0x1000;

    As36ControlStorageProcessor(machine::MachineState& m, const configuration::EmulatorConfig& cfg,
                                devices::DeviceSet& devices, storage::DiskBackend& disk, monitor::Tracer& trace);

    std::string modelName() const override { return "advanced36"; }
    MainStorageProcessor& mainStorage() override { return *msp_; }
    void bringUpControlProcessor() override;
    void iplMainProcessor() override;
    void controlStorageTerminate() override {}
    DispatchClass classify(uint8_t rByte) const override;
    bool isImplemented(uint8_t rByte) const override;
    bool svc(SvcRequest& req) override;
    std::string lastRefusal() const override { return lastRefusal_; }
    bool raiseStorageProtection(uint16_t logical, bool forWrite) override;
    ITransientArea& transients() override { return transients_; }

private:
    class Transients : public ITransientArea {
    public:
        bool busy() const override { return false; }
        int queueDepth() const override { return 0; }
        void schedule(uint8_t, uint8_t, uint8_t, int, int, int, int) override {}
        void setNotBusy() override {}
    };

    machine::MachineState& m_;
    const configuration::EmulatorConfig& cfg_;
    devices::DeviceSet& devices_;
    storage::DiskBackend& disk_;
    monitor::Tracer& trace_;
    Transients transients_;
    std::unique_ptr<MainStorageProcessor> msp_;
    std::string lastRefusal_;
};

}  // namespace sim36::processors::controlstorage
