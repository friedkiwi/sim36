#include "Host/StationMultiplexer.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <fmt/format.h>

#include "Storage/Ebcdic.h"

namespace sim36::host {

// ---- the screen builder ---------------------------------------------------------

// A 5250 output record, built from the same orders the guest itself uses:
// Clear Unit, Write To Display, Set Buffer Address, Start of Field, Insert
// Cursor and Read Input Fields.  Nothing exotic: an ordinary 5250 client
// renders it with no special case.
class StationMultiplexer::ScreenBuilder {
public:
    static constexpr int kRows = 24, kCols = 80;

    int length() const { return static_cast<int>(b_.size()); }
    const std::vector<uint8_t>& bytes() const { return b_; }

    void clearUnit()
    {
        b_.push_back(0x04); b_.push_back(0x40);   // ESC Clear Unit
        b_.push_back(0x04); b_.push_back(0x11);   // ESC Write To Display
        // CC1 0x00; CC2 0x08 = unlock keyboard.  A real 5250 client moves the
        // cursor to the Insert Cursor position only when the WTD unlocks the
        // keyboard; with CC2 0x00 it leaves the cursor where Clear Unit put
        // it, and the operator has to tab into the field.
        b_.push_back(0x00); b_.push_back(0x08);   // CC1, CC2
    }

    void text(int row, int col, std::string t)
    {
        if (col < 1) col = 1;
        if (col + static_cast<int>(t.size()) - 1 > kCols) t = t.substr(0, static_cast<std::size_t>(kCols - col + 1));
        if (t.empty()) return;
        sba(row, col);
        std::vector<uint8_t> e = storage::Ebcdic::fromAscii(t);
        b_.insert(b_.end(), e.begin(), e.end());
    }

    // One input field.  attrCol is where the leading attribute byte goes, so
    // the first data position is the column after it.  FFW 0x4800 is
    // input-capable, not bypass, alpha shift, with MDT set so a real client
    // returns the prefilled selection even when the operator presses Enter
    // without retyping it; attribute 0x20 is normal display.
    void field(int row, int attrCol, int length, const std::string& initial)
    {
        sba(row, attrCol);
        b_.push_back(0x1D);                       // SF
        b_.push_back(0x48); b_.push_back(0x00);   // FFW: input + MDT
        b_.push_back(0x20);                       // attribute
        b_.push_back(static_cast<uint8_t>(length >> 8));
        b_.push_back(static_cast<uint8_t>(length & 0xFF));
        // Re-address before writing the field's initial content: an
        // explicit SBA is correct whichever buffer-address convention the
        // client applies after an SF.
        sba(row, attrCol + 1);
        std::string padded = initial;
        padded.resize(static_cast<std::size_t>(length), ' ');
        std::vector<uint8_t> e = storage::Ebcdic::fromAscii(padded);
        b_.insert(b_.end(), e.begin(), e.end());
        // The declared length already ends the input field.  Do not append a
        // synthetic protected SF: a zero-length SF is 5250 negative response
        // 1005/01/25, which IBM Personal Communications correctly rejects.
    }

    void insertCursor(int row, int col)
    {
        b_.push_back(0x13); b_.push_back(static_cast<uint8_t>(row)); b_.push_back(static_cast<uint8_t>(col));
    }

    // ESC 42: Read Input Fields.  Its answer is cursor(2) + AID(1) + every
    // input field in screen order, with no SBA orders.
    void readInputFields()
    {
        b_.push_back(0x04); b_.push_back(0x42); b_.push_back(0x00); b_.push_back(0x00);
    }

private:
    void sba(int row, int col)
    {
        b_.push_back(0x11); b_.push_back(static_cast<uint8_t>(row)); b_.push_back(static_cast<uint8_t>(col));
    }

    std::vector<uint8_t> b_;
};

// ---- the conversation ------------------------------------------------------------

// One client, from accept until it is either handed to a station or hung
// up on.
class StationMultiplexer::Conversation {
public:
    static constexpr int kFieldLength = 6;
    static constexpr const char* kTitle = "SIM/36";
    static constexpr int kTitleWidth = 6;

    Conversation(StationMultiplexer& mux, SocketHandle client, std::string peer)
        : mux_(mux), client_(client), peer_(std::move(peer)) {}

    std::string label() const { return "multiplexer " + peer_; }

    void start()
    {
        session_ = std::make_shared<Telnet5250Session>(
            client_, peer_, label(), StationKind::Display, mux_.trace_, [this](const WorkstationRecord& r) { onRecord(r); },
            [this]() { onReady(); }, [this](const std::string& type, StationKind announced) { onMismatch(type, announced); });
        session_->setClosed([this]() {
            if (!handedOver_) mux_.forget(this);
        });
        session_->start();
    }

    bool handedOver() const { return handedOver_; }
    std::shared_ptr<Telnet5250Session> session() const { return session_; }

private:
    void onMismatch(const std::string& terminalType, StationKind)
    {
        mux_.trace_->ws("{}: REFUSED - the station multiplexer serves display stations and the client announced "
                        "TERMINAL-TYPE {}",
                        label(), terminalType);
        session_->dispose();
    }

    // Negotiation reached 5250 mode.  RFC 2877 section 4's DEVNAME, if the
    // client sent one, is acted on here, before a single byte of menu is
    // painted.
    void onReady()
    {
        std::string devname = session_->deviceName();
        if (!devname.empty()) {
            char kind;
            int number;
            std::string id;
            if (!tryParseSelection(devname, kind, number, id)) {
                message_ = "device name '" + devname + "' is not a station name";
            } else if (kind != 'W' && kind != '.') {
                message_ = "device name '" + devname + "': only W stations are attachable; " + std::string(1, kind) +
                           " devices are not implemented";
            } else if (tryHandOver(kind, number, id)) {
                return;
            }
        }
        paint();
    }

    std::vector<MultiplexStationView> stations() const
    {
        return mux_.machine_ != nullptr ? mux_.machine_->multiplexStations() : std::vector<MultiplexStationView>();
    }
    std::vector<std::string> mediaLines() const
    {
        return mux_.machine_ != nullptr ? mux_.machine_->mediaLines() : std::vector<std::string>();
    }
    std::string machineStatusText() const
    {
        return mux_.machine_ != nullptr ? mux_.machine_->machineStatusText() : std::string("stopped");
    }

    // Resolve, check availability, and hand the session over.  Sets the
    // message and returns false when it cannot.
    bool tryHandOver(char kind, int number, const std::string& id)
    {
        std::vector<MultiplexStationView> all = stations();
        const MultiplexStationView* v = lookup(all, kind, number, id);
        if (v == nullptr) {
            message_ = "no station " + (kind == '.' ? id : "W" + std::to_string(number)) + " on this machine";
            return false;
        }
        if (v->isConsole) {
            confirmingConsole_ = true;
            consoleKind_ = kind;
            consoleNumber_ = number;
            consoleId_ = id;
            paintConsoleConfirmation();
            return true;
        }
        return handOver(*v);
    }

    // A record from a client still sitting on the menu.  Once the session is
    // handed over this is never called again.
    void onRecord(const WorkstationRecord& r)
    {
        if (handedOver_) return;
        const std::vector<uint8_t>& d = r.data;
        if (hasFlag(r.flags, WorkstationRecordFlags::DataStreamOutputError)) {
            // This is a negative response to our last output, not keyboard
            // input.  Repainting the same stream creates an unbounded
            // error/repaint loop with strict IBM clients.
            mux_.trace_->ws("{}: selector output rejected by client: {}; retained without repaint",
                            label(), hexDash(d.data(), static_cast<int>(d.size())));
            return;
        }
        if (d.size() < 3) {
            mux_.trace_->ws("{}: selector ignored short non-input response: {}", label(), r.toString());
            return;
        }
        uint8_t aid = d[2];
        if (confirmingConsole_) {
            std::string answer = aid == 0xF1 ? trim(ebcdic(d, 3, static_cast<int>(d.size()) - 3)) : std::string();
            confirmingConsole_ = false;
            if (answer == "Y" || answer == "y") {
                std::vector<MultiplexStationView> all = stations();
                const MultiplexStationView* console = lookup(all, consoleKind_, consoleNumber_, consoleId_);
                if (console != nullptr && handOver(*console)) return;
            }
            paint();
            return;
        }
        // Enter is the only key the menu acts on.  Anything else repaints,
        // which is what a real panel does with an unhandled AID.
        if (aid != 0xF1) {
            paint();
            return;
        }

        std::string typed = ebcdic(d, 3, static_cast<int>(d.size()) - 3);
        // Some 5250 clients answer Enter with cursor+AID only when the
        // operator accepts a prefilled field unchanged.  The menu owns that
        // default, so an absent field payload means exactly "accept the
        // displayed selection".  An explicitly cleared field is still
        // transmitted as blanks and remains invalid.
        if (d.size() == 3) typed = defaultSelection_;
        char kind;
        int number;
        std::string id;
        if (!tryParseSelection(typed, kind, number, id)) {
            message_ = trim(typed).empty() ? std::string("type a station: W<number>, or its port.address id")
                                           : "'" + trim(typed) + "' is not a station name";
            paint();
            return;
        }
        if (kind != 'W' && kind != '.') {
            message_ = "only W stations are attachable; " + std::string(1, kind) + " devices are not implemented";
            paint();
            return;
        }
        if (!tryHandOver(kind, number, id)) paint();
    }

    bool handOver(const MultiplexStationView& v)
    {
        if (!v.available || !mux_.isFree(v.id)) {
            message_ = "W" + std::to_string(v.number) + " (" + v.id + ") already has a client attached";
            return false;
        }
        if (v.backend != nullptr) {
            if (!v.backend->adoptSession(session_)) {
                message_ = "W" + std::to_string(v.number) + " (" + v.id + ") could not be attached";
                return false;
            }
        } else {
            session_->retarget([this](const WorkstationRecord& r) { discardBeforeIpl(r); }, nullptr, nullptr,
                               "multiplexer parked on " + v.id);
        }
        mux_.bind(v.id, session_);
        handedOver_ = true;
        mux_.sessionsHandedOver_++;
        mux_.trace_->ws("{}: placed on station {} (W{}){}", label(), v.id, v.number,
                        v.backend == nullptr ? " - parked until IPL" : "");
        if (v.backend == nullptr) paintParked(v);
        return true;
    }

    // A record from a client parked on a station of a machine that is not
    // powered on.  There is nothing to deliver it to and nothing may be
    // invented on its behalf, so it is dropped with a trace.
    void discardBeforeIpl(const WorkstationRecord& r)
    {
        mux_.trace_->ws("{}: {} discarded - no machine is constructed", label(), r.toString());
    }

    // What a parked client sees while it waits for the machine.  No input
    // field and no read command: there is nothing for the operator to
    // answer until the guest is running.
    void paintParked(const MultiplexStationView& v)
    {
        ScreenBuilder b;
        b.clearUnit();
        b.text(1, centre(kTitleWidth), kTitle);
        int row = 4;
        for (const std::string& line : mediaLines()) {
            if (row > 12) break;
            b.text(row++, 3, line);
        }
        b.text(16, 3, fmt::format("Attached to W{} ({}). Waiting for IPL to construct the machine.", v.number, v.id));
        std::string status = "Machine status: " + machineStatusText();
        b.text(ScreenBuilder::kRows, ScreenBuilder::kCols - static_cast<int>(status.size()), status);
        session_->send(WorkstationOpcode::OutputOnly, WorkstationRecordFlags::None, b.bytes().data(), 0, b.length());
    }

    static std::string ebcdic(const std::vector<uint8_t>& d, int off, int len)
    {
        if (len <= 0) return std::string();
        return storage::Ebcdic::toAscii(d.data() + off, static_cast<std::size_t>(len));
    }

    static std::string trim(std::string s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    void paintConsoleConfirmation()
    {
        ScreenBuilder b;
        b.clearUnit();
        b.text(1, centre(kTitleWidth), kTitle);
        const int row = 16;
        b.text(row, 3, "Connecting to system console - continue?");
        fieldRow_ = row;
        fieldCol_ = 44;
        b.field(row, 43, 1, "_");
        b.text(row, 46, "(Y/N)");
        b.insertCursor(fieldRow_, fieldCol_);
        b.readInputFields();
        session_->send(WorkstationOpcode::PutGet, WorkstationRecordFlags::None, b.bytes().data(), 0, b.length());
    }

    void paint()
    {
        std::vector<MultiplexStationView> all = stations();
        ScreenBuilder b;
        b.clearUnit();

        b.text(1, centre(kTitleWidth), kTitle);

        // The media overview, and only that.  There is deliberately NO
        // station list: the addressable space is 8 ports by 7 addresses = 56
        // attachments and a list of those does not fit a 24x80 panel, while
        // the media block is bounded.  The input field accepts any configured
        // station whether or not anything on this screen names it.
        int row = 4;
        for (const std::string& line : mediaLines()) {
            if (row > 12) break;
            b.text(row++, 3, line);
        }

        // The prompt.  The hint names ONLY what can actually be chosen,
        // derived from the configured machine, so the menu never offers
        // something the code then refuses.
        const int promptRow = 16;
        b.text(promptRow, 3, "Connect to workstation . . .");
        fieldRow_ = promptRow;
        fieldCol_ = 33;
        defaultSelection_ = defaultSelection(all);
        b.field(promptRow, 32, kFieldLength, defaultSelection_);
        b.text(promptRow, 33 + kFieldLength + 3, selectableHint(all));

        if (!message_.empty()) b.text(ScreenBuilder::kRows - 1, 3, clip(message_, ScreenBuilder::kCols - 4));

        std::string status = "Machine status: " + machineStatusText();
        b.text(ScreenBuilder::kRows, ScreenBuilder::kCols - static_cast<int>(status.size()), status);

        b.insertCursor(fieldRow_, fieldCol_);
        b.readInputFields();
        message_.clear();
        session_->send(WorkstationOpcode::PutGet, WorkstationRecordFlags::None, b.bytes().data(), 0, b.length());
    }

    // The lowest-numbered available station, or empty when the machine has
    // none free.
    static std::string defaultSelection(const std::vector<MultiplexStationView>& stations)
    {
        for (const MultiplexStationView& v : stations)
            if (!v.isConsole && v.available) return "W" + std::to_string(v.number);
        return std::string();
    }

    // "(W2-W7)": the range that can actually be picked, derived from the
    // configured machine rather than a constant.
    static std::string selectableHint(const std::vector<MultiplexStationView>& stations)
    {
        int low = 0, high = 0, count = 0;
        for (const MultiplexStationView& v : stations) {
            if (v.isConsole) continue;
            if (count == 0 || v.number < low) low = v.number;
            if (count == 0 || v.number > high) high = v.number;
            count++;
        }
        if (count == 0) return "(no attachable station)";
        if (low == high) return "(W" + std::to_string(low) + ")";
        return "(W" + std::to_string(low) + "-W" + std::to_string(high) + ")";
    }

    static int centre(int width) { return std::max(1, (ScreenBuilder::kCols - width) / 2 + 1); }

    static std::string clip(const std::string& s, int max)
    {
        return static_cast<int>(s.size()) <= max ? s : s.substr(0, static_cast<std::size_t>(max));
    }

    StationMultiplexer& mux_;
    SocketHandle client_;
    std::string peer_;
    std::shared_ptr<Telnet5250Session> session_;
    std::atomic<bool> handedOver_{false};
    std::string message_;
    std::string defaultSelection_;
    int fieldRow_ = 0, fieldCol_ = 0;
    bool confirmingConsole_ = false;
    char consoleKind_ = '\0';
    int consoleNumber_ = 0;
    std::string consoleId_;
};

// ---- the multiplexer ----------------------------------------------------------------

StationMultiplexer::StationMultiplexer(const std::string& host, int port, monitor::Tracer* trace,
                                       IStationMultiplexerHost* machine)
    : host_(host.empty() ? kDefaultHost : host), port_(port == 0 ? kDefaultPort : port), trace_(trace), machine_(machine)
{
}

StationMultiplexer::~StationMultiplexer() { dispose(); }

bool StationMultiplexer::isFree(const std::string& stationId)
{
    std::lock_guard<std::mutex> lock(bindGate_);
    auto it = bound_.find(stationId);
    if (it == bound_.end()) return true;
    if (it->second != nullptr && it->second->connected()) return false;
    bound_.erase(it);
    return true;
}

void StationMultiplexer::bind(const std::string& stationId, std::shared_ptr<Telnet5250Session> session)
{
    std::lock_guard<std::mutex> lock(bindGate_);
    bound_[stationId] = std::move(session);
}

void StationMultiplexer::forget(Conversation* conversation)
{
    std::shared_ptr<Conversation> gone;
    {
        std::lock_guard<std::mutex> lock(conversationGate_);
        auto it = conversations_.find(conversation);
        if (it == conversations_.end()) return;
        gone = std::move(it->second);
        conversations_.erase(it);
    }
    // Called from the session's own reader thread: the session outlives
    // this call (it holds itself alive for the reader's lifetime), so the
    // conversation can go now.
}

void StationMultiplexer::rebind()
{
    if (machine_ == nullptr) return;
    std::vector<MultiplexStationView> stations = machine_->multiplexStations();
    std::lock_guard<std::mutex> lock(bindGate_);
    std::vector<std::string> gone;
    for (auto& pair : bound_) {
        std::shared_ptr<Telnet5250Session> session = pair.second;
        if (session == nullptr || !session->connected()) {
            gone.push_back(pair.first);
            continue;
        }
        const MultiplexStationView* v = nullptr;
        for (const MultiplexStationView& candidate : stations)
            if (candidate.id == pair.first) {
                v = &candidate;
                break;
            }
        if (v == nullptr || v->backend == nullptr) {
            // The station this client picked is not in the machine that was
            // just constructed.  Say so rather than leaving a socket attached
            // to nothing.
            trace_->ws("multiplexer: station {} is not in this machine; its client is disconnected", pair.first);
            session->dispose();
            gone.push_back(pair.first);
            continue;
        }
        if (!v->backend->adoptSession(session)) trace_->ws("multiplexer: station {} refused the re-bind", pair.first);
    }
    for (const std::string& id : gone) bound_.erase(id);
}

void StationMultiplexer::reclaim()
{
    if (machine_ == nullptr) return;
    std::vector<MultiplexStationView> stations = machine_->multiplexStations();
    std::lock_guard<std::mutex> lock(bindGate_);
    std::vector<std::string> gone;
    for (auto& pair : bound_) {
        const MultiplexStationView* v = nullptr;
        for (const MultiplexStationView& candidate : stations)
            if (candidate.id == pair.first) {
                v = &candidate;
                break;
            }
        if (v != nullptr && v->backend != nullptr) v->backend->releaseSession();
        if (pair.second == nullptr || !pair.second->connected()) gone.push_back(pair.first);
    }
    for (const std::string& id : gone) bound_.erase(id);
}

void StationMultiplexer::rebind(const std::string& host, int port)
{
    stopListening();
    host_ = host.empty() ? kDefaultHost : host;
    port_ = port == 0 ? kDefaultPort : port;
    stopping_ = false;
    listen();
}

void StationMultiplexer::listen()
{
    std::string error;
    listener_ = Sockets::listenOn(host_, port_, error);
    if (listener_ == kInvalidSocket) throw std::runtime_error("multiplexer: cannot listen on " + endpoint() + ": " + error);
    stopping_ = false;
    accept_ = std::thread([this]() { acceptLoop(); });
    trace_->ws("multiplexer: listening on {}", endpoint());
}

void StationMultiplexer::acceptLoop()
{
    while (!stopping_) {
        if (!Sockets::waitReadable(listener_, 250)) continue;
        if (stopping_) return;
        std::string peer;
        SocketHandle client = Sockets::accept(listener_, peer);
        if (client == kInvalidSocket) return;
        if (stopping_) {
            Sockets::close(client);
            return;
        }

        sessionsAccepted_++;
        // Every connection gets its own conversation object.  Unlike a
        // station endpoint the multiplexer serves an unbounded number of them
        // at once: a client sitting on the menu owns no station.
        auto conversation = std::make_shared<Conversation>(*this, client, peer);
        {
            std::lock_guard<std::mutex> lock(conversationGate_);
            // Conversations whose client has been placed and has since hung
            // up are released here.
            for (auto it = conversations_.begin(); it != conversations_.end();) {
                auto s = it->second->session();
                if (it->second->handedOver() && (s == nullptr || !s->connected())) it = conversations_.erase(it);
                else ++it;
            }
            conversations_[conversation.get()] = conversation;
        }
        conversation->start();
    }
}

bool StationMultiplexer::tryParseSelection(const std::string& text, char& kind, int& number, std::string& stationId)
{
    kind = '\0';
    number = 0;
    stationId.clear();
    if (text.empty()) return false;
    std::string s;
    for (char c : text) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return false;

    auto digits = [](const std::string& t) {
        if (t.empty()) return false;
        for (char c : t)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return t.size() <= 9;
    };

    std::size_t dot = s.find('.');
    if (dot != std::string::npos && dot > 0) {
        std::string p = s.substr(0, dot), a = s.substr(dot + 1);
        if (a.find('.') == std::string::npos && digits(p) && digits(a)) {
            kind = '.';
            stationId = std::to_string(std::stoi(p)) + "." + std::to_string(std::stoi(a));
            return true;
        }
        return false;
    }

    if (!std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    kind = s[0];
    std::string rest = s.substr(1);
    if (!digits(rest)) return false;
    number = std::stoi(rest);
    return number > 0;
}

const MultiplexStationView* StationMultiplexer::lookup(const std::vector<MultiplexStationView>& stations, char kind,
                                                       int number, const std::string& stationId)
{
    for (const MultiplexStationView& v : stations)
        if (kind == '.' ? v.id == stationId : (kind == 'W' && v.number == number)) return &v;
    return nullptr;
}

void StationMultiplexer::stopListening()
{
    stopping_ = true;
    if (accept_.joinable()) {
        if (accept_.get_id() == std::this_thread::get_id()) accept_.detach();
        else accept_.join();
    }
    Sockets::close(listener_);
    listener_ = kInvalidSocket;
}

void StationMultiplexer::dispose()
{
    stopListening();
    std::map<std::string, std::shared_ptr<Telnet5250Session>> bound;
    {
        std::lock_guard<std::mutex> lock(bindGate_);
        bound.swap(bound_);
    }
    for (auto& pair : bound) {
        if (pair.second == nullptr) continue;
        trace_->ws("multiplexer: station {}'s client is disconnected - the multiplexer is being turned off", pair.first);
        pair.second->dispose();
    }
    std::map<Conversation*, std::shared_ptr<Conversation>> conversations;
    {
        std::lock_guard<std::mutex> lock(conversationGate_);
        conversations.swap(conversations_);
    }
    for (auto& pair : conversations) {
        auto s = pair.second->session();
        if (s != nullptr) {
            s->dispose();
            s->waitForReader();
        }
    }
    for (auto& pair : bound)
        if (pair.second != nullptr) pair.second->waitForReader();
}

}  // namespace sim36::host
