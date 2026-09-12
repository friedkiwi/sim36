#include "Host/StationBackend.h"

#include <cctype>
#include <stdexcept>

#include <fmt/format.h>

namespace sim36::host {

namespace {

const char* kindText(StationKind kind) { return kind == StationKind::Printer ? "printer" : "display"; }

}  // namespace

// ---- StationBackend ------------------------------------------------------------

StationBackend::StationBackend(StationKind kind, const std::string& host, int port, const std::string& label,
                               monitor::Tracer* trace, std::function<void()> signalMachine)
    : trace_(trace), kind_(kind), host_(host), port_(port), label_(label), signalMachine_(std::move(signalMachine))
{
}

StationBackend::~StationBackend() { dispose(); }

std::shared_ptr<Telnet5250Session> StationBackend::session() const
{
    std::lock_guard<std::mutex> lock(sessionGate_);
    return session_;
}

bool StationBackend::sessionAttached() const
{
    auto s = session();
    return s != nullptr && s->connected();
}

bool StationBackend::attached() const { return sessionAttached(); }

bool StationBackend::ready() const
{
    auto s = session();
    return s != nullptr && s->ready();
}

std::string StationBackend::terminalType() const
{
    auto s = session();
    return s == nullptr ? std::string() : s->terminalType();
}

std::string StationBackend::deviceName() const
{
    auto s = session();
    return s == nullptr ? std::string() : s->deviceName();
}

std::string StationBackend::userName() const
{
    auto s = session();
    return s == nullptr ? std::string() : s->userName();
}

bool StationBackend::takePowerOffPending()
{
    bool expected = true;
    return powerOffPending_.compare_exchange_strong(expected, false);
}

bool StationBackend::takeAttentionPending()
{
    bool expected = true;
    return attentionPending_.compare_exchange_strong(expected, false);
}

void StationBackend::onSessionGone(const std::shared_ptr<Telnet5250Session>& session)
{
    if (!session->readySignalled()) return;
    powerOffPending_ = true;
    trace_->ws("{}: session gone after being present - display powered off", label_);
    signalMachine();
}

void StationBackend::raiseAttention()
{
    attentionPending_ = true;
    signalMachine();
}

void StationBackend::signalMachine()
{
    if (signalMachine_) signalMachine_();
}

void StationBackend::resetAttention(bool pending)
{
    attentionPending_ = pending;
    if (pending) signalMachine();
}

void StationBackend::rebind(const std::string& host, int port, monitor::Tracer* trace,
                            std::function<void()> signalMachine)
{
    stopping_ = true;
    detach();
    stopListener();
    host_ = host;
    port_ = port;
    trace_ = trace;
    signalMachine_ = std::move(signalMachine);
    stopping_ = false;
    listen();
}

void StationBackend::bindMachine(monitor::Tracer* trace, std::function<void()> signalMachine)
{
    trace_ = trace;
    signalMachine_ = std::move(signalMachine);
}

void StationBackend::resetForMachine() { resetAttention(attached()); }

void StationBackend::listen()
{
    if (port_ == 0) return;
    // The chassis opens the listener when the definition is reconciled and
    // the machine asks again when it starts; the second request finds the
    // socket already open.
    if (listener_ != kInvalidSocket) return;
    std::string error;
    listener_ = Sockets::listenOn(host_, port_, error);
    if (listener_ == kInvalidSocket) throw std::runtime_error(label_ + ": cannot listen on " + endpoint() + ": " + error);
    stopping_ = false;
    accept_ = std::thread([this]() { acceptLoop(); });
    trace_->ws("{}: listening on {} for a {} client", label_, endpoint(), kindText(kind_));
}

void StationBackend::stopListener()
{
    stopping_ = true;
    if (accept_.joinable()) {
        if (accept_.get_id() == std::this_thread::get_id()) accept_.detach();
        else accept_.join();
    }
    Sockets::close(listener_);
    listener_ = kInvalidSocket;
}

void StationBackend::acceptLoop()
{
    while (!stopping_) {
        // A bounded wait so a stop is noticed without the socket library's
        // help.
        if (!Sockets::waitReadable(listener_, 250)) continue;
        if (stopping_) return;
        std::string peer;
        SocketHandle client = Sockets::accept(listener_, peer);
        if (client == kInvalidSocket) return;
        if (stopping_) {
            Sockets::close(client);
            return;
        }

        if (attached()) {
            // One session per endpoint: this configuration is one listener
            // per station address, so a second client has nowhere to go.
            sessionsRefused_++;
            trace_->ws("{}: second client refused, session already attached", label_);
            Sockets::close(client);
            continue;
        }

        sessionsAccepted_++;
        auto session = std::make_shared<Telnet5250Session>(
            client, peer, label_, kind_, trace_, [this](const WorkstationRecord& r) { onRecord(r); },
            [this]() { sessionReady(); },
            [this](const std::string& type, StationKind announced) { onKindMismatch(type, announced); });
        std::weak_ptr<Telnet5250Session> weak = session;
        session->setClosed([this, weak]() {
            if (auto s = weak.lock()) onSessionGone(s);
        });
        installSession(session);
        trace_->ws("{}: session attached from {}", label_, peer);
        session->start();
    }
}

// The previous session, if any, has dropped its connection but still owns a
// socket handle; it is released before it is forgotten.
void StationBackend::installSession(std::shared_ptr<Telnet5250Session> session)
{
    std::shared_ptr<Telnet5250Session> stale;
    {
        std::lock_guard<std::mutex> lock(sessionGate_);
        stale = session_;
        session_ = std::move(session);
    }
    if (stale != nullptr) stale->dispose();
}

// A client whose announced terminal type belongs to the other family
// reached this endpoint.  A printer client and a display client agree on
// every Telnet option and differ only in the record framing they expect, so
// a mismatched pair negotiates all the way through and then silently fails
// to communicate; refusing here turns a silent misconfiguration into a
// traced one.
void StationBackend::onKindMismatch(const std::string& terminalType, StationKind announced)
{
    sessionsRejected_++;
    trace_->ws("{}: REFUSED - this endpoint is a {} station and the client announced TERMINAL-TYPE {}, which is a {} "
               "type (RFC 1205 section 2 lists the display types, RFC 2877 section 9 the printer ones). The two "
               "families negotiate identically and differ only in record framing, so a mismatch would look like a "
               "working session and carry nothing",
               label_, kindText(kind_), terminalType, kindText(announced));
    detach();
}

// The device becoming present (negotiation reached 5250 mode) is an I/O
// attention: raise the latch, then run whatever the subclass does on ready.
void StationBackend::sessionReady()
{
    raiseAttention();
    onReady();
}

bool StationBackend::adoptSession(std::shared_ptr<Telnet5250Session> session)
{
    if (session == nullptr) throw std::invalid_argument("session");
    // attached() can also mean an intrinsic attachment (the system
    // console's monitor display).  Only another network session makes a
    // multiplexer hand-over collide.
    if (sessionAttached()) return false;

    session->retarget([this](const WorkstationRecord& r) { onRecord(r); }, [this]() { sessionReady(); },
                      [this](const std::string& type, StationKind announced) { onKindMismatch(type, announced); }, label_);
    std::weak_ptr<Telnet5250Session> weak = session;
    session->setClosed([this, weak]() {
        if (auto s = weak.lock()) onSessionGone(s);
    });
    installSession(session);
    sessionsAccepted_++;
    trace_->ws("{}: session adopted from the station multiplexer", label_);
    sessionAdopted();
    return true;
}

void StationBackend::countReceived(const WorkstationRecord& r)
{
    recordsReceived_++;
    bytesReceived_ += static_cast<long long>(r.data.size());
}

bool StationBackend::sendRecord(const std::vector<uint8_t>& record, int payloadBytes, const std::string& what)
{
    auto s = session();
    if (s == nullptr || !s->connected()) {
        recordsDropped_++;
        trace_->ws("{}: {} dropped, no session attached", label_, what);
        return false;
    }
    if (!s->ready()) {
        recordsDropped_++;
        trace_->ws("{}: {} dropped, Telnet negotiation incomplete (RFC 1205 section 2 needs BINARY, EOR and "
                   "TERMINAL-TYPE)",
                   label_, what);
        return false;
    }
    if (!s->sendFramed(record)) {
        recordsDropped_++;
        return false;
    }
    recordsSent_++;
    bytesSent_ += payloadBytes;
    trace_->ws("{}: record out {}", label_, what);
    return true;
}

std::shared_ptr<Telnet5250Session> StationBackend::releaseSession()
{
    std::shared_ptr<Telnet5250Session> s;
    {
        std::lock_guard<std::mutex> lock(sessionGate_);
        s = session_;
        session_.reset();
    }
    if (s == nullptr) return nullptr;
    if (!s->connected()) {
        s->dispose();
        return nullptr;
    }
    // The socket outlives this backend's ownership: whoever adopts it next
    // installs its own close handler.
    s->setClosed(nullptr);
    trace_->ws("{}: session released to the station multiplexer", label_);
    return s;
}

void StationBackend::detach()
{
    std::shared_ptr<Telnet5250Session> s;
    {
        std::lock_guard<std::mutex> lock(sessionGate_);
        s = session_;
        session_.reset();
    }
    if (s == nullptr) return;
    s->dispose();
    trace_->ws("{}: session detached", label_);
}

void StationBackend::dispose()
{
    stopping_ = true;
    detach();
    stopListener();
}

// ---- WorkstationBackend -------------------------------------------------------------

WorkstationBackend::~WorkstationBackend()
{
    // Close the session and wait for its reader before the queues and the
    // console it delivers into go away.
    auto s = session();
    dispose();
    if (s != nullptr) s->waitForReader();
}

void WorkstationBackend::onSessionGone(const std::shared_ptr<Telnet5250Session>& session)
{
    // A mirror client leaving the intrinsic console does not power the
    // console off: the console is present from power-on and the IPL cannot
    // complete without it.
    if (isConsoleAttachment()) return;
    StationBackend::onSessionGone(session);
}

void WorkstationBackend::clearForPowerOff()
{
    std::lock_guard<std::mutex> lock(gate_);
    inbound_.clear();
    saveResponses_.clear();
    headStatusTaken_ = false;
    inputEnabled_ = false;
}

void WorkstationBackend::sessionAdopted()
{
    // The console was already present before the client arrived.  A second
    // present edge here is not a physical event and can reorder SSP IPL
    // processing.
    if (!isConsoleAttachment()) StationBackend::sessionAdopted();
}

bool WorkstationBackend::adoptSession(std::shared_ptr<Telnet5250Session> session)
{
    if (!StationBackend::adoptSession(session)) return false;
    if (!isConsoleAttachment()) return true;

    console_->setWriteLog(false);

    // The selector panel replaced the client's display.  Replaying the
    // bounded, exact outbound history reconstructs the guest console as it
    // stands now; subsequent output is mirrored live.  Only operations that
    // contribute display state are repainted: a replayed Save/Read request
    // would solicit a second response for an operation the guest issued
    // before this client joined.
    for (const DiagnosticRecord& r : captureDiagnosticHistory()) {
        if (r.direction != "out") continue;
        if (r.opcode != WorkstationOpcode::OutputOnly && r.opcode != WorkstationOpcode::PutGet &&
            r.opcode != WorkstationOpcode::RestoreScreen)
            continue;
        session->send(r.opcode, r.flags, r.data.data(), 0, static_cast<int>(r.data.size()));
    }
    return true;
}

std::shared_ptr<Telnet5250Session> WorkstationBackend::releaseSession()
{
    std::shared_ptr<Telnet5250Session> session = StationBackend::releaseSession();
    if (isConsoleAttachment()) console_->setWriteLog(true);
    return session;
}

int WorkstationBackend::pendingInput() const
{
    std::lock_guard<std::mutex> lock(gate_);
    return static_cast<int>(inbound_.size());
}

int WorkstationBackend::pendingSaveScreens() const
{
    std::lock_guard<std::mutex> lock(gate_);
    return static_cast<int>(saveResponses_.size());
}

std::vector<WorkstationBackend::DiagnosticRecord> WorkstationBackend::captureDiagnosticHistory() const
{
    std::lock_guard<std::mutex> lock(gate_);
    return std::vector<DiagnosticRecord>(history_.begin(), history_.end());
}

void WorkstationBackend::appendDiagnosticLocked(const char* direction, const char* disposition, WorkstationOpcode opcode,
                                                WorkstationRecordFlags flags, const uint8_t* data, int offset, int length)
{
    while (static_cast<int>(history_.size()) >= kDiagnosticHistoryLimit) history_.pop_front();
    DiagnosticRecord r;
    r.sequence = ++historySequence_;
    r.direction = direction;
    r.disposition = disposition;
    r.opcode = opcode;
    r.flags = flags;
    if (length > 0) r.data.assign(data + offset, data + offset + length);
    history_.push_back(std::move(r));
}

WorkstationBackend::DiagnosticQueues WorkstationBackend::captureDiagnosticQueues() const
{
    std::lock_guard<std::mutex> lock(gate_);
    DiagnosticQueues q;
    q.input.assign(inbound_.begin(), inbound_.end());
    q.saveScreen.assign(saveResponses_.begin(), saveResponses_.end());
    q.headStatusTaken = headStatusTaken_;
    return q;
}

void WorkstationBackend::resetForMachine()
{
    // These queues complete operations issued by one guest-machine
    // lifetime.  The TCP session survives reset, but stale input and
    // Save-Screen completions must not cross into the replacement.
    {
        std::lock_guard<std::mutex> lock(gate_);
        inbound_.clear();
        saveResponses_.clear();
        history_.clear();
        historySequence_ = 0;
        headStatusTaken_ = false;
        inputEnabled_ = false;
    }
    // The built-in 0.0 console is an always-present controller device, not a
    // newly attached TN5250 session.  Its native configure path already
    // supplies the power-on response, so fabricating a second attention here
    // would reorder SSP IPL.
    if (isConsoleAttachment()) resetAttention(false);
    else StationBackend::resetForMachine();
}

void WorkstationBackend::enqueue(const WorkstationRecord& r)
{
    bool saveResponse = r.opcode == WorkstationOpcode::SaveScreen;
    // Both a live socket record and the harness injection enter here.
    // AID-bearing data is an input-ready status even when RFC ATN/SRQ is
    // clear; flag-only ATTN/SYSREQ remains an unsolicited attention.
    // Opcode 04 is the response to an outstanding Save Screen request: its
    // body can be thousands of bytes long, but it is not cursor/AID input
    // and must never raise input attention.
    const uint8_t attentionKeys = flagsByte(WorkstationRecordFlags::Attention | WorkstationRecordFlags::SystemRequest);
    if (!saveResponse && ((flagsByte(r.flags) & attentionKeys) != 0 || r.data.size() >= 3)) raiseAttention();
    bool overran = false;
    {
        std::lock_guard<std::mutex> lock(gate_);
        std::deque<WorkstationRecord>& queue = saveResponse ? saveResponses_ : inbound_;
        int limit = saveResponse ? kSaveResponseLimit : kInboundLimit;
        while (static_cast<int>(queue.size()) >= limit) {
            queue.pop_front();
            if (!saveResponse) headStatusTaken_ = false;
            recordsDropped_++;
            overran = true;
        }
        queue.push_back(r);
        appendDiagnosticLocked("in", "accepted", r.opcode, r.flags, r.data.data(), 0, static_cast<int>(r.data.size()));
        recordsReceived_++;
        bytesReceived_ += static_cast<long long>(r.data.size());
    }
    if (overran)
        trace_->ws("{}: park slot overwritten - a newer response arrived before the guest read the previous one, which "
                   "A/36 also discards (the park is one AID byte plus one cursor halfword, last writer wins)",
                   label());
    trace_->ws("{}: record in  {}", label(), r.toString());
    // Save-Screen responses deliberately do not raise SSP input attention,
    // but they still complete a pending native device operation and must
    // wake a machine parked in its event wait.
    signalMachine();
}

bool WorkstationBackend::setInputEnabled(bool enabled)
{
    inputEnabled_ = enabled;
    return send(enabled ? WorkstationOpcode::Invite : WorkstationOpcode::CancelInvite, WorkstationRecordFlags::None, nullptr,
                0, 0);
}

bool WorkstationBackend::injectConsoleInput(uint8_t aid)
{
    if (!isConsoleAttachment()) return false;
    std::vector<uint8_t> body = console_->buildInput(aid);
    onRecord(WorkstationRecord(WorkstationOpcode::PutGet, WorkstationRecordFlags::None, std::move(body)));
    console_->clearPending();
    return true;
}

bool WorkstationBackend::sendDataStream(const uint8_t* data, int offset, int length)
{
    return send(inputEnabled_ ? WorkstationOpcode::PutGet : WorkstationOpcode::OutputOnly, WorkstationRecordFlags::None, data,
                offset, length);
}

bool WorkstationBackend::sendPut(const uint8_t* data, int offset, int length, bool withInvite)
{
    inputEnabled_ = withInvite;
    return send(withInvite ? WorkstationOpcode::PutGet : WorkstationOpcode::OutputOnly, WorkstationRecordFlags::None, data,
                offset, length);
}

bool WorkstationBackend::sendSaveScreen()
{
    const uint8_t request[] = {0x04, 0x02};
    return send(WorkstationOpcode::SaveScreen, WorkstationRecordFlags::None, request, 0, 2);
}

bool WorkstationBackend::sendRestoreScreen(const uint8_t* data, int offset, int length)
{
    return send(WorkstationOpcode::RestoreScreen, WorkstationRecordFlags::None, data, offset, length);
}

bool WorkstationBackend::sendSavedReadMode(uint8_t mode)
{
    std::vector<uint8_t> suffix;
    if (mode == 1)
        suffix = {0x04, 0x42, 0x00, 0x00};
    else if (mode == 0x20)
        suffix = {0x04, 0x52, 0x00, 0x00};
    else if (mode == 0x21)
        suffix = {0x04, 0xF3, 0x00, 0x08, 0xD9, 0x32, 0x00, 0x80, 0x00, 0x00};
    else
        return false;
    inputEnabled_ = true;
    return send(WorkstationOpcode::PutGet, WorkstationRecordFlags::None, suffix.data(), 0, static_cast<int>(suffix.size()));
}

bool WorkstationBackend::send(WorkstationOpcode opcode, WorkstationRecordFlags flags, const uint8_t* data, int offset,
                              int length)
{
    // The console attachment decodes to the operator interface.  It has no
    // session and never negotiates, so it also never drops for either reason
    // below, which is the point: the console is present from power-on
    // because the IPL cannot complete without it.
    if (isConsoleAttachment()) {
        auto consoleSession = session();
        bool connected = consoleSession != nullptr && consoleSession->connected();
        // TN5250 and stdio are alternative presentations.  Keep decoding the
        // screen for monitor commands and panic dumps, but do not print every
        // operation twice while the console is on the multiplexer.
        console_->setWriteLog(!connected);
        console_->apply(data, offset, length);
        inputEnabled_ = opcode == WorkstationOpcode::PutGet;
        bool mirrored = connected && consoleSession->ready() && consoleSession->send(opcode, flags, data, offset, length);
        recordsSent_++;
        bytesSent_ += length;
        {
            std::lock_guard<std::mutex> lock(gate_);
            appendDiagnosticLocked("out", mirrored ? "console+sent" : "console", opcode, flags, data, offset, length);
        }
        trace_->ws("{}: console record out {} flags={} {} byte(s)", label(), opcodeName(opcode), flagsName(flags), length);
        return true;
    }
    auto s = session();
    if (s == nullptr || !s->connected()) {
        recordsDropped_++;
        {
            std::lock_guard<std::mutex> lock(gate_);
            appendDiagnosticLocked("out", "dropped-no-session", opcode, flags, data, offset, length);
        }
        trace_->ws("{}: {} byte(s) dropped, no session attached", label(), length);
        return false;
    }
    if (!s->ready()) {
        recordsDropped_++;
        {
            std::lock_guard<std::mutex> lock(gate_);
            appendDiagnosticLocked("out", "dropped-not-ready", opcode, flags, data, offset, length);
        }
        trace_->ws("{}: {} byte(s) dropped, Telnet negotiation incomplete (RFC 1205 section 2 needs BINARY, EOR and "
                   "TERMINAL-TYPE)",
                   label(), length);
        return false;
    }
    if (!s->send(opcode, flags, data, offset, length)) {
        recordsDropped_++;
        std::lock_guard<std::mutex> lock(gate_);
        appendDiagnosticLocked("out", "send-failed", opcode, flags, data, offset, length);
        return false;
    }
    recordsSent_++;
    bytesSent_ += length;
    {
        std::lock_guard<std::mutex> lock(gate_);
        appendDiagnosticLocked("out", "sent", opcode, flags, data, offset, length);
    }
    trace_->ws("{}: record out {} flags={} {} byte(s)", label(), opcodeName(opcode), flagsName(flags), length);
    return true;
}

bool WorkstationBackend::tryTakeInput(std::vector<uint8_t>& stream)
{
    WorkstationRecord r;
    if (!tryReceive(r)) {
        stream.clear();
        return false;
    }
    stream = std::move(r.data);
    return true;
}

// This must be read from the record still on the wire queue, NOT from the
// display's retained input: the event pump delivers the status BEFORE it
// completes the invite, so at status time the retained slot is still empty.
bool WorkstationBackend::pendingInputHasFields() const
{
    std::lock_guard<std::mutex> lock(gate_);
    return !inbound_.empty() && inbound_.front().data.size() > 3;
}

// Raw 5250 response layout is cursor[0..1], AID[2], fields[3..].
bool WorkstationBackend::tryTakeInputStatus(uint16_t& cursor, uint8_t& aid)
{
    std::lock_guard<std::mutex> lock(gate_);
    if (headStatusTaken_ || inbound_.empty() || inbound_.front().data.size() < 3) {
        cursor = 0;
        aid = 0;
        return false;
    }
    const std::vector<uint8_t>& data = inbound_.front().data;
    cursor = static_cast<uint16_t>((data[0] << 8) | data[1]);
    aid = data[2];
    headStatusTaken_ = true;
    return true;
}

bool WorkstationBackend::tryReceive(WorkstationRecord& record)
{
    std::lock_guard<std::mutex> lock(gate_);
    if (inbound_.empty()) return false;
    record = std::move(inbound_.front());
    inbound_.pop_front();
    headStatusTaken_ = false;
    return true;
}

bool WorkstationBackend::tryTakeSaveScreen(std::vector<uint8_t>& body)
{
    std::lock_guard<std::mutex> lock(gate_);
    if (saveResponses_.empty()) {
        body.clear();
        return false;
    }
    body = std::move(saveResponses_.front().data);
    saveResponses_.pop_front();
    return true;
}

void WorkstationBackend::injectInput(const WorkstationRecord& record) { enqueue(record); }

// ---- PrinterBackend --------------------------------------------------------------

namespace {

// RFC 2877 section 10, figure 3, "Layout of the printer pass-through
// header".  Bytes 0-1 length including the field, 2-3 the GDS identifier,
// 4-5 the data flow record, 6 the length of the pass-through header
// including itself, 7-8 flags, 9 the printer operation code, 10..LL a
// diagnostic area zero-padded to LL.  "The print data will start in byte
// LL+1."
constexpr int kGdsIdentifier = 0x12A0;                // on every record in both directions
constexpr int kFlowStartUpConfirmation = 0x8000;     // bit 0
constexpr int kFlowDiagnosticIncluded = 0x1000;      // bit 3
constexpr int kFlowPrinterRecord = 0x0100;           // bit 7
constexpr int kFlowServerOriginated = 0x0001;        // bit 15
constexpr uint8_t kFlagLastOfChain = 0x08;
constexpr uint8_t kFlagFirstOfChain = 0x10;

}  // namespace

PrinterBackend::~PrinterBackend()
{
    auto s = session();
    dispose();
    if (s != nullptr) s->waitForReader();
}

// Negotiation finished, so the virtual printer power-on sequence owes the
// client its answer (RFC 2877 section 9).
void PrinterBackend::onReady()
{
    std::vector<uint8_t> record = startupResponse(kResponseSessionStarted, systemName, objectName);
    if (sendRecord(record, 0, std::string("startup response ") + kResponseSessionStarted)) {
        startupResponsesSent_++;
        recordsSent_--;   // the handshake is not print traffic; do not inflate the count
    }
}

// The record is 73 bytes (0049), which is what the length field in both of
// the RFC's examples says.  The client reads it as o = 6 + record[6] (here
// 11) and then takes four EBCDIC bytes at o + 5, which lands on +16: that is
// why byte +6 is 05 here and 04 on a display record.
std::vector<uint8_t> PrinterBackend::startupResponse(const std::string& code, const std::string& systemName,
                                                     const std::string& objectName)
{
    std::vector<uint8_t> r(73, 0);
    r[0] = 0x00;
    r[1] = 0x49;   // length, including this field
    r[2] = 0x12;
    r[3] = 0xA0;   // GDS LU6.2 header
    // Start-Up confirmation (bit 0) | Diagnostic information included (bit 3).
    int flow = kFlowStartUpConfirmation | kFlowDiagnosticIncluded;
    r[4] = static_cast<uint8_t>(flow >> 8);
    r[5] = static_cast<uint8_t>(flow);
    r[6] = 0x05;   // pass-through header length
    r[7] = 0x60;
    r[8] = 0x06;   // flags, verbatim from figure 1
    r[9] = 0x00;   // printer operation code: none
    r[10] = 0x20;  // verbatim from figure 1
    r[11] = 0xC0;
    r[12] = 0x00;
    r[13] = 0x3D;  // verbatim from figure 1
    r[14] = 0x00;
    r[15] = 0x00;
    putEbcdic(r, 16, code, 4);
    putEbcdic(r, 20, systemName, 8);
    putEbcdic(r, 28, objectName, 10);
    // +38..72 stay zero: "diagnostic information padded with zeros".
    return r;
}

bool PrinterBackend::sendDataStream(const uint8_t* data, int offset, int length)
{
    std::vector<uint8_t> record = printRecord(data, offset, length, static_cast<uint8_t>(kFlagFirstOfChain | kFlagLastOfChain));
    return sendRecord(record, length, fmt::format("print record, {} byte(s) of data stream", length));
}

bool PrinterBackend::endJob()
{
    const uint8_t nul[] = {0x00};
    std::vector<uint8_t> record = printRecord(nul, 0, 1, kFlagLastOfChain);
    bool sent = sendRecord(record, 0, "null print record - end of job");
    if (sent) {
        jobsEnded_++;
        recordsSent_--;
    }
    return sent;
}

// One print record, RFC 2877 section 10 figure 4: LLLL 12A0 0101 0A 1800 01
// 000000000000 then the print data.
std::vector<uint8_t> PrinterBackend::printRecord(const uint8_t* data, int offset, int length, uint8_t flags)
{
    std::vector<uint8_t> r(static_cast<std::size_t>(kPrintRecordHeaderBytes + length), 0);
    int total = static_cast<int>(r.size());
    r[0] = static_cast<uint8_t>(total >> 8);
    r[1] = static_cast<uint8_t>(total);
    r[2] = static_cast<uint8_t>(kGdsIdentifier >> 8);
    r[3] = static_cast<uint8_t>(kGdsIdentifier & 0xFF);
    int flow = kFlowPrinterRecord | kFlowServerOriginated;   // 0101: server to client
    r[4] = static_cast<uint8_t>(flow >> 8);
    r[5] = static_cast<uint8_t>(flow);
    r[6] = kPrintHeaderLength;
    r[7] = flags;
    r[8] = 0x00;
    r[9] = kOpcodePrint;
    // +10..15 stay zero: "zero pad header to LL specified".
    if (length > 0) std::copy(data + offset, data + offset + length, r.begin() + kPrintRecordHeaderBytes);
    return r;
}

// The client answered.  RFC 2877 section 10.2: a print complete record is
// ten bytes, no data, and "indicates successful completion of a print
// request".  It is counted and traced rather than fed anywhere: on the
// System/36 side a printer request completes through the IOB's event
// control mask, and the request that produced this record has already been
// completed synchronously by the time the acknowledgement arrives.
void PrinterBackend::onRecord(const WorkstationRecord& r)
{
    countReceived(r);
    signalMachine();
    uint8_t op = static_cast<uint8_t>(r.opcode);
    if (op == kOpcodePrint) {
        printCompletesReceived_++;
        trace_->ws("{}: print complete ({} so far)", label(), printCompletesReceived_.load());
        return;
    }
    if (op == kOpcodeClear) {
        trace_->ws("{}: client sent Clear Print Buffers - recorded, not acted on: nothing on the guest path takes a "
                   "printer-originated clear",
                   label());
        return;
    }
    trace_->ws("{}: record in with printer operation code {:02X}, which is neither Print ({:02X}) nor Clear ({:02X}) - "
               "RFC 2877 section 10 names no other",
               label(), op, kOpcodePrint, kOpcodeClear);
}

// EBCDIC, blank-padded to a fixed width: everything in a 5250 record is
// EBCDIC, and 40 is the blank.
void PrinterBackend::putEbcdic(std::vector<uint8_t>& into, int at, const std::string& s, int width)
{
    for (int i = 0; i < width; i++)
        into[static_cast<std::size_t>(at + i)] =
            i < static_cast<int>(s.size()) ? ebcdic(s[static_cast<std::size_t>(i)]) : static_cast<uint8_t>(0x40);
}

// The subset of code page 037 the two names can contain: A-Z, 0-9 and a
// blank.  These are device and system names, and the RFC's own examples
// (TARGET, PCPRINTER) are within this subset.
uint8_t PrinterBackend::ebcdic(char c)
{
    if (c >= 'a' && c <= 'z') c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'I') return static_cast<uint8_t>(0xC1 + (c - 'A'));
    if (c >= 'J' && c <= 'R') return static_cast<uint8_t>(0xD1 + (c - 'J'));
    if (c >= 'S' && c <= 'Z') return static_cast<uint8_t>(0xE2 + (c - 'S'));
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(0xF0 + (c - '0'));
    return 0x40;
}

}  // namespace sim36::host
