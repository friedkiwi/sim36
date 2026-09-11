// One listener for every display station.  A client connects, is offered
// the machine's stations, picks one, and its negotiated session is handed to
// that station's backend.  It lives on the monitor chassis, not the machine,
// so a client can be sitting on a station before the IPL runs and the
// placement survives `reset`.
//
// Milestone 1 carries the listener and its bookkeeping surface; the 5250
// conversation that paints the selection panel arrives in milestone 6.
#pragma once

#include <string>

#include "Host/Sockets.h"
#include "Monitor/Tracer.h"

namespace sim36::host {

class StationMultiplexer {
public:
    static constexpr int kDefaultPort = 2300;
    static constexpr const char* kDefaultHost = "127.0.0.1";

    StationMultiplexer(const std::string& host, int port, monitor::Tracer* trace);
    ~StationMultiplexer();
    StationMultiplexer(const StationMultiplexer&) = delete;
    StationMultiplexer& operator=(const StationMultiplexer&) = delete;

    std::string endpoint() const { return host_ + ":" + std::to_string(port_); }
    bool listening() const { return listener_ != kInvalidSocket; }

    // Is this station free of a multiplexer-placed client?
    bool isFree(const std::string& stationId) const { (void)stationId; return true; }

    void listen();
    // Move the listener to a new endpoint without disturbing placed clients.
    void rebind(const std::string& host, int port);
    // Take placed clients back out of the machine's backends (before reset).
    void reclaim() {}
    // Put placed clients into a newly constructed machine's backends.
    void rebind() {}

private:
    void stopListening();

    std::string host_;
    int port_;
    monitor::Tracer* trace_;
    SocketHandle listener_ = kInvalidSocket;
};

}  // namespace sim36::host
