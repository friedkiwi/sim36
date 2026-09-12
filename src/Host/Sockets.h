// The one place the host layer touches BSD sockets, with the Winsock shim
// behind the same names.  Everything above this is transport logic that
// does not care which socket library it runs on.
#pragma once

#include <cstdint>
#include <string>

namespace sim36::host {

#ifdef _WIN32
using SocketHandle = unsigned long long;   // SOCKET
constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

class Sockets {
public:
    static void initialize();

    // A bound, listening TCP socket on host:port, SO_REUSEADDR set so a bind
    // can reclaim a port left in TIME_WAIT by an emulator that exited a
    // moment ago.  kInvalidSocket and a message on failure.
    static SocketHandle listenOn(const std::string& host, int port, std::string& error);

    // Wait up to `milliseconds` for the socket to become readable (or, for a
    // listener, to have a pending connection).  False on timeout; true when
    // readable or when the socket is in error, so a caller always proceeds
    // to the call that reports the error.
    static bool waitReadable(SocketHandle s, int milliseconds);

    // Accept one connection; `peer` receives "address:port".  kInvalidSocket
    // when the listener is closed or in error.
    static SocketHandle accept(SocketHandle listener, std::string& peer);

    // Bytes received; 0 when the peer closed; negative on error.
    static int recv(SocketHandle s, uint8_t* buffer, int length);

    // Write the whole buffer, looping over partial sends.  False on error.
    static bool sendAll(SocketHandle s, const uint8_t* buffer, int length);

    static void setNoDelay(SocketHandle s);

    // Shut both directions down, which wakes a reader blocked on the socket,
    // then release the handle.
    static void shutdown(SocketHandle s);
    static void close(SocketHandle s);
};

}  // namespace sim36::host
