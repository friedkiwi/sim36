// The host side of a display or printer station: one listening socket, at
// most one attached session, records in and records out.
//
// This is the RFC 1205/Telnet transport seam, and it is deliberately
// primitive.  Nothing here mentions an IOB, an ACE or an SVC; a microcode
// control storage processor modelling the work station IOP below itself
// would drive exactly these methods, because a record of 5250 data stream
// bytes is what the wire carries whichever processor built it.
//
// What the backend owns: the listening socket and the attached client;
// Telnet option negotiation (RFC 1205 section 2, RFC 2877 section 3); the
// RFC 1205 section 3 record header, IAC doubling and IAC EOR.  What it does
// not own: the station's port and address, its device code, its unit block,
// which IOB command produced a record or which opcode a command maps to.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Host/ConsoleDisplay.h"
#include "Host/Sockets.h"
#include "Host/Telnet5250Session.h"
#include "Monitor/Tracer.h"

namespace sim36::host {

// The half of a station's host side that is the same whichever kind it is:
// one listening socket, at most one attached session, and the accounting.
// A printer endpoint and a display endpoint differ ONLY in how records are
// framed and what flows over them; the negotiation, the one-session-per-
// endpoint rule and the attach/detach lifecycle are identical.
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
    virtual bool attached() const;
    // True once Telnet negotiation has reached 5250 mode (RFC 1205 section
    // 2: Binary, End-Of-Record and Terminal-Type agreed).  Records sent
    // before this are refused rather than written.
    virtual bool ready() const;
    // The terminal type the client answered with ("IBM-3180-2" for a
    // display, "IBM-3812-1" for a printer); empty until it has.
    std::string terminalType() const;
    // RFC 2877 section 4's DEVNAME, empty when the client sent none.
    std::string deviceName() const;
    // RFC 1572's USER, empty when the client sent none; recorded only,
    // because sign-on happens inside SSP.
    std::string userName() const;

    // The power-off latch: a client hanging up is the twinax display being
    // switched off.  Set on whichever thread notices the socket is gone,
    // consumed on the guest thread.
    bool powerOffPending() const { return powerOffPending_; }
    // Guest thread only: read and clear the power-off latch.
    bool takePowerOffPending();
    // Read-only view of the device-attention latch, safe for diagnostics
    // because it does not consume the edge.
    bool attentionPending() const { return attentionPending_; }
    // Guest thread only: read and clear the attention latch.
    bool takeAttentionPending();

    long long sessionsAccepted() const { return sessionsAccepted_; }
    long long sessionsRefused() const { return sessionsRefused_; }
    long long sessionsRejected() const { return sessionsRejected_; }
    long long recordsSent() const { return recordsSent_; }
    long long recordsDropped() const { return recordsDropped_; }
    long long bytesSent() const { return bytesSent_; }
    long long recordsReceived() const { return recordsReceived_; }
    long long bytesReceived() const { return bytesReceived_; }

    virtual void listen();
    // Move a session-owned listener without reconstructing a guest machine.
    // An attached client is necessarily disconnected because its TCP
    // endpoint cannot migrate.
    void rebind(const std::string& host, int port, monitor::Tracer* trace, std::function<void()> signalMachine);
    // Retarget a persistent listener to the currently constructed machine's
    // trace and native-event doorbell, or back to the chassis.
    void bindMachine(monitor::Tracer* trace, std::function<void()> signalMachine);
    // Begin a new machine lifetime without disturbing the host listener or
    // its client.  An attached device is presented to the new adapter as an
    // attention edge, just as a device connected before a physical IPL is
    // present when the controller starts polling.
    virtual void resetForMachine();

    // Take over a session that some other listener negotiated: the station
    // multiplexer's handover.  The session arrives ALREADY in 5250 mode, so
    // the present edge an ordinary accept raises is raised here instead.
    // False when this endpoint already has a live client.
    virtual bool adoptSession(std::shared_ptr<Telnet5250Session> session);
    // Hand the live session back to whoever gave it to us, WITHOUT closing
    // it (the other half of adoptSession, for `reset`).  nullptr when there
    // is no live session to give back.
    virtual std::shared_ptr<Telnet5250Session> releaseSession();
    // Detach without destroying the endpoint: a slot persists across
    // disconnects, it is not created and destroyed.
    virtual void detach();
    virtual void dispose();

protected:
    // The session that owned this endpoint has gone away.  Only a session
    // that reached 5250 mode was ever present to the guest, so only that
    // one powers off.
    virtual void onSessionGone(const std::shared_ptr<Telnet5250Session>& session);
    // Accept/session thread: record that this device raised an I/O
    // attention.  Touches no guest storage, just the latch.
    void raiseAttention();
    // Wake the machine's guest-thread event pump after host-side state
    // changes; never touches guest storage.
    void signalMachine();
    void resetAttention(bool pending);
    std::shared_ptr<Telnet5250Session> session() const;
    // Whether a network client, as opposed to an intrinsic device attachment
    // such as the operator console, currently owns this endpoint.
    bool sessionAttached() const;
    // One complete record arrived from the client.
    virtual void onRecord(const WorkstationRecord& record) = 0;
    // Negotiation reached 5250 mode.  Called once per session.
    virtual void onReady() {}
    // The ordinary display attachment raises a device-present edge.
    virtual void sessionAdopted() { sessionReady(); }
    void countReceived(const WorkstationRecord& r);
    // Put an already-framed logical record on the wire.  False, and a
    // counted drop, when there is nowhere to put it.
    bool sendRecord(const std::vector<uint8_t>& record, int payloadBytes, const std::string& what);

    monitor::Tracer* trace_;
    std::atomic<long long> recordsSent_{0}, recordsDropped_{0}, bytesSent_{0}, recordsReceived_{0}, bytesReceived_{0};

private:
    void acceptLoop();
    void stopListener();
    void onKindMismatch(const std::string& terminalType, StationKind announced);
    void sessionReady();
    void installSession(std::shared_ptr<Telnet5250Session> session);

    StationKind kind_;
    std::string host_;
    int port_;
    std::string label_;
    std::function<void()> signalMachine_;
    SocketHandle listener_ = kInvalidSocket;
    std::thread accept_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex sessionGate_;
    std::shared_ptr<Telnet5250Session> session_;
    std::atomic<bool> attentionPending_{false};
    std::atomic<bool> powerOffPending_{false};
    std::atomic<long long> sessionsAccepted_{0}, sessionsRefused_{0}, sessionsRejected_{0};
};

// A display station's host side.  The console attachment decodes to the
// operator interface instead of a socket; every other display is a telnet
// client.
class WorkstationBackend : public StationBackend {
public:
    // The park slot: the newest arrival overwrites the previous one, so the
    // drop policy is last-writer-wins.  The distinction between an
    // acceptable, parked and discarded response needs the outstanding-invite
    // state, which lives on the guest side of the single-owner boundary.
    static constexpr int kInboundLimit = 1;
    // Save-Screen replies are solicited, so they keep a real queue.
    static constexpr int kSaveResponseLimit = 8;

    struct DiagnosticRecord {
        long long sequence = 0;
        std::string direction;
        std::string disposition;
        WorkstationOpcode opcode = WorkstationOpcode::NoOperation;
        WorkstationRecordFlags flags = WorkstationRecordFlags::None;
        std::vector<uint8_t> data;
    };
    struct DiagnosticQueues {
        std::vector<WorkstationRecord> input;
        std::vector<WorkstationRecord> saveScreen;
        bool headStatusTaken = false;
    };

    WorkstationBackend(const std::string& host, int port, const std::string& label, monitor::Tracer* trace,
                       std::function<void()> signalMachine)
        : StationBackend(StationKind::Display, host, port, label, trace, std::move(signalMachine)) {}
    ~WorkstationBackend() override;

    // The console attachment's decoder; nullptr for an ordinary display.
    ConsoleDisplay* console() { return console_.get(); }
    bool isConsoleAttachment() const { return console_ != nullptr; }
    // A TN5250 client may mirror and operate the intrinsic system console
    // without replacing its monitor-side attachment.
    bool consoleClientAttached() const { return sessionAttached(); }
    // Attach this station to the emulator's operator interface rather than
    // to a listener: work-station address 0.0 is the console on every 5250
    // machine, it must be present for the IPL to complete, so it reports
    // itself present from power-on and never negotiates.
    void attachConsole() { console_ = std::make_unique<ConsoleDisplay>(); }
    // Host-side half of a display power-off: whatever the departed client
    // left parked can never be read by the guest, and the next client starts
    // with input disabled until the guest invites it.
    void clearForPowerOff();

    bool adoptSession(std::shared_ptr<Telnet5250Session> session) override;
    std::shared_ptr<Telnet5250Session> releaseSession() override;
    bool listening() const override { return isConsoleAttachment() || StationBackend::listening(); }
    bool attached() const override { return isConsoleAttachment() || StationBackend::attached(); }
    bool ready() const override { return isConsoleAttachment() || StationBackend::ready(); }
    void listen() override { if (!isConsoleAttachment()) StationBackend::listen(); }
    void resetForMachine() override;

    int pendingInput() const;
    int pendingSaveScreens() const;
    // A bounded, non-consuming record of the wire-level terminal exchange.
    std::vector<DiagnosticRecord> captureDiagnosticHistory() const;
    // Non-consuming deep copy of both wire queues for a panic dump.
    DiagnosticQueues captureDiagnosticQueues() const;

    // Input enabled for this endpoint.  It is held here because it SELECTS
    // THE OPCODE: a data stream sent with input enabled is Put/Get, without
    // it Output Only (RFC 1205 section 4).
    bool inputEnabled() const { return inputEnabled_; }
    bool setInputEnabled(bool enabled);
    // Inject one operator response from the monitor, through the SAME
    // inbound path a telnet reader uses.
    bool injectConsoleInput(uint8_t aid);
    bool sendDataStream(const uint8_t* data, int offset, int length);
    // One atomic display operation.  PUT-with-invite is RFC 1205 Put/Get
    // carrying the write and its appended read command in the same logical
    // record.
    bool sendPut(const uint8_t* data, int offset, int length, bool withInvite);
    bool sendSaveScreen();
    bool sendRestoreScreen(const uint8_t* data, int offset, int length);
    bool sendSavedReadMode(uint8_t mode);
    // Take the next data stream the station sent, verbatim, or false.
    bool tryTakeInput(std::vector<uint8_t>& stream);
    // True when the record whose status is about to be reported carries
    // modified field bytes beyond the cursor and AID.
    bool pendingInputHasFields() const;
    // Return cursor/AID from the head record once, without dequeuing it.
    bool tryTakeInputStatus(uint16_t& cursor, uint8_t& aid);
    // Take an out-of-band RFC 1205 Attention/System Request control.  These
    // are not cursor/AID input and must never occupy the input-field park.
    bool tryTakeUnsolicitedRequest(WorkstationRecordFlags& flags);
    bool tryReceive(WorkstationRecord& record);
    bool tryTakeSaveScreen(std::vector<uint8_t>& body);
    // Harness entry point: the monitor's `wsinput` exercises the receive
    // path without a client attached, by the same route a client's records
    // take.
    void injectInput(const WorkstationRecord& record);

protected:
    void onSessionGone(const std::shared_ptr<Telnet5250Session>& session) override;
    void sessionAdopted() override;
    void onRecord(const WorkstationRecord& r) override { enqueue(r); }

private:
    void enqueue(const WorkstationRecord& r);
    void appendDiagnosticLocked(const char* direction, const char* disposition, WorkstationOpcode opcode,
                                WorkstationRecordFlags flags, const uint8_t* data, int offset, int length);
    bool send(WorkstationOpcode opcode, WorkstationRecordFlags flags, const uint8_t* data, int offset, int length);

    static constexpr int kDiagnosticHistoryLimit = 64;
    mutable std::mutex gate_;
    std::deque<WorkstationRecord> inbound_;
    std::deque<WorkstationRecord> saveResponses_;
    std::deque<DiagnosticRecord> history_;
    WorkstationRecordFlags unsolicitedRequest_ = WorkstationRecordFlags::None;
    bool headStatusTaken_ = false;
    long long historySequence_ = 0;
    std::atomic<bool> inputEnabled_{false};
    std::unique_ptr<ConsoleDisplay> console_;
};

// The host side of a printer station: one endpoint, one attached 5250
// printer client, and the RFC 2877 pass-through framing that client
// expects.  RFC 2877 gives printers their own record layout (section 10, a
// ten-octet pass-through header), their own startup handshake (section 9, a
// startup response record the server owes the client the moment negotiation
// completes) and their own acknowledgement (section 10.2, a print complete
// record for every print record).  What crosses the seam above is still
// bytes: the guest hands over a data stream by address (SA21-9436 5-50), and
// what goes into a print record is what SSP wrote.
class PrinterBackend : public StationBackend {
public:
    // Byte 9 of the pass-through header, RFC 2877 section 10: "'01'X
    // Print/Print complete", "'02'X Clear Print Buffers".
    static constexpr uint8_t kOpcodePrint = 0x01;
    static constexpr uint8_t kOpcodeClear = 0x02;
    // Byte 6 on a print record is 0A, giving six bytes of zero pad after
    // the opcode, so print data starts at byte 16.
    static constexpr uint8_t kPrintHeaderLength = 0x0A;
    static constexpr int kPrintRecordHeaderBytes = 6 + kPrintHeaderLength;
    // RFC 2877 section 9's response codes: I902 when a session starts.
    static constexpr const char* kResponseSessionStarted = "I902";

    PrinterBackend(const std::string& host, int port, const std::string& label, monitor::Tracer* trace,
                   std::function<void()> signalMachine, const std::string& output = "tn5250",
                   const std::string& outputPath = "")
        : StationBackend(StationKind::Printer, host, port, label, trace, std::move(signalMachine)),
          output_(output), outputPath_(outputPath) {}
    ~PrinterBackend() override;

    // The eight-character system name the startup response carries; nothing
    // on the volume supplies one, so it is the emulator's.
    std::string systemName = "S36REFEM";
    // The ten-character object name: the device this session is.  Set from
    // the station's identity by the device model above.
    std::string objectName = "PRT       ";

    long long startupResponsesSent() const { return startupResponsesSent_; }
    long long printCompletesReceived() const { return printCompletesReceived_; }
    long long jobsEnded() const { return jobsEnded_; }
    const std::string& output() const { return output_; }
    const std::string& outputPath() const { return outputPath_; }
    bool networkOutput() const { return output_ == "tn5250"; }
    bool listening() const override { return networkOutput() && StationBackend::listening(); }
    bool attached() const override { return networkOutput() ? StationBackend::attached() : true; }
    bool ready() const override { return networkOutput() ? StationBackend::ready() : true; }
    void listen() override { if (networkOutput()) StationBackend::listen(); }

    // RFC 2877 section 9's startup response record, built from figure 1
    // BYTE FOR BYTE: only the response code at +16, the system name at +20
    // and the object name at +28 are substituted.
    static std::vector<uint8_t> startupResponse(const std::string& code, const std::string& systemName,
                                                const std::string& objectName);
    // Render the SCS subset emitted by SVC 26 for the human-readable console
    // lister.  Network and file printers continue to receive the byte stream
    // verbatim.
    static std::vector<std::string> renderConsoleDataStream(const uint8_t* data, int offset, int length);
    // One print record (RFC 2877 section 10 figure 4), first and last of
    // chain because one Output Data command is one complete data stream.
    bool sendDataStream(const uint8_t* data, int offset, int length);
    // RFC 2877 section 10.3's null print record, "the last print command
    // the server sends to the client for a print job".  Nothing on the
    // guest path calls this; the monitor drives it, as policy.
    bool endJob();
    std::string name() const { return label(); }

protected:
    void onReady() override;
    void onRecord(const WorkstationRecord& r) override;

private:
    static std::vector<uint8_t> printRecord(const uint8_t* data, int offset, int length, uint8_t flags);
    static void putEbcdic(std::vector<uint8_t>& into, int at, const std::string& s, int width);
    static uint8_t ebcdic(char c);

    std::string output_;
    std::string outputPath_;
    std::ofstream outputFile_;
    // Console output is a stream: guest Output Data records can end in the
    // middle of either a word or an SCS control sequence.
    std::string consoleLine_;
    bool consoleIdeographic_ = false;
    std::vector<uint8_t> consolePending_;

    std::atomic<long long> startupResponsesSent_{0}, printCompletesReceived_{0}, jobsEnded_{0};
};

}  // namespace sim36::host
