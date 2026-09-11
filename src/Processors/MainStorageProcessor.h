// The main storage processor.  Milestone 2 carries only its execution state
// as the monitor reports it; the decoder and the 28 instructions are
// milestone 3.
#pragma once

#include <string>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"

namespace sim36::processors {

namespace controlstorage { class IControlStorageProcessor; }

class MainStorageProcessor {
public:
    MainStorageProcessor(machine::MachineState& m, controlstorage::IControlStorageProcessor& csp,
                         monitor::Tracer& trace)
        : m_(m), csp_(csp), trace_(trace) {}

    bool stopped() const { return stopped_; }
    const std::string& stopReason() const { return stopReason_; }
    long long instructionsExecuted() const { return instructions_; }
    bool atPreemptionPoint() const { return atPreemptionPoint_; }

    void reset();
    void start() { stopped_ = false; stopReason_.clear(); }
    void stop(const std::string& reason) { stopped_ = true; stopReason_ = reason; }

private:
    machine::MachineState& m_;
    controlstorage::IControlStorageProcessor& csp_;
    monitor::Tracer& trace_;
    bool stopped_ = false;
    std::string stopReason_;
    long long instructions_ = 0;
    bool atPreemptionPoint_ = true;
};

}  // namespace sim36::processors
