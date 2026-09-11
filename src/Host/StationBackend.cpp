#include "Host/StationBackend.h"

#include <stdexcept>

namespace sim36::host {

StationBackend::StationBackend(StationKind kind, const std::string& host, int port,
                               const std::string& label, monitor::Tracer* trace,
                               std::function<void()> signalMachine)
    : trace_(trace), kind_(kind), host_(host), port_(port), label_(label),
      signalMachine_(std::move(signalMachine)) {}

StationBackend::~StationBackend() { Sockets::close(listener_); }

void StationBackend::listen()
{
    if (port_ == 0) return;
    std::string error;
    listener_ = Sockets::listenOn(host_, port_, error);
    if (listener_ == kInvalidSocket)
        throw std::runtime_error(label_ + ": cannot listen on " + endpoint() + ": " + error);
    trace_->ws("{}: listening on {} for a {} client", label_, endpoint(),
               kind_ == StationKind::Printer ? "printer" : "display");
}

void StationBackend::bindMachine(monitor::Tracer* trace, std::function<void()> signalMachine)
{
    trace_ = trace;
    signalMachine_ = std::move(signalMachine);
}

}  // namespace sim36::host
