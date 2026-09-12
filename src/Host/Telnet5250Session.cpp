#include "Host/Telnet5250Session.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

namespace sim36::host {

namespace {

// RFC 854 page 14, "Telnet Command Structure".
constexpr uint8_t kInterpretAsCommand = 255;
constexpr uint8_t kSubnegotiationEnd = 240;
constexpr uint8_t kSubnegotiationBegin = 250;
constexpr uint8_t kWill = 251, kWont = 252, kDo = 253, kDont = 254;

// RFC 885: the End of Record option adds the command EOR, code 239, which
// terminates a logical record.
constexpr uint8_t kEndOfRecordCommand = 239;

// Option codes.  RFC 1205 section 2 requires exactly the first three;
// NEW-ENVIRON is RFC 2877's addition and is optional on both sides.
constexpr uint8_t kOptionTransmitBinary = 0;    // RFC 856
constexpr uint8_t kOptionTerminalType = 24;     // RFC 1091
constexpr uint8_t kOptionEndOfRecord = 25;      // RFC 885
constexpr uint8_t kOptionNewEnvironment = 39;   // RFC 1572

// Sub-negotiation qualifiers.  RFC 1091 and RFC 1572 both use 0 = IS and
// 1 = SEND; RFC 1572 adds INFO, sent when a variable changes after the
// initial IS.
constexpr uint8_t kQualifierIs = 0, kQualifierSend = 1, kQualifierInfo = 2;

// RFC 1572 section 2, the environment sub-option value codes.
constexpr uint8_t kEnvironVar = 0, kEnvironValue = 1, kEnvironEscape = 2, kEnvironUserVar = 3;

// RFC 2877 section 4: the USERVAR that names the device.  The other three it
// defines (KBDTYPE, CODEPAGE, CHARSET) are recorded by name too, but only
// DEVNAME is surfaced, because it is the only one an emulator with no
// virtual-device table can act on.
const char* const kDeviceNameUserVar = "DEVNAME";
// RFC 1572 section 2 defines six standard VARs; USER is the one RFC 2877
// section 5 gives a meaning to.
const char* const kUserVar = "USER";

// The printer terminal types, RFC 2877 section 9: "IBM-3812-1" (single-byte)
// or "IBM-5553-B01" (double-byte).  Those two are the whole printer set.
const char* const kPrinterTerminalTypes[] = {"IBM-3812-1", "IBM-5553-B01"};

// The display terminal types, from RFC 1205 section 2's own list.  Kept
// separate from the printer set rather than derived as "not a printer", so
// that a type in NEITHER list can be told apart from a type that contradicts
// the endpoint: the first is a client this emulator has not met, the second
// a misconfiguration.
const char* const kDisplayTerminalTypes[] = {"IBM-3179-2", "IBM-3180-2", "IBM-3196-A1", "IBM-5251-11",
                                             "IBM-5291-1", "IBM-5292-2", "IBM-5555-B01", "IBM-5555-C01"};

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

std::string opcodeName(WorkstationOpcode opcode)
{
    switch (opcode) {
        case WorkstationOpcode::NoOperation: return "NoOperation";
        case WorkstationOpcode::Invite: return "Invite";
        case WorkstationOpcode::OutputOnly: return "OutputOnly";
        case WorkstationOpcode::PutGet: return "PutGet";
        case WorkstationOpcode::SaveScreen: return "SaveScreen";
        case WorkstationOpcode::RestoreScreen: return "RestoreScreen";
        case WorkstationOpcode::ReadImmediate: return "ReadImmediate";
        case WorkstationOpcode::ReadScreen: return "ReadScreen";
        case WorkstationOpcode::CancelInvite: return "CancelInvite";
        case WorkstationOpcode::TurnOnMessageLight: return "TurnOnMessageLight";
        case WorkstationOpcode::TurnOffMessageLight: return "TurnOffMessageLight";
    }
    return std::to_string(static_cast<int>(opcode));
}

std::string flagsName(WorkstationRecordFlags flags)
{
    uint8_t f = flagsByte(flags);
    if (f == 0) return "None";
    struct Named { uint8_t bit; const char* name; };
    // Ascending value order, as a flags enumeration renders its members.
    static const Named named[] = {{0x01, "HelpInErrorState"}, {0x02, "TestRequest"}, {0x04, "SystemRequest"},
                                  {0x40, "Attention"}, {0x80, "DataStreamOutputError"}};
    std::string s;
    uint8_t covered = 0;
    for (const Named& n : named)
        if ((f & n.bit) != 0) {
            if (!s.empty()) s += ", ";
            s += n.name;
            covered = static_cast<uint8_t>(covered | n.bit);
        }
    if (covered != f) return std::to_string(static_cast<int>(f));
    return s;
}

std::string hexDash(const uint8_t* data, int length)
{
    std::string s;
    for (int i = 0; i < length; i++) {
        if (i != 0) s += '-';
        s += fmt::format("{:02X}", data[i]);
    }
    return s;
}

std::string WorkstationRecord::toString() const
{
    return fmt::format("{} flags={} {} byte(s)", opcodeName(opcode), flagsName(flags), data.size());
}

Telnet5250Session::Telnet5250Session(SocketHandle client, std::string peer, std::string label, StationKind kind,
                                     monitor::Tracer* trace, DeliverFn deliver, ReadyFn ready, MismatchFn mismatch)
    : client_(client), peer_(std::move(peer)), trace_(trace), kind_(kind), label_(std::move(label)),
      deliver_(std::move(deliver)), ready_(std::move(ready)), mismatch_(std::move(mismatch))
{
    // A screen is one short record; do not wait for a second.
    Sockets::setNoDelay(client_);
}

Telnet5250Session::~Telnet5250Session()
{
    close("session closed");
    if (reader_.joinable()) {
        if (reader_.get_id() == std::this_thread::get_id()) reader_.detach();
        else reader_.join();
    }
    Sockets::close(client_);
    client_ = kInvalidSocket;
}

void Telnet5250Session::setClosed(ClosedFn closed)
{
    std::lock_guard<std::mutex> lock(stateGate_);
    closed_fn_ = std::move(closed);
}

void Telnet5250Session::retarget(DeliverFn deliver, ReadyFn ready, MismatchFn mismatch, const std::string& label)
{
    std::lock_guard<std::mutex> lock(stateGate_);
    deliver_ = std::move(deliver);
    ready_ = std::move(ready);
    mismatch_ = std::move(mismatch);
    if (!label.empty()) label_ = label;
}

std::string Telnet5250Session::label() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return label_;
}

std::string Telnet5250Session::terminalType() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return terminalType_;
}

std::string Telnet5250Session::deviceName() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return deviceName_;
}

std::string Telnet5250Session::userName() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return userName_;
}

std::map<std::string, std::string> Telnet5250Session::environment() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return environment_;
}

bool Telnet5250Session::ready() const
{
    std::lock_guard<std::mutex> lock(stateGate_);
    return !terminalType_.empty() && peerWill_.count(kOptionEndOfRecord) != 0 && peerDo_.count(kOptionEndOfRecord) != 0 &&
           peerWill_.count(kOptionTransmitBinary) != 0 && peerDo_.count(kOptionTransmitBinary) != 0;
}

void Telnet5250Session::start()
{
    // RFC 2877 section 3: "the server will bundle an environment option
    // invitation along with the standard terminal type invitation".  One
    // write, two requests, exactly as its worked example shows.
    const uint8_t open[] = {kInterpretAsCommand, kDo, kOptionNewEnvironment, kInterpretAsCommand, kDo, kOptionTerminalType};
    writeRaw(open, static_cast<int>(sizeof open));
    {
        std::lock_guard<std::mutex> lock(stateGate_);
        weSentDo_.insert(kOptionNewEnvironment);
        weSentDo_.insert(kOptionTerminalType);
    }
    // The reader keeps the session alive for as long as it runs, so no
    // callback ever runs on a destroyed session.
    std::shared_ptr<Telnet5250Session> self = shared_from_this();
    reader_ = std::thread([self]() { self->readLoop(); });
}

// ---- send ---------------------------------------------------------------------

bool Telnet5250Session::send(WorkstationOpcode opcode, WorkstationRecordFlags flags, const uint8_t* data, int offset,
                             int length)
{
    // "This field indicates the length, in octets, of this logical record
    // including the header length.  The length is calculated BEFORE
    // doubling any IAC characters ... The length does not include the
    // <IAC><EOR>" - RFC 1205 section 3.
    int logicalLength = kHeaderLength + length;

    uint8_t head[kHeaderLength];
    head[0] = static_cast<uint8_t>(logicalLength >> 8);
    head[1] = static_cast<uint8_t>(logicalLength & 0xFF);
    head[2] = 0x12;
    head[3] = 0xA0;   // record type: General Data Stream
    head[4] = 0x00;
    head[5] = 0x00;   // reserved
    head[6] = 0x04;   // variable header length, "currently ... always '04'X"
    head[7] = flagsByte(flags);   // flags, first octet
    head[8] = 0x00;               // flags, second octet: bits 8-15 reserved
    head[9] = static_cast<uint8_t>(opcode);

    std::vector<uint8_t> wire;
    wire.reserve(static_cast<std::size_t>(logicalLength) + 16);
    appendEscaped(wire, head, 0, kHeaderLength);
    if (length > 0) appendEscaped(wire, data, offset, length);
    wire.push_back(kInterpretAsCommand);
    wire.push_back(kEndOfRecordCommand);

    trace_->ws("{}: SEND record opcode={} flags={} {} data byte(s): header={} body={}", label(), opcodeName(opcode),
               flagsName(flags), length, hexDash(head, kHeaderLength), length == 0 ? std::string() : hexDash(data + offset, length));
    return writeRaw(wire);
}

bool Telnet5250Session::sendFramed(const std::vector<uint8_t>& record)
{
    std::vector<uint8_t> wire;
    wire.reserve(record.size() + 16);
    appendEscaped(wire, record.data(), 0, static_cast<int>(record.size()));
    wire.push_back(kInterpretAsCommand);
    wire.push_back(kEndOfRecordCommand);
    return writeRaw(wire);
}

// RFC 854: "IAC IAC" is how a data octet of 255 is sent.
void Telnet5250Session::appendEscaped(std::vector<uint8_t>& into, const uint8_t* src, int off, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t b = src[off + i];
        into.push_back(b);
        if (b == kInterpretAsCommand) into.push_back(kInterpretAsCommand);
    }
}

bool Telnet5250Session::writeRaw(const uint8_t* b, int len)
{
    std::lock_guard<std::mutex> lock(sendGate_);
    if (closed_) return false;
    if (Sockets::sendAll(client_, b, len)) return true;
    close("write failed");
    return false;
}

void Telnet5250Session::sendVerb(uint8_t verb, uint8_t option)
{
    const uint8_t b[] = {kInterpretAsCommand, verb, option};
    writeRaw(b, 3);
}

// ---- receive --------------------------------------------------------------------

void Telnet5250Session::readLoop()
{
    uint8_t buffer[4096];
    std::vector<uint8_t> record;
    std::vector<uint8_t> subnegotiation;
    ReadState state = ReadState::Data;
    uint8_t verb = 0;

    while (!closed_) {
        // A bounded wait, so a close from another thread is noticed without
        // relying on the socket library to wake a blocked read.
        if (!Sockets::waitReadable(client_, 250)) continue;
        if (closed_) break;
        int got = Sockets::recv(client_, buffer, static_cast<int>(sizeof buffer));
        if (got <= 0) break;

        // Trace every inbound socket read (raw, before Telnet/5250
        // de-framing) so a keypress that never becomes a record - a locked
        // keyboard, an Attn, a stray byte - is still visible.
        trace_->ws("{}: socket read {} byte(s): {}", label(), got, hexDash(buffer, std::min(got, 96)));

        for (int i = 0; i < got; i++) {
            uint8_t c = buffer[i];
            switch (state) {
                case ReadState::Data:
                    if (c == kInterpretAsCommand) state = ReadState::HaveIac;
                    else record.push_back(c);
                    break;

                case ReadState::HaveIac:
                    if (c == kInterpretAsCommand) {
                        record.push_back(c);
                        state = ReadState::Data;
                    } else if (c == kWill || c == kWont || c == kDo || c == kDont) {
                        verb = c;
                        state = ReadState::HaveVerb;
                    } else if (c == kSubnegotiationBegin) {
                        subnegotiation.clear();
                        state = ReadState::InSubnegotiation;
                    } else if (c == kEndOfRecordCommand) {
                        deliver(record);
                        record.clear();
                        state = ReadState::Data;
                    } else {
                        // RFC 854's other commands - NOP, Data Mark, Break and
                        // so on.  None has a meaning in 5250 mode; a conforming
                        // receiver ignores what it does not use.
                        trace_->ws("{}: ignoring Telnet command {}", label(), c);
                        state = ReadState::Data;
                    }
                    break;

                case ReadState::HaveVerb:
                    handleVerb(verb, c);
                    state = ReadState::Data;
                    break;

                case ReadState::InSubnegotiation:
                    if (c == kInterpretAsCommand) state = ReadState::SubnegotiationIac;
                    else subnegotiation.push_back(c);
                    break;

                case ReadState::SubnegotiationIac:
                    if (c == kInterpretAsCommand) {
                        subnegotiation.push_back(c);
                        state = ReadState::InSubnegotiation;
                    } else if (c == kSubnegotiationEnd) {
                        handleSubnegotiation(subnegotiation);
                        subnegotiation.clear();
                        state = ReadState::Data;
                    } else {
                        state = ReadState::InSubnegotiation;
                    }
                    break;
            }
        }
    }

    close("client closed the connection");
}

void Telnet5250Session::deliver(const std::vector<uint8_t>& record)
{
    if (static_cast<int>(record.size()) < kHeaderLength) {
        trace_->ws("{}: record of {} byte(s) is shorter than the RFC 1205 header, dropped", label(), record.size());
        return;
    }
    auto flags = static_cast<WorkstationRecordFlags>(record[7]);
    auto opcode = static_cast<WorkstationOpcode>(record[9]);
    std::vector<uint8_t> data(record.begin() + kHeaderLength, record.end());
    trace_->ws("{}: RECV record opcode={} flags={} {} data byte(s): full={} body={}", label(), opcodeName(opcode),
               flagsName(flags), data.size(), hexDash(record.data(), static_cast<int>(record.size())),
               hexDash(data.data(), static_cast<int>(data.size())));
    DeliverFn deliverFn;
    {
        std::lock_guard<std::mutex> lock(stateGate_);
        deliverFn = deliver_;
    }
    if (deliverFn) deliverFn(WorkstationRecord(opcode, flags, std::move(data)));
}

void Telnet5250Session::handleVerb(uint8_t v, uint8_t option)
{
    // The answer is decided under the state lock and written after it is
    // released: a write can close the session, and close takes the lock.
    uint8_t answer = 0;
    {
        std::lock_guard<std::mutex> lock(stateGate_);
        switch (v) {
            case kWill:
                if (peerWill_.insert(option).second) {
                    if (acceptable(option)) {
                        if (weSentDo_.insert(option).second) answer = kDo;
                    } else {
                        peerWill_.erase(option);
                        answer = kDont;
                    }
                }
                break;

            case kWont:
                if (peerWill_.erase(option) != 0) {
                    weSentDo_.erase(option);
                    answer = kDont;
                }
                break;

            case kDo:
                if (peerDo_.insert(option).second) {
                    // The server offers only what RFC 1205 section 2 needs
                    // from its own side.  TERMINAL-TYPE and NEW-ENVIRON flow
                    // client to server, so a DO for either is refused.
                    if (option == kOptionTransmitBinary || option == kOptionEndOfRecord) {
                        if (weSentWill_.insert(option).second) answer = kWill;
                    } else {
                        peerDo_.erase(option);
                        answer = kWont;
                    }
                }
                break;

            case kDont:
                if (peerDo_.erase(option) != 0) {
                    weSentWill_.erase(option);
                    answer = kWont;
                }
                break;

            default:
                break;
        }
    }
    if (answer != 0) sendVerb(answer, option);

    trace_->ws("{}: negotiated {} {}", label(), verbName(v), optionName(option));

    // RFC 1205 section 2's example order: terminal type first, then EOR and
    // binary.  Asking for the type as soon as the client says WILL is what
    // its worked exchange shows, and RFC 2877 section 3 repeats it.
    if (v == kWill && option == kOptionTerminalType) {
        const uint8_t ask[] = {kInterpretAsCommand, kSubnegotiationBegin, kOptionTerminalType, kQualifierSend,
                               kInterpretAsCommand, kSubnegotiationEnd};
        writeRaw(ask, static_cast<int>(sizeof ask));
    }

    if (v == kWill && option == kOptionNewEnvironment) {
        // RFC 1572 section 2: a SEND naming VAR and USERVAR with no names
        // after them asks for every variable of each kind.
        const uint8_t ask[] = {kInterpretAsCommand, kSubnegotiationBegin, kOptionNewEnvironment, kQualifierSend,
                               kEnvironVar, kEnvironUserVar, kInterpretAsCommand, kSubnegotiationEnd};
        writeRaw(ask, static_cast<int>(sizeof ask));
    }

    // 5250 mode needs the type AND both directions of binary and EOR, and
    // the last of those four usually arrives here rather than in the
    // terminal-type sub-negotiation, so the check belongs in both places.
    signalReadyOnce();
}

bool Telnet5250Session::acceptable(uint8_t option)
{
    return option == kOptionTransmitBinary || option == kOptionEndOfRecord || option == kOptionTerminalType ||
           option == kOptionNewEnvironment;
}

void Telnet5250Session::handleSubnegotiation(const std::vector<uint8_t>& sb)
{
    if (sb.size() < 2) return;

    if (sb[0] == kOptionTerminalType && sb[1] == kQualifierIs) {
        std::string type(sb.begin() + 2, sb.end());
        {
            std::lock_guard<std::mutex> lock(stateGate_);
            terminalType_ = type;
        }
        trace_->ws("{}: terminal type {}", label(), type);

        // The type is the only thing on this wire that says which FAMILY the
        // client belongs to, and the two need different record framing, so it
        // is checked before negotiation is completed rather than after: a
        // session that cannot be served should not be told it is in 5250 mode.
        if (!acceptableTerminalType()) {
            StationKind announced = kind_;
            classifyTerminalType(type, announced);
            MismatchFn mismatch;
            {
                std::lock_guard<std::mutex> lock(stateGate_);
                mismatch = mismatch_;
            }
            if (mismatch) mismatch(type, announced);
            return;
        }

        // The type has arrived, so 5250 mode can be completed.  RFC 1205
        // section 2 shows the server sending DO and WILL for each of EOR and
        // binary; the client answers WILL/DO and both directions are then
        // agreed.
        std::vector<uint8_t> open;
        {
            std::lock_guard<std::mutex> lock(stateGate_);
            if (weSentDo_.insert(kOptionEndOfRecord).second) {
                open.push_back(kInterpretAsCommand); open.push_back(kDo); open.push_back(kOptionEndOfRecord);
            }
            if (weSentWill_.insert(kOptionEndOfRecord).second) {
                open.push_back(kInterpretAsCommand); open.push_back(kWill); open.push_back(kOptionEndOfRecord);
            }
            if (weSentDo_.insert(kOptionTransmitBinary).second) {
                open.push_back(kInterpretAsCommand); open.push_back(kDo); open.push_back(kOptionTransmitBinary);
            }
            if (weSentWill_.insert(kOptionTransmitBinary).second) {
                open.push_back(kInterpretAsCommand); open.push_back(kWill); open.push_back(kOptionTransmitBinary);
            }
        }
        if (!open.empty()) writeRaw(open);
        signalReadyOnce();
        return;
    }

    if (sb[0] == kOptionNewEnvironment && (sb[1] == kQualifierIs || sb[1] == kQualifierInfo)) {
        parseEnvironment(sb);
        return;
    }
}

// Is this client's announced type one this endpoint can serve?  A type in
// neither published list is ALLOWED, with a trace: RFC 1205 and RFC 2877
// between them name ten types and clients emulate more than ten devices, so
// refusing an unrecognised one would break working clients to catch a
// misconfiguration.  A type positively identified as the OTHER family is
// refused, because that is not an unknown, it is a contradiction.
bool Telnet5250Session::acceptableTerminalType()
{
    std::string type = terminalType();
    StationKind announced = kind_;
    if (!classifyTerminalType(type, announced)) {
        trace_->ws("{}: terminal type {} is in neither RFC 1205 section 2's display list nor RFC 2877 section 9's "
                   "printer list - served as the {} this endpoint is configured as, because the published lists are "
                   "not exhaustive",
                   label(), type, kind_ == StationKind::Printer ? "printer" : "display");
        return true;
    }
    return announced == kind_;
}

// Which family a terminal type belongs to; false when it is in neither
// published list.
bool Telnet5250Session::classifyTerminalType(const std::string& type, StationKind& kind)
{
    if (type.empty()) return false;
    for (const char* t : kPrinterTerminalTypes)
        if (equalsIgnoreCase(t, type)) {
            kind = StationKind::Printer;
            return true;
        }
    for (const char* t : kDisplayTerminalTypes)
        if (equalsIgnoreCase(t, type)) {
            kind = StationKind::Display;
            return true;
        }
    return false;
}

// Announce that 5250 mode is complete, once per session.  A printer owes the
// client an RFC 2877 section 9 startup response record the moment
// negotiation finishes; a display owes nothing, so the hook does nothing
// there.
void Telnet5250Session::signalReadyOnce()
{
    if (readySignalled_ || !ready()) return;
    readySignalled_ = true;
    ReadyFn readyFn;
    {
        std::lock_guard<std::mutex> lock(stateGate_);
        readyFn = ready_;
    }
    if (readyFn) readyFn();
}

// RFC 1572 section 2's encoding: the sub-option body is a sequence of VAR
// name VALUE value, or USERVAR name VALUE value.  A name or value runs to
// the next code byte; ESC quotes a code byte inside one.
void Telnet5250Session::parseEnvironment(const std::vector<uint8_t>& sb)
{
    std::size_t i = 2;
    while (i < sb.size()) {
        uint8_t kind = sb[i];
        if (kind != kEnvironVar && kind != kEnvironUserVar) {
            i++;
            continue;
        }
        i++;

        std::string name = readEnvironToken(sb, i);
        std::string value;
        if (i < sb.size() && sb[i] == kEnvironValue) {
            i++;
            value = readEnvironToken(sb, i);
        }

        if (name.empty()) continue;
        {
            std::lock_guard<std::mutex> lock(stateGate_);
            environment_[name] = value;
            // RFC 2877 section 4 classifies DEVNAME as a USERVAR and RFC 1572
            // section 2 classifies USER as a standard VAR, but the kind is not
            // checked here: some clients send every variable they were given
            // as a standard VAR, DEVNAME included, so a receiver that insisted
            // on the classification would silently learn nothing from a client
            // that is otherwise conforming.  The name is what carries meaning.
            if (name == kDeviceNameUserVar) deviceName_ = value;
            if (name == kUserVar) userName_ = value;
        }
        trace_->ws("{}: environment {} {} = {}", label(), kind == kEnvironUserVar ? "USERVAR" : "VAR", name, value);
    }
}

std::string Telnet5250Session::readEnvironToken(const std::vector<uint8_t>& sb, std::size_t& i)
{
    std::string s;
    while (i < sb.size()) {
        uint8_t b = sb[i];
        if (b == kEnvironEscape) {
            i++;
            if (i < sb.size()) s += static_cast<char>(sb[i++]);
            continue;
        }
        if (b == kEnvironVar || b == kEnvironValue || b == kEnvironUserVar) break;
        s += static_cast<char>(b);
        i++;
    }
    return s;
}

std::string Telnet5250Session::verbName(uint8_t v)
{
    switch (v) {
        case kWill: return "WILL";
        case kWont: return "WONT";
        case kDo: return "DO";
        case kDont: return "DONT";
        default: return std::to_string(static_cast<int>(v));
    }
}

std::string Telnet5250Session::optionName(uint8_t o)
{
    switch (o) {
        case kOptionTransmitBinary: return "TRANSMIT-BINARY";
        case kOptionTerminalType: return "TERMINAL-TYPE";
        case kOptionEndOfRecord: return "END-OF-RECORD";
        case kOptionNewEnvironment: return "NEW-ENVIRON";
        default: return "option " + std::to_string(static_cast<int>(o));
    }
}

void Telnet5250Session::close(const std::string& why)
{
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) return;
    trace_->ws("{}: {}", label(), why);
    Sockets::shutdown(client_);
    ClosedFn closedFn;
    {
        std::lock_guard<std::mutex> lock(stateGate_);
        closedFn = closed_fn_;
    }
    if (closedFn) closedFn();
}

void Telnet5250Session::dispose() { close("session closed"); }

void Telnet5250Session::waitForReader()
{
    if (!reader_.joinable()) return;
    if (reader_.get_id() == std::this_thread::get_id()) return;
    reader_.join();
}

}  // namespace sim36::host
