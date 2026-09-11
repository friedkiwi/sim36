#include "Host/StationMultiplexer.h"

#include <stdexcept>

namespace sim36::host {

StationMultiplexer::StationMultiplexer(const std::string& host, int port, monitor::Tracer* trace)
    : host_(host.empty() ? kDefaultHost : host), port_(port == 0 ? kDefaultPort : port), trace_(trace) {}

StationMultiplexer::~StationMultiplexer() { stopListening(); }

void StationMultiplexer::listen()
{
    std::string error;
    listener_ = Sockets::listenOn(host_, port_, error);
    if (listener_ == kInvalidSocket)
        throw std::runtime_error("multiplexer: cannot listen on " + endpoint() + ": " + error);
    trace_->ws("multiplexer: listening on {}", endpoint());
}

void StationMultiplexer::stopListening()
{
    Sockets::close(listener_);
    listener_ = kInvalidSocket;
}

void StationMultiplexer::rebind(const std::string& host, int port)
{
    stopListening();
    host_ = host.empty() ? kDefaultHost : host;
    port_ = port == 0 ? kDefaultPort : port;
    listen();
}

}  // namespace sim36::host
