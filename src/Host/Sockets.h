// A small shim over BSD sockets and Winsock: initialisation, a listening
// socket bound to host:port with SO_REUSEADDR, and close.
#pragma once

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
    // Idempotent; Winsock needs WSAStartup, POSIX needs nothing.
    static void initialize();

    // Binds and listens.  On failure returns kInvalidSocket and fills
    // `error` with the operating system's message.
    static SocketHandle listenOn(const std::string& host, int port, std::string& error);

    static void close(SocketHandle s);
};

}  // namespace sim36::host
