// The default display transport: one listening socket that serves every
// display station.  A client connects, negotiates 5250 the ordinary way, is
// painted a menu of the machine's stations, picks one, and its
// already-negotiated session is HANDED OVER to that station's backend.  A
// client that supplied an RFC 2877 section 4 DEVNAME naming an available
// station never sees the menu at all.
//
// It is host-side transport and nothing else: it writes no guest storage,
// fabricates no terminal unit block, and marks no station acquired.  The
// only state it changes is which socket a backend holds, which is exactly
// what an ordinary telnet connect changes.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Host/Sockets.h"
#include "Host/StationBackend.h"
#include "Host/Telnet5250Session.h"
#include "Monitor/Tracer.h"

namespace sim36::host {

// One selectable attachment, as the multiplexer's menu sees it.  Built on
// the guest-independent side of the seam: nothing here is a System/36
// concept beyond the station's own id.
struct MultiplexStationView {
    // The W number: the 1-based index of this station among the machine's
    // DISPLAY stations, in the controller's own port/address grid order.
    int number = 0;
    // port.address, the id the rest of the emulator uses.
    std::string id;
    bool isConsole = false;
    // No multiplexer client is attached.
    bool available = false;
    // Where a chosen session is handed over.  nullptr before machine
    // construction, when the selected session is parked until IPL.
    StationBackend* backend = nullptr;
};

// What the multiplexer needs from the machine: an interface rather than a
// machine reference so the transport layer keeps not knowing what a
// machine is.
class IStationMultiplexerHost {
public:
    virtual ~IStationMultiplexerHost() = default;
    // Every display station this machine has, in W order.  Enumerated fresh
    // on each call: the count, the selectable range and the "next
    // available" default are all derived from the configured machine.
    virtual std::vector<MultiplexStationView> multiplexStations() = 0;
    // "running" when a guest thread owns the machine, "stopped" otherwise.
    virtual std::string machineStatusText() = 0;
    // One line per attached medium, already formatted.
    virtual std::vector<std::string> mediaLines() = 0;
};

class StationMultiplexer {
public:
    static constexpr int kDefaultPort = 2300;
    static constexpr const char* kDefaultHost = "127.0.0.1";

    // The host may be supplied later through setHost: the multiplexer starts
    // the moment `set terminal multiplex on` runs, with no machine yet.
    StationMultiplexer(const std::string& host, int port, monitor::Tracer* trace,
                       IStationMultiplexerHost* machine = nullptr);
    ~StationMultiplexer();
    StationMultiplexer(const StationMultiplexer&) = delete;
    StationMultiplexer& operator=(const StationMultiplexer&) = delete;

    void setHost(IStationMultiplexerHost* machine) { machine_ = machine; }

    std::string endpoint() const { return host_ + ":" + std::to_string(port_); }
    bool listening() const { return listener_ != kInvalidSocket; }
    long long sessionsAccepted() const { return sessionsAccepted_; }
    long long sessionsHandedOver() const { return sessionsHandedOver_; }

    // Is this station free of a multiplexer-placed client?
    bool isFree(const std::string& stationId);

    // IPL construction: put every client the multiplexer placed into the
    // newly constructed machine's backends, without reconnecting.
    void rebind();
    // `reset`: take the live sessions back out of the backends BEFORE the
    // machine disposes its guest side.
    void reclaim();
    // Move the listener to a new endpoint without disturbing the clients
    // already placed on stations.
    void rebind(const std::string& host, int port);
    void listen();
    // `set terminal multiplex off`, and monitor shutdown: clients placed on
    // stations are CLOSED, with a trace line each.
    void dispose();

    // Resolve what a client typed, or what its DEVNAME said: "W<n>" (the W
    // number) or "port.address".  A leading letter other than W parses but
    // is not acted on.
    static bool tryParseSelection(const std::string& text, char& kind, int& number, std::string& stationId);

private:
    class Conversation;
    class ScreenBuilder;
    friend class Conversation;

    void acceptLoop();
    void stopListening();
    void bind(const std::string& stationId, std::shared_ptr<Telnet5250Session> session);
    void forget(Conversation* conversation);
    static const MultiplexStationView* lookup(const std::vector<MultiplexStationView>& stations, char kind, int number,
                                              const std::string& stationId);

    std::string host_;
    int port_;
    monitor::Tracer* trace_;
    IStationMultiplexerHost* machine_;
    SocketHandle listener_ = kInvalidSocket;
    std::thread accept_;
    std::atomic<bool> stopping_{false};
    std::atomic<long long> sessionsAccepted_{0}, sessionsHandedOver_{0};

    // Which station each client the multiplexer placed is sitting on, by
    // port.address.  THE MULTIPLEXER OUTLIVES THE MACHINE, so a placement is
    // recorded here and survives `reset`: reclaim takes the live sessions
    // back before the machine is disposed and rebind puts them into the
    // replacement machine's backends.
    std::mutex bindGate_;
    std::map<std::string, std::shared_ptr<Telnet5250Session>> bound_;
    // Clients still on the menu (or parked), each owning its conversation.
    std::mutex conversationGate_;
    std::map<Conversation*, std::shared_ptr<Conversation>> conversations_;
};

}  // namespace sim36::host
