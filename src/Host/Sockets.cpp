#include "Host/Sockets.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sim36::host {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
NativeSocket native(SocketHandle s) { return static_cast<SOCKET>(s); }
#else
using NativeSocket = int;
NativeSocket native(SocketHandle s) { return s; }
#endif

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

std::string peerText(const sockaddr_in& addr)
{
    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), text, sizeof text) == nullptr) return "?";
    return std::string(text) + ":" + std::to_string(ntohs(addr.sin_port));
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
    NativeSocket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == native(kInvalidSocket)) {
        error = lastError();
        return kInvalidSocket;
    }
    // SO_REUSEADDR lets a bind reclaim a port in TIME_WAIT left by a
    // previous emulator that exited a moment ago; it does not permit two live
    // listeners on one port, so a genuine conflict still surfaces.
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 16) != 0) {
        error = lastError();
        close(static_cast<SocketHandle>(s));
        return kInvalidSocket;
    }
    return static_cast<SocketHandle>(s);
}

bool Sockets::waitReadable(SocketHandle s, int milliseconds)
{
    if (s == kInvalidSocket) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(native(s), &readable);
    fd_set failed;
    FD_ZERO(&failed);
    FD_SET(native(s), &failed);
    timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
#ifdef _WIN32
    int n = select(0, &readable, nullptr, &failed, &tv);
#else
    int n = select(native(s) + 1, &readable, nullptr, &failed, &tv);
#endif
    if (n < 0) return true;   // let the caller's recv/accept report the error
    return n > 0;
}

SocketHandle Sockets::accept(SocketHandle listener, std::string& peer)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
#ifdef _WIN32
    int len = sizeof addr;
#else
    socklen_t len = sizeof addr;
#endif
    NativeSocket s = ::accept(native(listener), reinterpret_cast<sockaddr*>(&addr), &len);
    if (s == native(kInvalidSocket)) {
        peer.clear();
        return kInvalidSocket;
    }
    peer = peerText(addr);
    return static_cast<SocketHandle>(s);
}

int Sockets::recv(SocketHandle s, uint8_t* buffer, int length)
{
#ifdef _WIN32
    int got = ::recv(native(s), reinterpret_cast<char*>(buffer), length, 0);
    return got == SOCKET_ERROR ? -1 : got;
#else
    for (;;) {
        ssize_t got = ::recv(s, buffer, static_cast<size_t>(length), 0);
        if (got < 0 && errno == EINTR) continue;
        return got < 0 ? -1 : static_cast<int>(got);
    }
#endif
}

bool Sockets::sendAll(SocketHandle s, const uint8_t* buffer, int length)
{
    int sent = 0;
    while (sent < length) {
#ifdef _WIN32
        int n = ::send(native(s), reinterpret_cast<const char*>(buffer + sent), length - sent, 0);
        if (n == SOCKET_ERROR) return false;
#else
        ssize_t n = ::send(s, buffer + sent, static_cast<size_t>(length - sent), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
#endif
        sent += static_cast<int>(n);
    }
    return true;
}

void Sockets::setNoDelay(SocketHandle s)
{
    int one = 1;
    setsockopt(native(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

void Sockets::shutdown(SocketHandle s)
{
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(native(s), SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

void Sockets::close(SocketHandle s)
{
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(native(s));
#else
    ::close(s);
#endif
}

}  // namespace sim36::host
