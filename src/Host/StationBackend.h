// The host end of a station: one TCP endpoint per work station address.
//
// Milestone 1 carries the listener itself (bind and listen, so that the
// operator-visible endpoint state is truthful); the TN5250 session that
// accepts and negotiates a client arrives with the host layer in milestone 6.
#pragma once

#include <functional>
#include <string>

#include "Host/Sockets.h"
#include "Monitor/Tracer.h"

namespace sim36::host {

enum class StationKind { Display, Printer };

class StationBackend {
public:
    StationBackend(StationKind kind, const std::string& host, int port, const std::string& label,
                   monitor::Tracer* trace, std::function<void()> signalMachine);
    virtual ~StationBackend();
    StationBackend(const StationBackend&) = delete;
    StationBackend& operator=(const StationBackend&) = delete;

    StationKind kind() const { return kind_; }
    const std::string& label() const { return label_; }
    std::string endpoint() const { return host_ + ":" + std::to_string(port_); }
    int port() const { return port_; }

    // True once the listener is bound.  A port of zero means this endpoint
    // is not served, which is a legitimate configuration.
    virtual bool listening() const { return listener_ != kInvalidSocket; }
    virtual bool attached() const { return false; }
    virtual bool ready() const { return false; }
    // The terminal type the client answered with; empty when none.
    std::string terminalType() const { return std::string(); }

    virtual void listen();
    void bindMachine(monitor::Tracer* trace, std::function<void()> signalMachine);
    virtual void resetForMachine() {}

protected:
    monitor::Tracer* trace_;

private:
    StationKind kind_;
    std::string host_;
    int port_;
    std::string label_;
    std::function<void()> signalMachine_;
    SocketHandle listener_ = kInvalidSocket;
};

class WorkstationBackend : public StationBackend {
public:
    WorkstationBackend(const std::string& host, int port, const std::string& label,
                       monitor::Tracer* trace, std::function<void()> signalMachine)
        : StationBackend(StationKind::Display, host, port, label, trace, std::move(signalMachine)) {}

    // The operator console is an intrinsic attachment: it has no listener,
    // is present from power-on, and reports itself attached and ready.
    void attachConsole() { consoleAttachment_ = true; }
    bool isConsoleAttachment() const { return consoleAttachment_; }
    bool listening() const override { return consoleAttachment_ || StationBackend::listening(); }
    bool attached() const override { return consoleAttachment_; }
    bool ready() const override { return consoleAttachment_; }

private:
    bool consoleAttachment_ = false;
};

class PrinterBackend : public StationBackend {
public:
    PrinterBackend(const std::string& host, int port, const std::string& label,
                   monitor::Tracer* trace, std::function<void()> signalMachine)
        : StationBackend(StationKind::Printer, host, port, label, trace, std::move(signalMachine)) {}
};

}  // namespace sim36::host
