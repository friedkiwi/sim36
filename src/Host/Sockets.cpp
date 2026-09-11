#include "Host/Sockets.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sim36::host {

namespace {

std::string lastError()
{
#ifdef _WIN32
    int code = WSAGetLastError();
    char* text = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code),
                             0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string s = n != 0 && text != nullptr ? std::string(text, n) : "socket error " + std::to_string(code);
    if (text != nullptr) LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
#else
    return std::strerror(errno);
#endif
}

}  // namespace

void Sockets::initialize()
{
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

SocketHandle Sockets::listenOn(const std::string& host, int port, std::string& error)
{
    initialize();
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        error = "invalid address " + host;
        return kInvalidSocket;
    }
    SocketHandle s = static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, 0));
    if (s == kInvalidSocket) {
        error = lastError();
        return kInvalidSocket;
    }
    // SO_REUSEADDR lets a bind reclaim a port in TIME_WAIT left by a
    // previous emulator that exited a moment ago.
    int one = 1;
    setsockopt(static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof one);
    if (bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(static_cast<int>(s), 16) != 0) {
        error = lastError();
        close(s);
        return kInvalidSocket;
    }
    return s;
}

void Sockets::close(SocketHandle s)
{
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(s));
#else
    ::close(s);
#endif
}

}  // namespace sim36::host
