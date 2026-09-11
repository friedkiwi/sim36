#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <fmt/format.h>

namespace sim36::processors::controlstorage {

As36ControlStorageProcessor::As36ControlStorageProcessor(machine::MachineState& m,
                                                         const configuration::EmulatorConfig& cfg,
                                                         devices::DeviceSet& devices, storage::DiskBackend& disk,
                                                         monitor::Tracer& trace)
    : m_(m), cfg_(cfg), devices_(devices), disk_(disk), trace_(trace),
      msp_(std::make_unique<MainStorageProcessor>(m, *this, trace)) {}

// Stage A, vacuous by construction: there is no control storage to load
// microcode into and no microcode to load.
void As36ControlStorageProcessor::bringUpControlProcessor()
{
    devices_.resetPendingIo();
    trace_.csp("control processor bring-up: native, no microcode to load");
}

void As36ControlStorageProcessor::iplMainProcessor()
{
    // Not yet ported.  Nothing below is fabricated: no low storage, no phase
    // 1 image and no task block are written, and the MSP is not started.
    trace_.line("csp", "main storage IPL is not ported yet (milestone 4): guest low storage, "
                       "phase 1 at {:04X} and the initial task block are not built", kPhase1LoadAddress);
    lastRefusal_ = "main storage IPL is not ported yet (milestone 4)";
}

DispatchClass As36ControlStorageProcessor::classify(uint8_t) const { return DispatchClass::Immediate; }

bool As36ControlStorageProcessor::isImplemented(uint8_t) const { return false; }

bool As36ControlStorageProcessor::svc(SvcRequest& req)
{
    lastRefusal_ = fmt::format("SVC {:02X}: the control storage processor is not ported yet (milestone 4)", req.r);
    return false;
}

bool As36ControlStorageProcessor::raiseStorageProtection(uint16_t, bool) { return false; }

}  // namespace sim36::processors::controlstorage
