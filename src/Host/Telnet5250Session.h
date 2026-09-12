// One attached client, speaking the 5250 Telnet interface.
//
// This is transport and nothing else.  It knows Telnet option negotiation,
// the RFC 1205 section 3 record header and IAC escaping; it does not know
// what a work station is.  Everything here is implemented from the
// published specifications, and each rule cites the section it comes from:
//
//   RFC 854   Telnet protocol - IAC, WILL/WONT/DO/DONT, SB/SE
//   RFC 856   Binary Transmission (option 0)
//   RFC 885   End of Record (option 25, and the EOR command 239)
//   RFC 1091  Terminal-Type (option 24, IS/SEND)
//   RFC 1205  5250 Telnet Interface - which options 5250 mode needs, and
//             the General Data Stream record header
//   RFC 1572  New Environment (option 39, VAR/VALUE/USERVAR)
//   RFC 2877  5250 Telnet Enhancements - DEVNAME and the negotiation order
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Host/Sockets.h"
#include "Monitor/Tracer.h"

namespace sim36::host {

// What is attached at a work-station-bus address.  The kind is the
// ATTACHMENT, not a transport option: a Display is attached over telnet, the
// Console is attached to this emulator's own operator interface and never
// opens a socket, and a Printer is a different 5250 family with its own
// record framing.  A client announces its family in the TERMINAL-TYPE
// sub-negotiation (RFC 1091), and the family has to be known before a single
// record can be built.
enum class StationKind { Display, Printer, Console };

// The operation code in the RFC 1205 section 3 record header.  These are the
// 5250 Telnet interface's own opcodes and nothing more: they are NOT
// System/36 work station IOB commands, and the mapping between the two
// belongs to the device model above.
enum class WorkstationOpcode : uint8_t {
    NoOperation = 0x00,
    Invite = 0x01,
    OutputOnly = 0x02,
    PutGet = 0x03,
    SaveScreen = 0x04,
    RestoreScreen = 0x05,
    ReadImmediate = 0x06,
    ReadScreen = 0x08,
    CancelInvite = 0x0A,
    TurnOnMessageLight = 0x0B,
    TurnOffMessageLight = 0x0C
};

// The flags of the RFC 1205 section 3 variable header.  The field is
// sixteen bits and every flag the RFC defines lives in the first octet
// ("Bits 8-15: reserved"), so this models the first octet and the second is
// always written as zero.  IBM numbers bits from 0 = most significant, so
// RFC 1205's "Bit 0: ERR" is mask 0x80.
enum class WorkstationRecordFlags : uint8_t {
    None = 0x00,
    DataStreamOutputError = 0x80,   // ERR
    Attention = 0x40,               // ATN, the 5250 Attn key
    SystemRequest = 0x04,           // SRQ, the 5250 System Request key
    TestRequest = 0x02,             // TRQ
    HelpInErrorState = 0x01         // HLP
};

inline WorkstationRecordFlags operator|(WorkstationRecordFlags a, WorkstationRecordFlags b)
{
    return static_cast<WorkstationRecordFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline uint8_t flagsByte(WorkstationRecordFlags f) { return static_cast<uint8_t>(f); }
inline bool hasFlag(WorkstationRecordFlags f, WorkstationRecordFlags bit)
{
    return (static_cast<uint8_t>(f) & static_cast<uint8_t>(bit)) != 0;
}

// The names the reference prints for an opcode and a flag set: the opcode's
// enumerator name (or its number when unassigned), and the flags as "None"
// or the set names in ascending value order, comma separated.
std::string opcodeName(WorkstationOpcode opcode);
std::string flagsName(WorkstationRecordFlags flags);
// "00-1A-12-A0": bytes as upper-case hex joined by dashes.
std::string hexDash(const uint8_t* data, int length);

// One logical record: an RFC 1205 header and the 5250 work station data
// stream that follows it.  `data` is the data stream alone; the ten header
// octets and the trailing IAC EOR belong to the transport and never reach a
// caller.
struct WorkstationRecord {
    WorkstationOpcode opcode = WorkstationOpcode::NoOperation;
    WorkstationRecordFlags flags = WorkstationRecordFlags::None;
    std::vector<uint8_t> data;

    WorkstationRecord() = default;
    WorkstationRecord(WorkstationOpcode o, WorkstationRecordFlags f, std::vector<uint8_t> d)
        : opcode(o), flags(f), data(std::move(d)) {}

    std::string toString() const;
};

class Telnet5250Session : public std::enable_shared_from_this<Telnet5250Session> {
public:
    using DeliverFn = std::function<void(const WorkstationRecord&)>;
    using ReadyFn = std::function<void()>;
    using MismatchFn = std::function<void(const std::string&, StationKind)>;
    using ClosedFn = std::function<void()>;

    // RFC 1205 section 3: six octets fixed plus four variable.
    static constexpr int kHeaderLength = 10;

    // `client` is an accepted, connected socket; `peer` its "address:port".
    // The session owns the socket from here on.
    Telnet5250Session(SocketHandle client, std::string peer, std::string label, StationKind kind,
                      monitor::Tracer* trace, DeliverFn deliver, ReadyFn ready, MismatchFn mismatch);
    ~Telnet5250Session();
    Telnet5250Session(const Telnet5250Session&) = delete;
    Telnet5250Session& operator=(const Telnet5250Session&) = delete;

    // Invoked exactly once, on whichever thread notices the connection is
    // gone (the reader thread on a peer close or read failure, the caller's
    // thread on dispose or a failed write).  The owner uses it to latch "the
    // terminal was powered off"; the session itself draws no conclusion.
    // Not part of retarget: ownership of the socket, not the record
    // callbacks, is what this reports on.
    void setClosed(ClosedFn closed);

    // True once the ready callback has fired: the device was PRESENT to the
    // guest.  A client that hangs up during negotiation was never present
    // and its departure is not a power-off.
    bool readySignalled() const { return readySignalled_; }

    bool connected() const { return !closed_; }
    const std::string& peer() const { return peer_; }

    // Re-point this session's callbacks at a different owner, without
    // touching the socket or the negotiated Telnet state: the station
    // multiplexer's handover.  Above the transport seam nothing can tell the
    // result from a client that connected to the station's own listener.
    void retarget(DeliverFn deliver, ReadyFn ready, MismatchFn mismatch, const std::string& label);

    // The terminal type the client answered with; empty until it has.
    std::string terminalType() const;
    // RFC 2877 section 4's DEVNAME USERVAR, empty when the client sent none.
    std::string deviceName() const;
    // RFC 1572's USER standard VAR, empty when the client sent none.
    std::string userName() const;
    // Every environment string the client sent, whether or not this emulator
    // has a use for it.
    std::map<std::string, std::string> environment() const;

    // RFC 1205 section 2: 5250 mode needs Binary, End-Of-Record and
    // Terminal-Type agreed, both directions for the first two.
    bool ready() const;

    void start();

    // Frame and write one record: RFC 1205 section 3's six-octet fixed
    // header, four-octet variable header, the data stream, then IAC EOR.
    bool send(WorkstationOpcode opcode, WorkstationRecordFlags flags, const uint8_t* data, int offset, int length);
    // Write one logical record the caller has already framed, from its
    // length field onward: the printer records carry other variable-header
    // lengths, and only the IAC doubling and the terminating IAC EOR are
    // common to all.
    bool sendFramed(const std::vector<uint8_t>& record);

    // Close("session closed").
    void dispose();

    // Wait for the reader thread to finish (after a close), unless the
    // caller IS the reader thread.  Owners call this before they go away so
    // no callback runs into a destroyed owner.
    void waitForReader();

private:
    enum class ReadState { Data, HaveIac, HaveVerb, InSubnegotiation, SubnegotiationIac };

    static void appendEscaped(std::vector<uint8_t>& into, const uint8_t* src, int off, int len);
    bool writeRaw(const uint8_t* b, int len);
    bool writeRaw(const std::vector<uint8_t>& b) { return writeRaw(b.data(), static_cast<int>(b.size())); }
    void sendVerb(uint8_t verb, uint8_t option);
    void readLoop();
    void deliver(const std::vector<uint8_t>& record);
    void handleVerb(uint8_t v, uint8_t option);
    static bool acceptable(uint8_t option);
    void handleSubnegotiation(const std::vector<uint8_t>& sb);
    bool acceptableTerminalType();
    static bool classifyTerminalType(const std::string& type, StationKind& kind);
    void signalReadyOnce();
    void parseEnvironment(const std::vector<uint8_t>& sb);
    static std::string readEnvironToken(const std::vector<uint8_t>& sb, std::size_t& i);
    static std::string verbName(uint8_t v);
    static std::string optionName(uint8_t o);
    void close(const std::string& why);
    std::string label() const;

    SocketHandle client_;
    std::string peer_;
    monitor::Tracer* trace_;
    StationKind kind_;

    mutable std::mutex stateGate_;   // label, callbacks, negotiated state
    std::string label_;
    DeliverFn deliver_;
    ReadyFn ready_;
    MismatchFn mismatch_;
    ClosedFn closed_fn_;
    std::string terminalType_, deviceName_, userName_;
    std::map<std::string, std::string> environment_;
    // Option state, per RFC 854's rule that a party "only requests a change
    // in option status": a response is sent when the state changes and not
    // otherwise, which is what stops a WILL/DO exchange looping forever.
    std::set<uint8_t> peerWill_, peerDo_, weSentDo_, weSentWill_;

    std::mutex sendGate_;
    std::thread reader_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> readySignalled_{false};
};

}  // namespace sim36::host
