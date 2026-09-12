// The work-station monitor commands: the station inventory, the harness
// that stands in for the guest on the work-station device path, the console
// attachment, the display transfer and sign-on producers, and the read-only
// decoders of the guest's unit-block topology and its classifier.
#include "Monitor/MonitorCli.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Devices/UnitBlock.h"
#include "Devices/VirtualFixedDisk.h"
#include "Devices/WorkStationIob.h"
#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Storage/Ebcdic.h"

namespace sim36::monitor {

using devices::UnitBlock;
using devices::VirtualPrinter;
using devices::VirtualWorkstation;
using devices::WorkStationController;
using devices::WorkStationIob;
using devices::WorkStationSlot;
using processors::controlstorage::ActionControlElement;
using processors::controlstorage::As36ControlStorageProcessor;
using processors::controlstorage::GuestLowStorage;
using processors::controlstorage::RequestBlock;
using processors::controlstorage::TaskBlock;
using storage::DiskBackend;
using storage::Ebcdic;

namespace {

// Convert.ToInt32(s, 16): optional 0x prefix, hex digits only.
int hex32(const std::string& s)
{
    std::string h = s;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.empty()) throw MonitorError("Could not find any recognizable digits.");
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            throw MonitorError("Could not find any recognizable digits.");
    if (h.size() > 8) throw MonitorError("Value was either too large or too small for an Int32.");
    unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    if (v > 0x7FFFFFFFUL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

// byte.Parse(s, NumberStyles.HexNumber): hex digits only, no prefix.
bool tryHexByte(const std::string& s, uint8_t& out)
{
    if (s.empty() || s.size() > 2) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<uint8_t>(std::strtoul(s.c_str(), nullptr, 16));
    return true;
}

uint8_t hexByte(const std::string& s)
{
    uint8_t v;
    if (!tryHexByte(s, v)) throw MonitorError("Input string was not in a correct format.");
    return v;
}

int decimal(const std::string& s)
{
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    std::size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i == s.size()) throw MonitorError("Input string was not in a correct format.");
    for (std::size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k])))
            throw MonitorError("Input string was not in a correct format.");
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    if (v > 2147483647LL || v < -2147483648LL)
        throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

// int.TryParse with NumberStyles.None: unsigned digits only.
bool tryDigits(const std::string& s, int& out)
{
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::atoi(s.c_str());
    return true;
}

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string stripHash(std::string s)
{
    while (!s.empty() && s[0] == '#') s.erase(s.begin());
    return s;
}

const char* boolText(bool v) { return v ? "True" : "False"; }

std::string joinFrom(const std::vector<std::string>& a, std::size_t from)
{
    std::string s;
    for (std::size_t i = from; i < a.size(); ++i) {
        if (i != from) s += " ";
        s += a[i];
    }
    return s;
}

std::string hexJoin(const std::vector<int>& v, const char* fmtSpec = "{:06X}")
{
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) s += ", ";
        s += fmt::format(fmtSpec, v[i]);
    }
    return s;
}

// BitConverter.ToString(bytes).Replace("-", " ")
std::string bytesHex(const uint8_t* b, int n)
{
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i != 0) s += " ";
        s += fmt::format("{:02X}", b[i]);
    }
    return s;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// A-Z, a-z, 0-9, blank; anything else becomes a blank.  Enough to see
// characters come out of a real client, and no more: a full code page
// belongs with a data stream this emulator does not build.
uint8_t asciiToEbcdic(char c)
{
    if (c >= 'a' && c <= 'i') return static_cast<uint8_t>(0x81 + (c - 'a'));
    if (c >= 'j' && c <= 'r') return static_cast<uint8_t>(0x91 + (c - 'j'));
    if (c >= 's' && c <= 'z') return static_cast<uint8_t>(0xA2 + (c - 's'));
    if (c >= 'A' && c <= 'I') return static_cast<uint8_t>(0xC1 + (c - 'A'));
    if (c >= 'J' && c <= 'R') return static_cast<uint8_t>(0xD1 + (c - 'J'));
    if (c >= 'S' && c <= 'Z') return static_cast<uint8_t>(0xE2 + (c - 'S'));
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(0xF0 + (c - '0'));
    return 0x40;
}

// Set Buffer Address then text.  SBA is order hex 11 with a one-based row
// and column.
void appendAt(std::vector<uint8_t>& b, int row, int col, const std::string& text)
{
    b.push_back(0x11);
    b.push_back(static_cast<uint8_t>(row));
    b.push_back(static_cast<uint8_t>(col));
    const std::vector<uint8_t> e = Ebcdic::fromAscii(text);
    b.insert(b.end(), e.begin(), e.end());
}

// A 5250 data stream that puts legible text on a display, so a client can
// be seen to render bytes that came through the seam.
//
// This is not SSP's sign-on screen and does not pretend to be: it is three
// orders out of SA21-9247 - Clear Unit, Write to Display, Set Buffer
// Address - chosen because they are the smallest stream a conforming client
// must draw.  The real screen will arrive from the guest as bytes this
// harness never sees.
std::vector<uint8_t> demonstrationScreen()
{
    std::vector<uint8_t> b;
    b.push_back(0x04); b.push_back(0x40);   // ESC, Clear Unit
    b.push_back(0x04); b.push_back(0x11);   // ESC, Write to Display
    b.push_back(0x00); b.push_back(0x08);   // control characters 1 and 2; CC2 bit
                                            // 0x08 unlocks the keyboard, without
                                            // which the client sits on X SYSTEM
                                            // and the screen looks dead
    appendAt(b, 3, 20, "S36REFEMU WORK STATION PATH");
    appendAt(b, 5, 20, "STATION IS ATTACHED");
    appendAt(b, 7, 20, "DATA STREAM FORWARDED VERBATIM");
    appendAt(b, 9, 20, "SA21-9436 5-50");
    return b;
}

// The Write Structured Field Query of RFC 1205 section 4.1, byte for byte:
// "04 F3 00 05 D9 70 00".  A conforming client answers with a Query Reply
// describing itself, which is how the inbound half of the seam can be shown
// working without anyone touching a keyboard.
std::vector<uint8_t> queryCommand() { return {0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00}; }

// Translate a compiled $SFGR screen format into a 5250 Write-To-Display,
// matching what SSP's display manager would emit.  The sign-on format's
// grammar is `11 RR CC AA <EBCDIC>` SBA label records (from bodyOff on) and
// `11 RR CC 1D AT <FCW> <00 LEN>` field records (before it).  Labels are
// copied VERBATIM; field start-of-field orders are reconstructed.
std::vector<uint8_t> buildSignOnStream(const std::vector<uint8_t>& fmtBytes, int bodyOff, bool withFields)
{
    std::vector<uint8_t> b;
    b.push_back(0x04); b.push_back(0x40);               // ESC, Clear Unit
    b.push_back(0x04); b.push_back(0x11);               // ESC, Write to Display
    b.push_back(0x00); b.push_back(0x08);               // CC1=00; CC2=08 unlocks the keyboard

    // (1) visible label body, VERBATIM off disk (guest's own EBCDIC),
    //     trimming only trailing NULs.  This is the genuine on-disk panel:
    //     0x11 RR CC SBA orders and EBCDIC text, already ~1:1 with a 5250
    //     Write-To-Display body, so a real 5250 client renders it directly.
    int end = static_cast<int>(fmtBytes.size());
    while (end > bodyOff && fmtBytes[end - 1] == 0x00) end--;
    for (int i = bodyOff; i < end; i++) b.push_back(fmtBytes[i]);

    // Locate the User ID entry column (first `11 06 CC 1D` field record in
    // the field-control table) so the cursor can be homed there even when
    // the (unfaithful) field reconstruction is off.
    int userRow = 6, userCol = 0x37 + 1;
    for (int i = 0x30; i + 4 < bodyOff; i++)
        if (fmtBytes[i] == 0x11 && fmtBytes[i + 1] == 0x06 && fmtBytes[i + 3] == 0x1D) {
            userCol = fmtBytes[i + 2] + 1;
            break;
        }

    // (2) OPT-IN ONLY, BEST-EFFORT / UNFAITHFUL: input fields reconstructed
    //     from the compiled field-control table (`11 RR CC 1D AT ... 00
    //     LEN`).  The compiled S/36 field attribute is NOT a 5250 Field
    //     Format Word, so a stock 5250 client mis-parses these Start-of-Field
    //     control words; a faithful render needs the $SFGR-time field
    //     expansion the emulator does not run.  Off by default.
    if (withFields) {
        for (int i = 0x30; i + 5 < bodyOff; i++) {
            if (fmtBytes[i] != 0x11 || fmtBytes[i + 3] != 0x1D) continue;
            const int row = fmtBytes[i + 1], col = fmtBytes[i + 2], attr = fmtBytes[i + 4];
            if (row == 0 || row > 27 || col == 0 || col > 78) continue;
            int len = 0;
            for (int j = i + 5; j < bodyOff - 2 && j < i + 18; j++)
                if ((fmtBytes[j] == 0x30 || fmtBytes[j] == 0x37) && fmtBytes[j + 1] == 0x00 && fmtBytes[j + 2] != 0x00) {
                    len = fmtBytes[j + 2];
                    break;
                }
            if (len <= 0 || len > 60) len = 8;
            if (col + len + 1 > 80) len = 80 - col - 1;
            b.push_back(0x11); b.push_back(static_cast<uint8_t>(row)); b.push_back(static_cast<uint8_t>(col));
            b.push_back(0x1D); b.push_back(static_cast<uint8_t>(attr));
            for (int k = 0; k < len; k++) b.push_back(0x00);
            b.push_back(0x11); b.push_back(static_cast<uint8_t>(row)); b.push_back(static_cast<uint8_t>(col + len + 1));
            b.push_back(0x1D); b.push_back(0x20);
        }
    }

    // (3) Insert Cursor at the User ID entry position (order 0x13 RR CC)
    b.push_back(0x13); b.push_back(static_cast<uint8_t>(userRow)); b.push_back(static_cast<uint8_t>(userCol));
    return b;
}

// Where the harness lays the IOB and the data stream it is pretending SSP
// built.  High enough to be clear of low storage, of phase 1, and of
// `diskread`'s landing area.
constexpr int kWorkstationIoBlockAddress = 0x30000;
constexpr int kWorkstationStreamBuffer = 0x30100;
constexpr int kDiskReadBuffer = 0x20000;

}  // namespace

bool MonitorCli::CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const
{
    return toLower(a) < toLower(b);
}

// ---- the station inventory ----------------------------------------------

void MonitorCli::stations()
{
    auto& csp = m_.nativeControlStorage();
    // With the multiplexer on, no station has a listener of its own - one
    // port serves them all - so say so rather than letting seven "(no
    // listener)" lines read as a broken configuration.
    if (m_.config.stationMultiplex)
        fmt::print("  station multiplexer on {}:{}: one listener serves every display station\n",
                   m_.config.multiplexHost, m_.config.multiplexPort);
    for (auto& sp : m_.stations()) {
        VirtualWorkstation& s = *sp;
        host::WorkstationBackend& b = s.backend();
        // Resolve a not-yet-linked station's guest TUB from the guest's own
        // TUB+12 unit field (the IPL built one per configured station), so
        // `stations` reports each station's own block even before it has
        // issued an SVC 43.  Learned, not chosen.
        int tub = s.tubAddress;
        if (tub == 0) tub = csp.resolveTubByUnit((s.port() << 4) | s.address());
        fmt::print("  {}  device {}  {:<20} {}{}\n", s.id(), s.deviceCode(),
                   b.listening() ? b.endpoint()
                   : m_.config.stationMultiplex && !s.isConsole()
                       ? "mux " + m_.config.multiplexHost + ":" + std::to_string(m_.config.multiplexPort)
                       : "(no listener)",
                   b.attached() ? (b.ready() ? "attached" : "attached, negotiating") : "idle",
                   s.isConsole() ? "  (console)" : "");
        fmt::print("        role {:<8} tub {:<10} invite {}\n", s.role(),
                   tub == 0 ? std::string("not built") : fmt::format("{:06X}", tub),
                   s.inviteOutstanding() ? "outstanding" : "none");
        if (!b.terminalType().empty() || !b.deviceName().empty() || !b.userName().empty())
            fmt::print("        terminal {}  devname {}  user {}\n",
                       b.terminalType().empty() ? "-" : b.terminalType(),
                       b.deviceName().empty() ? "-" : b.deviceName(),
                       b.userName().empty() ? "-" : b.userName());
        fmt::print("        out {} record(s) {} byte(s), {} dropped; in {} record(s) {} byte(s), {} waiting; {} session(s)\n",
                   b.recordsSent(), b.bytesSent(), b.recordsDropped(), b.recordsReceived(), b.bytesReceived(),
                   b.pendingInput(), b.sessionsAccepted());
        if (b.sessionsRejected() > 0)
            fmt::print("        {} session(s) REFUSED for announcing a printer terminal type at a display slot\n",
                       b.sessionsRejected());
    }

    for (auto& pp : m_.printers()) {
        VirtualPrinter& p = *pp;
        host::PrinterBackend& b = p.backend();
        fmt::print("  {}  device {}  {:<20} {}\n", p.id(), p.deviceCode(),
                   b.listening() ? b.endpoint() : std::string("(no listener)"),
                   b.attached() ? (b.ready() ? "attached" : "attached, negotiating") : "idle");
        fmt::print("        role printer   pub {:<10} object name {}\n",
                   p.pubAddress == 0 ? std::string("not built") : fmt::format("{:06X}", p.pubAddress), b.objectName);
        if (!b.terminalType().empty() || !b.deviceName().empty())
            fmt::print("        terminal {}  devname {}\n", b.terminalType().empty() ? "-" : b.terminalType(),
                       b.deviceName().empty() ? "-" : b.deviceName());
        fmt::print("        out {} print record(s) {} byte(s), {} dropped; {} startup response(s), {} job(s) ended, "
                   "{} print complete(s) in; {} session(s)\n",
                   b.recordsSent(), b.bytesSent(), b.recordsDropped(), b.startupResponsesSent(), b.jobsEnded(),
                   b.printCompletesReceived(), b.sessionsAccepted());
        if (b.sessionsRejected() > 0)
            fmt::print("        {} session(s) REFUSED for announcing a display terminal type at a printer slot\n",
                       b.sessionsRejected());
    }
}

void MonitorCli::listenerAutoSignOn(const std::vector<std::string>& a)
{
    if (a.size() == 1) {
        fmt::print("listener-auto-signon {}\n", m_.config.listenerAutoSignOn ? "on" : "off");
        return;
    }
    if (a.size() != 2 || (!equalsIgnoreCase(a[1], "on") && !equalsIgnoreCase(a[1], "off"))) {
        fmt::print("listener-auto-signon [on|off]\n");
        return;
    }
    m_.config.listenerAutoSignOn = equalsIgnoreCase(a[1], "on");
    fmt::print("listener-auto-signon {}; applies to the next listener bind\n",
               m_.config.listenerAutoSignOn ? "on" : "off");
}

// ---- the work station harness -------------------------------------------
//
// These commands stand in for the guest.  `wswrite` is the real path: it
// lays an IOB in guest storage and issues SVC 43 through the control
// processor, exactly as `diskread` does for SVC 40, so what reaches the wire
// went through every layer SSP's own request will.

VirtualWorkstation* MonitorCli::findStation(const std::string& id)
{
    for (auto& s : m_.stations())
        if (s->id() == id || (toLower(id) == "console" && s->isConsole())) return s.get();
    fmt::print("no station '{}'; configured:\n", id);
    for (auto& s : m_.stations()) fmt::print("  {}\n", s->id());
    return nullptr;
}

VirtualPrinter* MonitorCli::findPrinter(const std::string& id)
{
    for (auto& p : m_.printers())
        if (p->id() == id) return p.get();
    fmt::print("no printer '{}'; configured:\n", id);
    for (auto& p : m_.printers()) fmt::print("  {}\n", p->id());
    if (m_.printers().empty()) fmt::print("  (none - add a [station p.a] with role = printer)\n");
    return nullptr;
}

VirtualWorkstation* MonitorCli::findDisplayStation(const std::string& name)
{
    for (auto& s : m_.stations())
        if (!s->isPrinter() && equalsIgnoreCase(s->id(), name)) return s.get();
    if (name.size() >= 2 && (name[0] == 'W' || name[0] == 'w')) {
        int number;
        if (tryDigits(name.substr(1), number) && number >= 1 && number <= 7)
            for (auto& s : m_.stations())
                if (!s->isPrinter() && s->port() == 0 && s->address() == number - 1) return s.get();
    }
    return nullptr;
}

VirtualWorkstation* MonitorCli::stationById(const std::string& id, bool displayOnly)
{
    for (auto& s : m_.stations())
        if (s->id() == id && (!displayOnly || !s->isPrinter())) return s.get();
    return nullptr;
}

void MonitorCli::workstationOutput(const std::vector<std::string>& a)
{
    if (a.size() != 3 && a.size() != 4) {
        fmt::print("usage: wsoutput <station-id> passthrough|wtd|slic-display\n");
        fmt::print("       wsoutput <station-id> invite-read 20|21\n");
        return;
    }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;
    const std::string mode = toLower(a[2]);
    if (mode == "invite-read" && a.size() == 4) {
        if (a[3] == "20") station->setInviteReadMode(devices::PutWithInviteReadMode::ReadInputFields20);
        else if (a[3] == "21") station->setInviteReadMode(devices::PutWithInviteReadMode::StructuredField21);
        else {
            fmt::print("unknown PUT-with-invite operation '{}' - use 20 or 21\n", a[3]);
            return;
        }
        fmt::print("station {}: PUT-with-invite operation 0x{}\n", station->id(), a[3]);
        return;
    }
    if (a.size() != 3) {
        fmt::print("usage: wsoutput <station-id> invite-read 20|21\n");
        return;
    }
    if (mode == "passthrough") station->setOutputMode(devices::WorkstationOutputMode::PassThrough);
    else if (mode == "wtd") station->setOutputMode(devices::WorkstationOutputMode::WtdText);
    else if (mode == "slic-display") station->setOutputMode(devices::WorkstationOutputMode::SlicDisplay);
    else {
        fmt::print("unknown output mode '{}' - use passthrough, wtd, or slic-display\n", a[2]);
        return;
    }
    const char* selected = station->outputMode() == devices::WorkstationOutputMode::PassThrough ? "passthrough"
                           : station->outputMode() == devices::WorkstationOutputMode::WtdText ? "wtd" : "slic-display";
    fmt::print("station {}: output mode {}\n", station->id(), selected);
}

// Lay the harness IOB: class C0 at +0x0A (the class byte has to be presented
// as `Cn` or the printer entry refuses the request before reading the
// command at all; this harness used to leave it zero, so every request it
// built was refused on the class check and nothing ever reached a backend,
// while the failure read as "nothing attached"), the command at +0x0B, the
// unit at +0x0C, the buffer address at +0x0D and the length at +0x10.
void MonitorCli::layWorkstationIob(int unit, const std::vector<uint8_t>& stream)
{
    auto& st = m_.state;
    for (int i = 0; i < 48; i++) st.writeByte(kWorkstationIoBlockAddress + i, 0);
    st.write(kWorkstationStreamBuffer, stream.data(), static_cast<int>(stream.size()));
    st.writeByte(kWorkstationIoBlockAddress + WorkStationIob::kOffClass, WorkStationIob::kClassWorkStation);
    st.writeByte(kWorkstationIoBlockAddress + WorkStationIob::kOffCommand, static_cast<uint8_t>(WorkStationIob::kCmdPut));
    st.writeByte(kWorkstationIoBlockAddress + WorkStationIob::kOffUnitAddress, static_cast<uint8_t>(unit));
    st.writeAddr24(kWorkstationIoBlockAddress + WorkStationIob::kOffDataBuffer, kWorkstationStreamBuffer);
    st.writeHalf(kWorkstationIoBlockAddress + WorkStationIob::kOffLength, static_cast<uint16_t>(stream.size()));
}

// `21` Output Data to a printer, through SVC 42 and the real path: an IOB in
// guest storage, the printer's unit address at +0x0C, the data stream
// address at +0x0D and its length at +0x10, then the supervisor call through
// the control processor.  `42` rather than `43` because SA21-9436 calls `42`
// the printer entry point; what selects the device is the unit address.
void MonitorCli::printerWrite(const std::vector<std::string>& a)
{
    if (a.size() < 3) {
        fmt::print("prtwrite <id> <hex>|@file|<text...>\n");
        return;
    }
    VirtualPrinter* printer = findPrinter(a[1]);
    if (printer == nullptr) return;

    std::vector<uint8_t> stream;
    if (!printerDataStream(a, 2, stream)) return;
    if (stream.size() > 0xFFFF) { fmt::print("data stream too long\n"); return; }

    layWorkstationIob((printer->port() << 4) | printer->address(), stream);

    const long long before = printer->backend().recordsSent();
    if (issueDeviceSvc(0x42, kWorkstationIoBlockAddress) < 0) return;

    fmt::print("printer {}: {} byte(s) of data stream, {}\n", printer->id(), stream.size(),
               printer->backend().recordsSent() > before
                   ? std::string("on the wire as an RFC 2877 print record")
                   : "not sent - " + std::string(printer->backend().attached() ? "the session is still negotiating"
                                                                                 : "no session attached"));
    fmt::print("{}", hexDump(stream.data(), std::min<int>(64, static_cast<int>(stream.size())), kWorkstationStreamBuffer));
}

// The bytes to print: hex, a file, or plain text translated to EBCDIC.
// Text is offered because a print data stream is mostly characters, and the
// translation is deliberately trivial: it is the harness making something
// legible, not the emulator claiming to build an SCS stream.
bool MonitorCli::printerDataStream(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out)
{
    const std::string& first = a[from];
    if (!first.empty() && first[0] == '@') {
        const std::string path = first.substr(1);
        if (!readFile(path, out)) { fmt::print("no such file: {}\n", path); return false; }
        return true;
    }
    bool hex = true;
    for (std::size_t i = from; i < a.size() && hex; i++)
        for (char c : a[i])
            if (!std::isxdigit(static_cast<unsigned char>(c))) { hex = false; break; }
    if (hex) return parseHexBytes(a, from, out);

    const std::string text = joinFrom(a, from);
    out.clear();
    for (char c : text) out.push_back(asciiToEbcdic(c));
    return true;
}

// End the print job: RFC 2877 section 10.3's null print record, which is
// what makes a client close its output command and actually print.
// Operator policy, not a guest event: nothing recovered from the System/36
// side means "end of spool file".
void MonitorCli::printerEndJob(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("prtend <id>\n"); return; }
    VirtualPrinter* printer = findPrinter(a[1]);
    if (printer == nullptr) return;
    fmt::print("printer {}: end of job {}\n", printer->id(), printer->endJob() ? "sent" : "dropped");
}

// Operator access to the console attachment.  Address 0.0 is the console on
// every 5250 machine and is attached to this interface rather than to a
// listener, so this is how an operator reads it and answers it.
//
//   console                 replay the decoded operation log
//   console clear           empty the log
//   console fields          the format table, with each field's record offset
//   console put R C TEXT    type into the input field covering R,C
//   console send KEY        send Enter / PF1..PF24 / Clear / Help / RollUp
void MonitorCli::consoleCommand(const std::vector<std::string>& a)
{
    host::WorkstationBackend* backend = findConsoleBackend();
    if (backend == nullptr) {
        fmt::print("no console attachment: no station has `role console`\n");
        return;
    }
    host::ConsoleDisplay& console = *backend->console();
    const std::string sub = a.size() > 1 ? toLower(a[1]) : "show";
    if (sub == "show") {
        if (console.logCount() == 0) fmt::print("console: nothing written yet\n");
        else
            for (const std::string& line : console.log()) fmt::print("console: {}\n", line);
    } else if (sub == "clear") {
        console.clearLog();
        fmt::print("console: log cleared\n");
    } else if (sub == "put") {
        if (a.size() < 5) { fmt::print("usage: console put <row> <col> <text>\n"); return; }
        const int row = decimal(a[2]), col = decimal(a[3]);
        std::string text = joinFrom(a, 4);
        while (!text.empty() && (text.front() == '\'' || text.front() == '"')) text.erase(text.begin());
        while (!text.empty() && (text.back() == '\'' || text.back() == '"')) text.pop_back();
        // The value is held on the FIELD, not in one monitor-wide string:
        // the attended-IPL console sign-on wants User ID, Date and Time in
        // the same transmitted record.
        host::ConsoleField* f = console.typeInto(row, col, text);
        if (f == nullptr) {
            fmt::print("console: no input field covers {},{} - `console fields` lists the format table\n", row, col);
            return;
        }
        if (static_cast<int>(text.size()) > f->length)
            fmt::print("console: '{}' is longer than the {}-byte field at {},{} and will be truncated\n", text,
                       f->length, f->row, f->col);
        fmt::print("console: typed '{}' at {},{} (field {},{} length {}) (send a key to submit)\n", text, row, col,
                   f->row, f->col, f->length);
    } else if (sub == "send") {
        if (a.size() < 3) { fmt::print("usage: console send <Enter|PF1..PF24|Clear|Help|RollUp|RollDown>\n"); return; }
        uint8_t aid;
        if (!host::ConsoleDisplay::tryParseAid(a[2], aid)) {
            fmt::print("console: unknown key '{}'\n", a[2]);
            return;
        }
        std::vector<std::string> typed;
        for (const host::ConsoleField& g : console.inputFields())
            if (g.hasPending) typed.push_back(fmt::format("{},{}='{}'", g.row, g.col, g.pending));
        backend->injectConsoleInput(aid);
        std::string list;
        for (std::size_t i = 0; i < typed.size(); ++i) list += (i ? " " : "") + typed[i];
        fmt::print("console: sent {} with {}\n", upper(a[2]), typed.empty() ? std::string("no typed field") : list);
    } else if (sub == "fields") {
        const std::vector<host::ConsoleField> table = console.inputFields();
        if (table.empty()) { fmt::print("console: no format table\n"); return; }
        int at = 0;
        for (const host::ConsoleField& g : table) {
            fmt::print("console: field {:2},{:<2} length {:3} offset {:3}{}{}{}\n", g.row, g.col, g.length, at,
                       g.bypass() ? " bypass" : "", g.nonDisplay() ? " nondisplay" : "",
                       g.hasPending ? " typed '" + g.pending + "'" : "");
            at += g.length;
        }
    } else {
        fmt::print("console [show|clear|fields|put <row> <col> <text>|send <key>]\n");
    }
}

host::WorkstationBackend* MonitorCli::findConsoleBackend()
{
    for (auto& st : m_.stations())
        if (st->backend().isConsoleAttachment()) return &st->backend();
    return nullptr;
}

void MonitorCli::workstationWrite(const std::vector<std::string>& a)
{
    if (a.size() < 3) {
        fmt::print("wswrite <id> demo|query|@file|<hex>\n");
        return;
    }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;

    std::vector<uint8_t> stream;
    if (!buildDataStream(a, 2, stream)) return;
    if (stream.size() > 0xFFFF) { fmt::print("data stream too long\n"); return; }

    // The work station command is at +0x0B, NOT +0x0A where the disk's is -
    // the two are mirror images, and +0x0A is a class byte the CONTROLLER
    // writes back.
    layWorkstationIob((station->port() << 4) | station->address(), stream);

    const long long before = station->backend().recordsSent();
    if (issueDeviceSvc(0x43, kWorkstationIoBlockAddress) < 0) return;

    fmt::print("station {}: {} byte(s) of data stream, {}\n", station->id(), stream.size(),
               station->backend().recordsSent() > before
                   ? std::string("on the wire")
                   : "not sent - " + std::string(station->backend().attached() ? "the session is still negotiating"
                                                                                 : "no session attached"));
    fmt::print("{}", hexDump(stream.data(), std::min<int>(64, static_cast<int>(stream.size())), kWorkstationStreamBuffer));
}

// LABELED-EXPERIMENTAL: render the guest's own on-disk compiled $SFGR SIGN
// ON format onto a work station, bypassing only the host-opaque
// message-session TRIGGER, never the faithful default IPL path.  It reads
// the format member off the mounted volume through the REAL disk path (SVC
// 40, exactly as `diskread`), translates the format's own SBA-ordered
// records into a 5250 Write-To-Display, and pushes it through the proven
// work station pipe (SVC 43, exactly as `wswrite demo`).
void MonitorCli::workstationFormat(const std::vector<std::string>& a)
{
    if (a.size() < 2) {
        fmt::print("wsformat <id> [sector] [count] [bodyoff-hex]  (EXPERIMENTAL: render on-disk $SFGR #$CPIPL; defaults 80800 4 C0)\n");
        return;
    }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;

    const int sector = a.size() > 2 ? decimal(a[2]) : 80800;
    const int count = a.size() > 3 ? decimal(a[3]) : 4;
    const int bodyOff = a.size() > 4 ? hex32(a[4]) : 0xC0;
    // Optional trailing token `fields` opts in to a best-effort input-field
    // reconstruction from the compiled field-control table.  It is OFF by
    // default because a faithful field render needs the $SFGR-time field
    // expansion that this harness does not perform.
    bool withFields = false;
    for (std::size_t k = 2; k < a.size(); k++)
        if (equalsIgnoreCase(a[k], "fields")) withFields = true;

    std::vector<uint8_t> fmtBytes;
    if (!readFormatSectors(sector, count, fmtBytes)) return;
    if (bodyOff < 0 || bodyOff >= static_cast<int>(fmtBytes.size())) { fmt::print("body offset out of range\n"); return; }

    const std::vector<uint8_t> stream = buildSignOnStream(fmtBytes, bodyOff, withFields);
    fmt::print("EXPERIMENTAL wsformat: on-disk $SFGR format sector {} ({} sectors), body +0x{:X}; built {}-byte 5250 "
               "Write-To-Display (labels VERBATIM{})\n", sector, count, bodyOff, stream.size(),
               withFields ? "; input fields RECONSTRUCTED (best-effort, unfaithful)" : "");
    if (stream.size() > 0xFFFF) { fmt::print("data stream too long\n"); return; }

    layWorkstationIob((station->port() << 4) | station->address(), stream);

    const long long before = station->backend().recordsSent();
    if (issueDeviceSvc(0x43, kWorkstationIoBlockAddress) < 0) return;
    fmt::print("station {}: {} byte(s) of data stream, {}\n", station->id(), stream.size(),
               station->backend().recordsSent() > before
                   ? std::string("on the wire")
                   : "not sent - " + std::string(station->backend().attached() ? "the session is still negotiating"
                                                                                 : "no session attached"));
    fmt::print("{}", hexDump(stream.data(), std::min<int>(96, static_cast<int>(stream.size())), kWorkstationStreamBuffer));
}

// Read `count` sectors from `sector` through the real disk model (SVC 40,
// the same IOB as `diskread`) and return the raw bytes.  Requires `boot`
// (needs a current request block).
bool MonitorCli::readFormatSectors(int sector, int count, std::vector<uint8_t>& out)
{
    constexpr int iob = 0x0600;
    auto& st = m_.state;
    for (int i = 0; i < 48; i++) st.writeByte(iob + i, 0);
    st.writeByte(iob + devices::IoBlock::kOffCommand, devices::VirtualFixedDisk::kCommandRead);
    st.writeAddr24(iob + devices::IoBlock::kOffDiskSector, sector + 1);   // field is 1-based
    st.writeAddr24(iob + devices::IoBlock::kOffDiskCount, count - 1);      // field is 0-based
    st.writeAddr24(iob + devices::IoBlock::kOffDataBuffer, kDiskReadBuffer);
    if (issueDeviceSvc(0x40, iob) < 0) return false;
    out.resize(static_cast<std::size_t>(count) * 256);
    st.read(kDiskReadBuffer, out.data(), static_cast<int>(out.size()));
    return true;
}

void MonitorCli::workstationInvite(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("wsinvite <id>\n"); return; }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;
    fmt::print("station {}: invite {}\n", station->id(), station->invite() ? "sent" : "dropped");
}

void MonitorCli::workstationInput(const std::vector<std::string>& a)
{
    if (a.size() < 3) {
        fmt::print("wsinput <id> <opcode-hex> [data-hex]\n");
        return;
    }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;
    const auto opcode = static_cast<host::WorkstationOpcode>(hexByte(a[2]));
    std::vector<uint8_t> data;
    if (a.size() > 3 && !parseHexBytes(a, 3, data)) return;
    station->backend().injectInput(host::WorkstationRecord(opcode, host::WorkstationRecordFlags::None, data));
    fmt::print("station {}: injected {} with {} byte(s); {} waiting\n", station->id(), host::opcodeName(opcode),
               data.size(), station->pendingInput());
}

void MonitorCli::workstationRead(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("wsread <id>\n"); return; }
    VirtualWorkstation* station = findStation(a[1]);
    if (station == nullptr) return;
    std::vector<uint8_t> stream;
    if (!station->tryTakeInput(stream)) { fmt::print("station {}: nothing waiting\n", station->id()); return; }
    fmt::print("station {}: {} byte(s) of input data stream\n", station->id(), stream.size());
    if (!stream.empty()) fmt::print("{}", hexDump(stream.data(), static_cast<int>(stream.size()), 0));
}

bool MonitorCli::buildDataStream(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out)
{
    const std::string first = toLower(a[from]);
    if (first == "demo") { out = demonstrationScreen(); return true; }
    if (first == "query") { out = queryCommand(); return true; }
    if (!first.empty() && first[0] == '@') {
        const std::string path = a[from].substr(1);
        if (!readFile(path, out)) { fmt::print("no such file: {}\n", path); return false; }
        return true;
    }
    return parseHexBytes(a, from, out);
}


// ---- the transfer and sign-on producers ---------------------------------

void MonitorCli::wsPresent(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("wspresent <task-addr> [unit-block-addr]\n"); return; }
    auto& csp = m_.nativeControlStorage();
    const int tb = hex32(a[1]);
    const int ublk = a.size() > 2 ? hex32(a[2]) : 0;
    const bool ok = csp.postDevicePresent(tb, ublk, "wspresent");
    fmt::print("{}\n", ok ? "posted; run/step to see if it wakes" : "post failed");
}

// EXPERIMENTAL per-station present, by station id.  Resolves the station's
// OWN terminal unit block from the guest's TUB+12 unit field and delivers a
// device-present tagged with it: the same thing the ws_interactive idle
// driver does, but callable from a script with no live socket.
void MonitorCli::wsPresentStation(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("wspresentst <station-id, e.g. 0.1>\n"); return; }
    auto& csp = m_.nativeControlStorage();
    VirtualWorkstation* s = stationById(a[1], false);
    if (s == nullptr) { fmt::print("no station {}\n", a[1]); return; }
    const int unit = (s->port() << 4) | s->address();
    const int tub = s->tubAddress != 0 ? s->tubAddress : csp.resolveTubByUnit(unit);
    if (tub == 0) {
        fmt::print("station {}: no #SVTUB terminal unit block for unit {:02X} (phase 2 has not built it - boot/run first)\n",
                   s->id(), unit);
        return;
    }
    s->tubAddress = tub;
    const bool ok = csp.raiseDeviceAttention(tub, GuestLowStorage::kTaskBlock, "station " + s->id() + " present (wspresentst)");
    if (ok)
        fmt::print("station {}: presented - own TUB {:06X} (unit {:02X}) to router 0F00; start/run to see routing\n",
                   s->id(), tub, unit);
    else
        fmt::print("post failed\n");
}

// Model the host command boundary, rather than making terminal connect
// synonymous with machine IPL.  The IPL has already brought the machine to
// its idle wait; the transfer lends one attached host display to the
// configured SSP work station and starts that station's sign-on session.
void MonitorCli::tfrM36(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() == 2 && equalsIgnoreCase(a[1], "state")) {
        fmt::print("TFRM36 transfer: {}; queued workstation-controller work: {}\n", csp.m36WorkStationTransferState(),
                   pendingWsEntryUnits_.size());
        return;
    }
    const std::string stationId = a.size() > 1 ? a[1] : "0.0";
    const std::string mode = a.size() > 2 ? toLower(a[2]) : "yes";
    const std::string phase = a.size() > 3 ? toLower(a[3]) : "all";
    bool autoSignOn;
    if (mode == "yes" || mode == "*yes" || mode == "auto") autoSignOn = true;
    else if (mode == "no" || mode == "*no" || mode == "prompt") autoSignOn = false;
    else {
        fmt::print("tfrm36 [station-id] [yes|no] [all|prepare|present]\n");
        return;
    }
    if (phase != "all" && phase != "prepare" && phase != "present") {
        fmt::print("tfrm36 [station-id] [yes|no] [all|prepare|present]\n");
        return;
    }

    VirtualWorkstation* station = stationById(stationId, false);
    if (station == nullptr || station->isPrinter()) {
        fmt::print("no display station {}\n", stationId);
        return;
    }
    if (phase != "present" && (!m_.msp().stopped() || !csp.idleEventWait())) {
        fmt::print("M36 is not at its STRM36 idle/wait state; run IPL to idle first\n");
        return;
    }
    if (!station->attached()) {
        fmt::print("station {} has no attached 5250 session; TFRM36 has nothing to lend\n", station->id());
        return;
    }

    const int unit = (station->port() << 4) | station->address();
    const int tub = csp.resolveConfiguredTubByUnit(unit);
    if (tub == 0) {
        fmt::print("station {}: no configured #SVTUB TU/OC pair for unit {:02X}; STRM36 phase 2 has not completed\n",
                   station->id(), unit);
        return;
    }
    bool bindPending = false;
    if (phase == "present") {
        if (!csp.isM36WorkStationTransferReady(tub, autoSignOn)) {
            fmt::print("TFRM36 station {}: present refused; transfer is {}. Run `tfrm36 {} {} prepare`, and drain pending "
                       "wsentry work first\n", station->id(), csp.m36WorkStationTransferState(), station->id(),
                       autoSignOn ? "yes" : "no");
            return;
        }
    } else if (!csp.beginM36WorkStationTransfer(tub, autoSignOn, !pendingWsEntryUnits_.empty(), "tfrm36 " + station->id(),
                                                bindPending)) {
        fmt::print("station {}: transfer setup failed\n", station->id());
        return;
    }
    if (bindPending) {
        fmt::print("TFRM36 station {}: bind pended behind {} queued workstation-controller action(s); use `wsentry drain "
                   "scan`, then `tfrm36 {} {} present`\n", station->id(), pendingWsEntryUnits_.size(), station->id(),
                   autoSignOn ? "yes" : "no");
        return;
    }

    // The socket can have latched its initial connect while the machine was
    // still IPLing.  The transfer is now the authoritative presentation of
    // that same display, so consume the older edge before posting the
    // transfer event.  Leaving it armed makes the idle driver present the
    // station a second time after the first transfer has parked.
    station->takeAttentionPending();

    station->tubAddress = tub;
    WorkStationSlot* nativeSlot = m_.devices().workStations().find(unit);
    if (nativeSlot != nullptr) nativeSlot->bindTransferRenderer();
    if (phase == "prepare") {
        fmt::print("TFRM36 station {}: action-0 bind complete for configured TU {:06X}, AUTOSIGNON(*{}); no guest-present "
                   "event posted\n", station->id(), tub, autoSignOn ? "YES" : "NO");
        return;
    }

    // After the action-0 bind, make the configured station present.
    // AUTOSIGNON(*YES) additionally supplies the inbound request/statement
    // which the host transfer worker originates.  The guest remains
    // responsible for building the JCB and display request.
    bool ok = csp.raiseDeviceAttention(tub, GuestLowStorage::kTaskBlock, "tfrm36 " + station->id() + " action-0 bind complete");
    if (autoSignOn) {
        // The reference also raises its class map-gate experiment switch
        // here; SIM/36 does not carry that experiment.
        ok = csp.postConsoleSignOnRequest(tub, "tfrm36 " + station->id() + " request") && ok;
        ok = csp.postConsoleSignOnStatement(tub, "tfrm36 " + station->id() + " statement") && ok;
    }
    if (ok)
        fmt::print("TFRM36 station {}: configured TU {:06X}, AUTOSIGNON(*{}), phase {}; run to execute the transferred session\n",
                   station->id(), tub, autoSignOn ? "YES" : "NO", phase);
    else
        fmt::print("TFRM36 station {}: transfer events could not be queued\n", station->id());
}

// Stage the controller-completion contract independently of the
// transfer-present edge.  `both` is the faithful response post: TU+8E
// first, then the dispatcher.  `byte` and `interrupt` exist to establish
// ordering in monitor experiments, not as runtime configuration.
void MonitorCli::wsAid(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() > 1 && equalsIgnoreCase(a[1], "cnfws")) {
        std::string setting = a.size() > 2 ? toLower(a[2]) : "";
        if (setting == "on") setting = "exact";
        if (a.size() != 3 || (setting != "exact" && setting != "configured" && setting != "deferred" && setting != "off")) {
            fmt::print("wsaid cnfws <exact|configured|deferred|off>\n");
            return;
        }
        csp.cnfwsPowerOnAidExperiment = setting != "off";
        csp.deferCnfwsPowerOnAid = setting == "deferred";
        csp.cnfwsPowerOnAidConfiguredTargetExperiment = setting == "configured";
        fmt::print("WSAID cnfws success-tail experiment {}\n", upper(setting));
        return;
    }
    const std::string stationId = a.size() > 1 ? a[1] : "0.0";
    const std::string operation = a.size() > 2 ? toLower(a[2]) : "both";
    const bool storeByte = operation == "both" || operation == "byte";
    const bool postInterrupt = operation == "both" || operation == "interrupt";
    if (!storeByte && !postInterrupt) {
        fmt::print("wsaid [station-id] [both|byte|interrupt] [function]\n");
        return;
    }
    uint8_t function = 0xF7;
    if (a.size() > 3 && !tryHexByte(a[3], function)) {
        fmt::print("wsaid: function must be a hexadecimal byte (default F7)\n");
        return;
    }
    VirtualWorkstation* station = stationById(stationId, false);
    if (station == nullptr || station->isPrinter()) {
        fmt::print("no display station {}\n", stationId);
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    const int tub = csp.resolveConfiguredTubByUnit(unit);
    if (tub == 0) {
        fmt::print("station {}: no configured #SVTUB TU/OC pair for unit {:02X}\n", station->id(), unit);
        return;
    }
    const bool ok = csp.deliverWorkStationControllerFunction(tub, function, storeByte, postInterrupt,
                                                             "wsaid " + station->id() + " " + operation);
    if (ok)
        fmt::print("WSAID station {}: TU {:06X}, operation {}, function {:02X}\n", station->id(), tub, operation, function);
    else
        fmt::print("WSAID station {}: delivery failed\n", station->id());
}

void MonitorCli::wsOc(const std::vector<std::string>& a)
{
    if (a.size() != 3 || (toLower(a[2]) != "active" && toLower(a[2]) != "inactive")) {
        fmt::print("wsoc <station-id> <active|inactive>\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    VirtualWorkstation* station = stationById(a[1], false);
    if (station == nullptr || station->isPrinter()) {
        fmt::print("no display station {}\n", a[1]);
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    const int tub = csp.resolveConfiguredTubByUnit(unit);
    const bool active = equalsIgnoreCase(a[2], "active");
    const bool ok = tub != 0 && csp.setWorkStationOcActive(tub, active, "wsoc " + station->id());
    if (ok) fmt::print("WSOC station {}: TU {:06X} {}\n", station->id(), tub, active ? "ACTIVE" : "INACTIVE");
    else fmt::print("WSOC station {}: no configured TU/OC\n", station->id());
}

// EXPERIMENTAL, LABELED, OFF THE FAITHFUL PATH.  Stand in for the host
// server passing an SSP sign-on identity through the work-station session
// handshake by writing an 8-byte EBCDIC user name into a station's terminal
// unit block at TUB+0x66, the offset a signed-on console TUB carries it on
// real hardware.  A configuration key was not used on purpose: no reached
// instruction consumes the identity, so it belongs in the monitor harness.
void MonitorCli::wsUser(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("wsuser <station-id, e.g. 0.1> <name>  (EXPERIMENTAL)\n"); return; }
    auto& csp = m_.nativeControlStorage();
    VirtualWorkstation* s = stationById(a[1], false);
    if (s == nullptr) { fmt::print("no station {}\n", a[1]); return; }
    const std::string& name = a[2];
    const int unit = (s->port() << 4) | s->address();
    const int tub = s->tubAddress != 0 ? s->tubAddress : csp.resolveTubByUnit(unit);
    if (tub == 0) {
        fmt::print("station {}: no #SVTUB terminal unit block yet (boot/run first)\n", s->id());
        return;
    }
    s->tubAddress = tub;
    std::string padded = name.size() > 8 ? name.substr(0, 8) : name;
    while (padded.size() < 8) padded += ' ';
    const std::vector<uint8_t> eb = Ebcdic::fromAscii(padded);
    for (int i = 0; i < 8; i++) m_.state.writeByte(tub + 0x66 + i, eb[i]);
    std::string trimmed = name;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    fmt::print("[exp] station {}: wrote user '{}' into TUB {:06X}+0x66 (8 bytes EBCDIC)\n", s->id(), trimmed, tub);
}

// TEST: post a typed completion event to a task block by ADDRESS.  Used to
// probe the "data-waiting" notification the work-station I/O task waits on
// after a Put, before it collects input via SVC 43.
void MonitorCli::wsPost(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("wspost <task-block-hex> <event-type-hex> [unit-block-hex]\n"); return; }
    auto& csp = m_.nativeControlStorage();
    const int tb = hex32(a[1]);
    const auto ev = static_cast<uint16_t>(hex32(a[2]));
    const int ublk = a.size() > 3 ? hex32(a[3]) : 0;
    const bool ok = csp.postTypedEventToTask(tb, ev, ublk, "wspost");
    fmt::print("{}\n", ok ? "posted; run/step to see" : "post failed");
}

void MonitorCli::wsPresentWs(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("wspresentws <unit-block-addr>\n"); return; }
    auto& csp = m_.nativeControlStorage();
    const int ublk = hex32(a[1]);
    const bool ok = csp.postWorkStationPresent(ublk, "wspresentws");
    fmt::print("{}\n", ok ? "posted work-station-present to #CPTC; run/step to see" : "post failed");
}

// Sweep the command router's routing key: post a multiple-wait element to
// task <id> with routing key <key> in ace+29, to find which key reaches the
// sign-on module.
void MonitorCli::postRk(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("postrk <task-id-hex> <routing-key-hex> [unit-block-hex]\n"); return; }
    auto& csp = m_.nativeControlStorage();
    const int tid = hex32(a[1]);
    const int key = hex32(a[2]);
    const int ublk = a.size() > 3 ? hex32(a[3]) : 0;
    const bool ok = csp.postRouterElement(tid, key, ublk, "postrk");
    fmt::print("{}\n", ok ? "posted router element; run/step to see routing" : "post failed");
}

// Reproduce the native SSP call scheduler rather than reducing it to its
// 0x1C routing key.  The optional request address is the producer's guest
// request block copied to CF+24..26; it is not a task ID.
void MonitorCli::callSsp(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() > 1 && (equalsIgnoreCase(a[1], "syslog") || equalsIgnoreCase(a[1], "console"))) {
        // Defaults are one grounded call site, not a work-station guess:
        // message 8607, line-CB +410/+408 state 00/03, and tags 8B/40.
        const auto messageId = static_cast<uint16_t>(a.size() > 2 ? hex32(a[2]) : 0x8607);
        const auto line410 = static_cast<uint8_t>(a.size() > 3 ? hex32(a[3]) : 0);
        const auto line408 = static_cast<uint8_t>(a.size() > 4 ? hex32(a[4]) : 3);
        const auto tag0 = static_cast<uint8_t>(a.size() > 5 ? hex32(a[5]) : 0x8B);
        const auto tag1 = static_cast<uint8_t>(a.size() > 6 ? hex32(a[6]) : 0x40);
        int request = 0, consoleCf = 0;
        const bool consoleOk = csp.postNativeS36ConsoleCall(messageId, line410, line408, tag0, tag1, "callssp syslog",
                                                            request, consoleCf);
        if (consoleOk)
            fmt::print("posted NuS36 DLC system-log request {:06X} as CF {:06X}; run/step to trace #CPSI -> #MASF -> #CLSG\n",
                       request, consoleCf);
        else
            fmt::print("native S/36 system-log call post failed\n");
        return;
    }
    const std::string program = a.size() > 1 ? upper(a[1]) : "#CLSG";
    const int requestAddress = a.size() > 2 ? hex32(a[2]) : 0;
    int cf = 0;
    const bool ok = csp.postNativeSspCall(program, requestAddress, "callssp", cf);
    if (ok) fmt::print("posted native SSP call '{}' as CF {:06X}; run/step to trace #MSSC -> #CPSC\n", program, cf);
    else fmt::print("native SSP call post failed\n");
}

void MonitorCli::signOnStatement(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const int tub = a.size() > 1 ? hex32(a[1]) : 0;
    const bool ok = csp.postConsoleSignOnStatement(tub, "signonstmt");
    fmt::print("{}\n", ok ? "posted sign-on statement; run to see #CPSC -> #CPSI -> #CPRT" : "post failed");
}

void MonitorCli::signOnCommand(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const int tub = a.size() > 1 ? hex32(a[1]) : 0;
    const bool ok = csp.postConsoleCommandStatement(tub, "signoncmd");
    fmt::print("{}\n", ok ? "posted console-command statement (code 0x1B); run to see #CPSC -> #CPSI -> #CCJS (the 0x1504 arm segment)"
                          : "post failed");
}

void MonitorCli::signOnRequest(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const int tub = a.size() > 1 ? hex32(a[1]) : 0;
    const bool ok = csp.postConsoleSignOnRequest(tub, "signonreq");
    fmt::print("{}\n", ok ? "COUNTERFACTUAL short-route state stamped; signonstmt then run to see #CPRT -> #CPTS" : "stamp failed");
}

void MonitorCli::wsAttach(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const int tub = a.size() > 1 ? hex32(a[1]) : 0;
    const bool ok = csp.postConsoleDeviceAttach(tub, "wsattach");
    fmt::print("{}\n", ok ? "modelled console device attach (TUB+0x79 bit 0x20); run to see #SVAT -> #ICDB"
                          : "attach model failed - no published console TUB");
}

// Inject an inbound console message to the command router's dispatch loop.
void MonitorCli::msscMsg(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() < 3) {
        fmt::print("usage: msscmsg <taskblock-hex> <type-hex> [p1-hex] [p2-hex]  (#MSSC types: 14/15/18/19/1A/101; get the task from `whereis`)\n");
        return;
    }
    const int tb = hex32(a[1]);
    const int type = hex32(a[2]);
    const int p1 = a.size() > 3 ? hex32(a[3]) : 0;
    const int p2 = a.size() > 4 ? hex32(a[4]) : 0;
    const bool ok = csp.postConsoleMessage(tb, type, p1, p2, "msscmsg");
    if (ok)
        fmt::print("injected console message type {:X} (p1 {:X}, p2 {:X}) to task {:04X}; run to see #MSSC dispatch\n", type, p1, p2, tb);
    else
        fmt::print("message not injected - not a task block, or ACE pool exhausted\n");
}

void MonitorCli::conDbElem(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const int tub = a.size() > 1 ? hex32(a[1]) : 0;
    const bool ok = csp.postConsoleDbElement(tub, "condbelem");
    fmt::print("{}\n", ok ? "EXPERIMENTAL: appended a synthetic console-DB element to queue 50; run to see #ICDB@0x1047 find it"
                          : "console-DB element not spliced - see trace (no console TUB, or the +0x4B key/chain cycle; docs/s36/console-db-grounding.md)");
}

// The reference's sign-on remainder experiments.  The switches that map,
// stamp or fabricate guest state are not carried by SIM/36: each is parsed
// so scripts run, says so, and changes nothing.  The producer sub-commands
// that post through the control processor's own producers (jcb, wake, oc,
// session, cptc2b) are carried.
void MonitorCli::signonExp(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    const std::string sub = a.size() > 1 ? toLower(a[1]) : "on";
    if (sub == "jcb") {
        const int tub = a.size() > 2 ? hex32(a[2]) : 0;
        const bool ok = csp.synthesizeJcb(tub, "signonexp jcb");
        fmt::print("{}\n", ok ? "EXPERIMENTAL - synthesized #CPON-style JCB and linked it at the console TUB (UNFAITHFUL; run `signonexp on` then start/run to drive #CLSS -> class-init chain)"
                              : "JCB synthesis failed - no published console TUB, or heap exhausted (see trace)");
        return;
    }
    if (sub == "wake") {
        const int tid = a.size() > 2 ? hex32(a[2]) : 0x0003;
        const bool okw = csp.wakeJobTaskSelfEvent(tid, "signonexp wake");
        fmt::print("{}\n", okw ? "EXPERIMENTAL - woke job task on its own saved-XR1 event; run to see the #CI* console-init chain advance (UNFAITHFUL poster; no $SFGR - re-parks)"
                               : "job-task wake not posted - task not in an event wait, or not found (see trace)");
        return;
    }
    if (sub == "oc") {
        const int key = a.size() > 2 ? hex32(a[2]) : 0x0002;
        const int oc = csp.installMessageSessionOc(key, "signonexp oc");
        if (oc != 0)
            fmt::print("EXPERIMENTAL - installed OC block at {:06X} keyed {:04X} on QH53 (#CPSC SVC 1B will FIND it; FOUND branch tears down w/o $SFGR)\n", oc, key);
        else
            fmt::print("OC install failed (heap)\n");
        return;
    }
    if (sub == "session") {
        csp.installMessageSessionOc(0x3002, "signonexp session/oc");
        csp.installMessageSessionOc(0x0002, "signonexp session/oc");
        const bool okw = csp.wakeJobTaskSelfEvent(0x0003, "signonexp session/wake");
        fmt::print("{}\n", okw ? "EXPERIMENTAL - installed OC (0x3002+0x0002) on QH53 and posted the 8B70 CQ wake; run to drive the #CI* chain -> #CPSC FOUND branch (0x196D). No $SFGR: the FOUND branch tears down the session (PB-state content gap)."
                               : "session drive: 8B70 not in an event wait - reach the message-session park first");
        return;
    }
    if (sub == "cptc2b") {
        if (a.size() < 3) {
            fmt::print("usage: signonexp cptc2b <task-block-hex> [station-id]\n");
            return;
        }
        const int tb2 = hex32(a[2]);
        const std::string stationId = a.size() > 3 ? a[3] : "console";
        VirtualWorkstation* station = findStation(stationId);
        if (station == nullptr) return;
        const int unit = (station->port() << 4) | station->address();
        const int ub2 = station->tubAddress != 0 ? station->tubAddress : csp.resolveTubByUnit(unit);
        if (ub2 == 0) {
            fmt::print("0x2B post failed - station {} has no guest-built TUB yet\n", station->id());
            return;
        }
        station->tubAddress = ub2;
        const bool ok2 = csp.postTypedEventToTask(tb2, 0x002B, ub2, "signonexp cptc2b");
        if (ok2)
            fmt::print("EXPERIMENTAL - posted 0x2B typed event to #CPTC task with station {} TUB {:06X}; run to see\n", station->id(), ub2);
        else
            fmt::print("0x2B post failed (task not in event wait / not a task block)\n");
        return;
    }
    // Every remaining switch (content, wddqwrite, display, mapgate, jcb-arm,
    // copymap, off, automap, groundmap, on) fabricates guest state through a
    // control-processor experiment SIM/36 does not carry.
    fmt::print("signonexp {}: this experiment is not carried by SIM/36; nothing changed\n", sub);
}

// ---- the controller configuration and the raw device call ---------------

void MonitorCli::wsConfig(const std::vector<std::string>& a)
{
    if (a.size() > 1) {
        setVolumeWorkStationConfiguration(a);
        return;
    }
    WorkStationController& ctl = m_.devices().workStations();
    fmt::print("work station controller: {} slot(s), max {}\n", ctl.count(), WorkStationController::kMaxDevices);
    for (WorkStationSlot* slot : ctl.slots()) {
        const std::vector<uint8_t> rec = slot->configurationRecord();
        std::string hex;
        for (uint8_t b : rec) hex += fmt::format("{:02X} ", b);
        while (!hex.empty() && hex.back() == ' ') hex.pop_back();
        fmt::print("  {:<40} record {}\n", slot->toString(), hex);
    }

    fmt::print("\n");
    fmt::print("unit definition table, sectors {}-{} - work station entries:\n", As36ControlStorageProcessor::kUdtSector,
               As36ControlStorageProcessor::kUdtSector + 1);
    std::vector<uint8_t> rc(2 * DiskBackend::kSectorBytes);
    m_.diskBackend().readSector(As36ControlStorageProcessor::kUdtSector, rc.data());
    m_.diskBackend().readSector(As36ControlStorageProcessor::kUdtSector + 1, rc.data() + DiskBackend::kSectorBytes);
    std::size_t off = 0;
    while (off + 11 <= rc.size() && rc[off] != 0) {
        const int l1 = rc[off + 8], l2 = rc[off + 9], l3 = rc[off + 10];
        const std::size_t size = 11 + l1 + l2 + l3;
        if (size <= 11 || off + size > rc.size()) break;
        // Byte 1 is the device class; C0 is the work station family, and
        // byte 4 is the unit address the IOB will name.
        if (rc[off + 1] == 0xC0) {
            std::string name = Ebcdic::toAscii(rc.data() + off + 11 + l1 + l2, static_cast<std::size_t>(l3));
            for (char& c : name) if (c == '\0') c = ' ';
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            fmt::print("  id {:02X} class {:02X} unit {:02X} (port {} address {})  {}\n", rc[off], rc[off + 1], rc[off + 4],
                       WorkStationController::portOf(rc[off + 4]), WorkStationController::addressOf(rc[off + 4]), name);
        }
        off += size;
    }
}

// Change one of the seven 64-byte SSP work-station configuration records
// beginning at volume sector 8234 byte 0x40.  This is configuration input
// consumed by the IPL's unit-block builder, not a fabricated runtime OC or
// session object: it selects cfg+11..12 as its queue-53 key when cfg+4 is
// 0x80 and builds the OC itself.  The operation is deliberately an explicit
// monitor command and requires a writable volume.
void MonitorCli::setVolumeWorkStationConfiguration(const std::vector<std::string>& a)
{
    if (a.size() < 4 || !equalsIgnoreCase(a[1], "set")) {
        fmt::print("usage: wsconfig set W1..W7 message-session [key-hex] | physical\n");
        return;
    }
    const std::string name = upper(a[2]);
    int number = 0;
    if (name.size() != 2 || name[0] != 'W' || !tryDigits(name.substr(1), number) || number < 1 || number > 7) {
        fmt::print("station record must be W1 through W7\n");
        return;
    }
    const bool message = equalsIgnoreCase(a[3], "message-session");
    const bool physical = equalsIgnoreCase(a[3], "physical");
    if (!message && !physical) {
        fmt::print("record type must be message-session or physical\n");
        return;
    }
    const int key = message && a.size() > 4 ? hex32(a[4]) : message ? 0x0002 : 0;
    if (key < 0 || key > 0xFFFF) throw MonitorError("key must fit in 16 bits (Parameter 'key')");

    constexpr int tableSector = 8234;
    constexpr int firstRecordOffset = 0x40;
    constexpr int recordBytes = 64;
    const int absolute = firstRecordOffset + (number - 1) * recordBytes;
    const int sector = tableSector + absolute / DiskBackend::kSectorBytes;
    const int offset = absolute % DiskBackend::kSectorBytes;
    std::vector<uint8_t> data(DiskBackend::kSectorBytes);
    m_.diskBackend().readSector(sector, data.data());

    // Refuse to edit an unexpected object.  +1..2 is EBCDIC Wn and +3 is
    // the terminal discriminator; cfg+4 is the independent OC selector.
    const auto ebcdicDigit = static_cast<uint8_t>(0xF0 + number);
    if (data[offset + 1] != 0xE6 || data[offset + 2] != ebcdicDigit || data[offset + 3] != 0xC0)
        throw MonitorError(fmt::format("volume sector {} offset {:02X} is not the expected {} WSC record", sector, offset, name));

    data[offset + 4] = message ? static_cast<uint8_t>(0x80) : static_cast<uint8_t>(0xC0);
    data[offset + 11] = static_cast<uint8_t>(key >> 8);
    data[offset + 12] = static_cast<uint8_t>(key);
    m_.diskBackend().writeSector(sector, data.data());
    fmt::print("volume WSC {}: cfg+4={:02X}, alternate QH53 key={:04X}; issue `ipl` for #SVTUB to rebuild the guest OC chain\n",
               name, data[offset + 4], key);
}

// Build an IOB and issue SVC 42 or SVC 43 through the control processor.
// This is NOT evidence that the model is right: it is the model's only
// caller and it is ours, so agreement between them is a tautology.  It
// exists to make the path runnable and inspectable before real SSP reaches
// it.
void MonitorCli::wsIoch(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("wsioch 42|43 <cmd-hex> [length] [unit-hex]\n"); return; }
    const uint8_t r = hexByte(a[1]);
    const uint8_t cmd = hexByte(a[2]);
    const int length = a.size() > 3 ? decimal(a[3]) : 61;   // 10 records plus the FF
    // The unit address matters as much as the command: the controller
    // routes FF to the invite arm before the command byte is read at all,
    // so a harness that could only address unit 00 could not reach it.
    const uint8_t unit = a.size() > 4 ? hexByte(a[4]) : static_cast<uint8_t>(0x00);

    // Well clear of low storage, which the IPL fills with elements and IOBs.
    constexpr int iob = 0x20600, buffer = 0x20700;
    auto& st = m_.state;
    for (int i = 0; i < 64; i++) st.writeByte(iob + i, 0);

    // The class byte's high nibble must be C or the printer entry refuses
    // the request; the command is the byte after it.
    st.writeByte(iob + WorkStationIob::kOffClass, 0xC0);
    st.writeByte(iob + WorkStationIob::kOffCommand, cmd);
    st.writeByte(iob + WorkStationIob::kOffUnitAddress, unit);
    st.writeAddr24(iob + WorkStationIob::kOffDataBuffer, buffer);
    st.writeHalf(iob + WorkStationIob::kOffLength, static_cast<uint16_t>(length));

    // XR1 carries the IOB address, and it has to be set as a REGISTER: the
    // control processor spills the live MSP registers into the current
    // request block on the way in, so anything written straight into the
    // block is overwritten before a handler sees it.  The request block IS
    // the register save area, in both directions.
    st.msp.pactXr1 = static_cast<uint8_t>(iob >> 16);
    st.msp.xr1 = static_cast<uint16_t>(iob & 0xFFFF);

    processors::controlstorage::SvcRequest req;
    req.r = r;
    req.q = 0;
    req.dispatch = m_.controlStorage().classify(r);
    const bool ok = m_.controlStorage().svc(req);
    fmt::print("SVC {:02X} command {:02X} ({}) -> {}\n", r, cmd, WorkStationIob::commandName(cmd), ok ? "completed" : "rejected");
    fmt::print("  iob+06 completion {:02X}\n", st.readByte(iob + processors::controlstorage::Ecm::kOffCompletion));
    std::vector<uint8_t> got(static_cast<std::size_t>(std::min(length + 8, 64)));
    st.read(buffer, got.data(), static_cast<int>(got.size()));
    fmt::print("-- buffer at {:06X} --\n", buffer);
    fmt::print("{}", hexDump(got.data(), static_cast<int>(got.size()), buffer));
}

// ---- the display-queue module and the work-station task decoders -------

namespace {

const char* wddqVerb(uint16_t verb)
{
    if (verb == 0x0008) return "REGISTER";
    if (verb == 0x0009) return "WRITE";
    if ((verb & 0x00F0) == 0x00F0) return "extended/control dispatch";
    return "other/not-yet-stored";
}

}  // namespace

void MonitorCli::wddqState(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("usage: wddqstate\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;

    std::vector<As36ControlStorageProcessor::LoadedProgramBlock> programs;
    for (const auto& p : csp.getLoadedProgramBlocks())
        if (equalsIgnoreCase(p.name, "#WDDQ")) programs.push_back(p);
    std::stable_sort(programs.begin(), programs.end(),
                     [](const auto& x, const auto& y) { return x.programBlock < y.programBlock; });
    fmt::print("WDDQ-STATE: read-only request snapshot (REGISTER=0008, WRITE=0009)\n");
    fmt::print("  #WDDQ residency: {} loader-attributed program block(s)\n", programs.size());
    for (const auto& p : programs)
        fmt::print("    pb {:06X}: extent {}; logical {:06X}; physical {:06X}; ready {}\n", p.programBlock, p.extentSector,
                   p.logicalBase, p.physicalBase, p.ready ? "yes" : "no");

    std::set<int> wddqPbs;
    for (const auto& p : programs) wddqPbs.insert(p.programBlock);
    int matched = 0;
    int tb = st.readAddr24(GuestLowStorage::queueHeader(39));
    std::set<int> seenTasks;
    while (tb != 0 && seenTasks.size() < 4096 && seenTasks.insert(tb).second) {
        if (!TaskBlock::isTaskBlock(st, tb)) break;
        const std::vector<RequestFrame> frames = requestBlockChain(tb);
        std::vector<RequestFrame> wddqFrames;
        for (const auto& f : frames)
            if (wddqPbs.count(f.programBlock) != 0) wddqFrames.push_back(f);
        const int b = st.readAddr24(tb + TaskBlock::kOffWorkBase);
        const int savedRequest = canReadGuest(b, 0x24) ? st.readHalf(b + 0x13) : 0;
        const uint16_t bVerb = canReadGuest(b, 0x24) ? st.readHalf(b + 0x22) : static_cast<uint16_t>(0);
        const uint16_t savedEye = canReadGuest(savedRequest, 0x1C) ? st.readHalf(savedRequest) : static_cast<uint16_t>(0);
        const bool requestSignature = savedEye == 0xC3D7 || savedEye == 0xE6D7;

        // Once the module returns its RB frame is gone, but the REGISTER arm
        // deliberately leaves the CP request in the task work area.  The
        // CP/WP eyecatchers are the structural survivors; do not guess from
        // B+23 alone, since every task owns that offset.
        if (!wddqFrames.empty() || requestSignature) {
            matched++;
            const uint16_t id = st.readHalf(tb + TaskBlock::kOffTaskId);
            const uint8_t state = st.readByte(tb + TaskBlock::kOffState);
            fmt::print("  task {:04X}/{:06X}{}: state={:02X} stat2={:02X} JCB={:06X} B={:06X}\n", id, tb,
                       tb == csp.currentTaskBlock() ? " (current)" : "", state, st.readByte(tb + TaskBlock::kOffStat2),
                       st.readAddr24(tb + TaskBlock::kOffJobControlBlock), b);
            fmt::print("    B+13..14 (field ending +14) saved request={:04X}; B+22..23 (field ending +23) state={:04X} ({})\n",
                       savedRequest, bVerb, wddqVerb(bVerb));

            for (const auto& frame : wddqFrames) {
                const int entryRequest = RequestBlock::readXr2RealAddress(st, frame.requestBlock);
                const uint16_t wr4 = RequestBlock::readWr(st, frame.requestBlock, 4);
                fmt::print("    WDDQ frame[{}] rb={:06X} resume={:04X} saved-WR4={:04X} ({}); entry-XR2={:06X}; previous={:06X}\n",
                           frame.depth, frame.requestBlock, frame.resumeIar, wr4, wddqVerb(wr4), entryRequest, frame.previous);
                if (entryRequest != savedRequest) dumpWddqRequest("entry-XR2 request", entryRequest);
            }
            dumpWddqRequest("B-saved request", savedRequest);
            dumpWddqCompleteQueue(tb);
        }
        tb = st.readAddr24(tb + TaskBlock::kOffQueue39Link);
    }

    if (matched == 0) fmt::print("  no live #WDDQ frame or surviving CP/WP request on QH39\n");
    fmt::print("  system queue heads searched by #WDDQ: QH43={:06X}; QH52={:06X}\n",
               st.readAddr24(GuestLowStorage::queueHeader(0x43)), st.readAddr24(GuestLowStorage::queueHeader(0x52)));
}

void MonitorCli::dumpWddqRequest(const char* source, int request)
{
    auto& st = m_.state;
    if (!canReadGuest(request, 0x1C)) {
        fmt::print("      {}: {:06X} null/outside guest storage\n", source, request);
        return;
    }
    const uint16_t eye = st.readHalf(request);
    const int pointer0B = st.readHalf(request + 0x0A);
    const uint16_t selector1B = st.readHalf(request + 0x1A);
    const char* eyeName = eye == 0xC3D7 ? "CP" : eye == 0xE6D7 ? "WP" : "unknown";
    const char* pointerName = eye == 0xC3D7 ? "display-data" : eye == 0xE6D7 ? "workstation-TU" : "pointer";
    fmt::print("      {} {:06X}: eye={:04X} ({}) +02={:02X} +03={:02X} +04={:02X}; +0A..0B (field ending +0B) {}={:04X}{}\n", source,
               request, eye, eyeName, st.readByte(request + 2), st.readByte(request + 3), st.readByte(request + 4),
               pointerName, pointer0B, pointer0B == 0 ? " NULL" : "");
    if (eye == 0xE6D7) {
        fmt::print("        WP +1A..1B (field ending +1B) selector={:04X} ({})\n", selector1B, wddqVerb(selector1B));
        if (canReadGuest(pointer0B, 0xAA)) {
            const int pendingStatus = st.readAddr24(pointer0B + 0x45);
            const int cponSession = st.readAddr24(pointer0B + 0xA7);
            fmt::print("        TU +45..47 pending device-status={:06X}{}\n", pendingStatus, pendingStatus == 0 ? " NULL" : "");
            fmt::print("        TU +A7..A9 CPON per-job/session block={:06X}{}\n", cponSession, cponSession == 0 ? " NULL" : "");
        }
    }
}

void MonitorCli::dumpWddqCompleteQueue(int tb)
{
    auto& st = m_.state;
    const int head = st.readAddr24(tb + TaskBlock::kOffCompleteQueue);
    const int returnAce = st.readAddr24(tb + TaskBlock::kOffReturnAce);
    fmt::print("    completion: TB+45..47 head={:06X}; TB+17..19 return-ACE={:06X}\n", head, returnAce);
    int ace = head, count = 0;
    std::set<int> seen;
    while (ace != 0 && count < 64 && seen.insert(ace).second && canReadGuest(ace, ActionControlElement::kSize)) {
        fmt::print("      ACE[{}] {:06X}: next={:06X} event={:04X} flags={:02X} target-TB={:06X} XR1={:06X} XR2={:06X}\n", count, ace,
                   st.readAddr24(ace + ActionControlElement::kOffChainLink), st.readHalf(ace + ActionControlElement::kOffEventType),
                   st.readByte(ace + ActionControlElement::kOffFlags), st.readAddr24(ace + ActionControlElement::kOffTaskBlock),
                   st.readAddr24(ace + ActionControlElement::kOffXr1Copy), st.readAddr24(ace + ActionControlElement::kOffXr2));
        ace = st.readAddr24(ace + ActionControlElement::kOffChainLink);
        count++;
    }
    if (head == 0) fmt::print("      complete queue empty\n");
    else if (ace != 0) fmt::print("      complete queue walk stopped: invalid, cyclic, or >64 ACEs\n");
}

// Decode the small set of live structures that gate the work-station
// task's display-post path.  In the member's notation B is the task
// scratch base published in queue header 38 (and saved at tb+69), while C
// is the task block itself, published in queue header 37 while the task is
// dispatched.  Reading B from the requested task rather than the live
// queue-header cell makes the command useful for parked tasks as well as
// the current task.
void MonitorCli::cptcState(const std::vector<std::string>& a)
{
    if (a.size() < 2 || a.size() > 3 || (a.size() == 3 && !equalsIgnoreCase(a[2], "classify"))) {
        fmt::print("usage: cptcstate <task-id-or-address-hex> [classify]\n");
        return;
    }
    auto& st = m_.state;
    const int want = hex32(a[1]);
    std::vector<int> matches;
    int at = st.readAddr24(GuestLowStorage::queueHeader(39));
    for (int guard = 0; at != 0 && guard++ < 4096 && std::find(matches.begin(), matches.end(), at) == matches.end();) {
        if (!TaskBlock::isTaskBlock(st, at)) break;
        if (at == want || st.readHalf(at + TaskBlock::kOffTaskId) == want) matches.push_back(at);
        at = st.readAddr24(at + TaskBlock::kOffQueue39Link);
    }

    if (matches.empty()) {
        fmt::print("no task with id/address {:X}\n", want);
        return;
    }
    if (matches.size() > 1) {
        fmt::print("task id {:04X} is ambiguous ({}); use a task-block address\n", want, hexJoin(matches, "{:04X}"));
        return;
    }

    const int tb = matches[0];
    const int taskId = st.readHalf(tb + TaskBlock::kOffTaskId);
    const int b = st.readAddr24(tb + TaskBlock::kOffWorkBase);
    const int c = tb;
    const int chainHead = st.readAddr24(GuestLowStorage::queueHeader(50));

    fmt::print("#CPTC state for task {:04X} (id {:04X})\n", tb, taskId);
    if (taskId != 0x0109) fmt::print("  warning: task id is not #CPTC's expected 0109\n");
    fmt::print("  B work base       {:06X}  (tb+69; queue-header-38 value when dispatched)\n", b);
    fmt::print("  C task base       {:06X}  (task block; queue-header-37 value when dispatched)\n", c);
    fmt::print("  QH50 chain head   {:06X}  (guest 0BCB)\n", chainHead);

    if (!canReadGuest(b, 0x30)) {
        fmt::print("  B is null or does not cover #CPTC's 0x30-byte scratch area\n");
        return;
    }

    // The member's listing names multi-byte fields by their RIGHTMOST byte.
    // The storage readers take the LEFTMOST byte, hence 03 -> 01, 0C -> 0A,
    // and 11 -> 10 here.
    const int cursor = st.readAddr24(b + 0x01);
    const int displayBlock = st.readAddr24(b + 0x0A);
    const int request = st.readHalf(b + 0x10);
    const uint8_t latch = st.readByte(c + 0x55);
    const bool idleSkip = (request >> 8) == 0xA1 && (latch & 0x08) == 0;

    fmt::print("  chain cursor      {:06X}  (operand B+03; storage B+01..03)\n", cursor);
    fmt::print("  display block     {:06X}  (operand B+0C; storage B+0A..0C)\n", displayBlock);
    fmt::print("  request word      {:04X}    (operand B+11; storage B+10..11)\n", request);
    fmt::print("  display latch     {:02X}      (C+55; bit 08 is {})\n", latch, (latch & 0x08) != 0 ? "ARMED" : "clear");
    // Parked tasks resume at 16C7, after this guard.  This is the branch
    // the saved bytes would select when execution next reaches 1652; it is
    // not by itself proof that the current 0x2B wake has pending work.
    fmt::print("  guard if reached  {}\n", idleSkip ? "IDLE/SKIP (request high byte A1 and latch clear)"
                                                    : "BUILD/POST (non-idle request or latch armed)");

    int selected = 0, scanned = 0;
    std::string selectedDetail;
    std::set<int> seen;
    for (int node = chainHead; node != 0 && scanned < 4096 && seen.insert(node).second;) {
        std::string detail;
        scanned++;
        if (cptcNodeIsActive(node, detail)) {
            selected = node;
            selectedDetail = detail;
            break;
        }
        if (!canReadGuest(node, 0x50)) break;
        // The module's operand +4D names the rightmost byte; the physical
        // link is +4B..4D.
        node = st.readAddr24(node + 0x4B);
    }
    fmt::print("  selected node     {}\n", selected == 0 ? std::string("none") : fmt::format("{:06X}", selected));
    fmt::print("  active predicate  {}{}\n", selected != 0 ? "true" : "false",
               selected != 0 ? " (" + selectedDetail + ")" : fmt::format(" (scanned {} QH50 node(s); no active SU/TU)", scanned));

    if (a.size() == 3) cptcClassifierTrace(chainHead);
}

// Read-only rendering of the fields consumed by the classifier at
// 110C..12D2.  Addresses are derived from QH50; none is supplied by the
// operator and this routine deliberately has no write/watch side effect.
void MonitorCli::cptcClassifierTrace(int chainHead)
{
    auto& st = m_.state;
    fmt::print("  classifier 110C..12D2 (physical offsets; '--' means no readable target)\n");
    std::set<int> seen;
    int node = chainHead;
    for (int ordinal = 0; node != 0 && ordinal < 4096 && seen.insert(node).second; ordinal++) {
        if (!canReadGuest(node, 0x9B)) {
            fmt::print("    [{}] {:06X} unreadable\n", ordinal, node);
            break;
        }
        const uint16_t eye = st.readHalf(node);
        const int link = st.readAddr24(node + 0x4B);
        const int oc = st.readAddr24(node + 0x53);
        const int p62 = st.readAddr24(node + 0x60);
        const int p65 = st.readAddr24(node + 0x63);
        const uint16_t f99 = st.readHalf(node + 0x99);
        const std::string ocFields = canReadGuest(oc, 0x0F)
            ? fmt::format("OC={:06X}:0C={:02X},0E={:02X}", oc, st.readByte(oc + 0x0C), st.readByte(oc + 0x0E))
            : fmt::format("OC={:06X}:--", oc);
        const std::string p62Fields = canReadGuest(p62, 0x18)
            ? fmt::format("P62={:06X}:eye={:04X},P17={:06X}", p62, st.readHalf(p62), st.readAddr24(p62 + 0x15))
            : fmt::format("P62={:06X}:--", p62);
        const std::string p65Fields = canReadGuest(p65, 0x3D) ? fmt::format("P65={:06X}:3C={:02X}", p65, st.readByte(p65 + 0x3C))
                                                              : fmt::format("P65={:06X}:--", p65);

        fmt::print("    [{}] {:06X} eye={:04X} 09={:02X} 14={:02X} 2A={:02X} 2B={:02X} 2C={:02X} 4F={:04X} 78={:02X} 79={:02X} 7A={:02X} "
                   "7B={:02X} 81={:02X} 8E={:02X} 99:9A={:04X}\n",
                   ordinal, node, eye, st.readByte(node + 0x09), st.readByte(node + 0x14), st.readByte(node + 0x2A),
                   st.readByte(node + 0x2B), st.readByte(node + 0x2C), st.readHalf(node + 0x4E), st.readByte(node + 0x78),
                   st.readByte(node + 0x79), st.readByte(node + 0x7A), st.readByte(node + 0x7B), st.readByte(node + 0x81),
                   st.readByte(node + 0x8E), f99);
        fmt::print("        {}; {}; {}; next={:06X}; route={}\n", ocFields, p62Fields, p65Fields, link,
                   cptcClassifierRoute(node, eye, oc, p62, p65, f99));
        node = link;
    }
}

std::string MonitorCli::cptcClassifierRoute(int node, uint16_t eye, int oc, int p62, int p65, uint16_t f99)
{
    auto& st = m_.state;
    constexpr uint16_t kSu = 0xE2E4;
    constexpr uint16_t kTu = 0xE3E4;
    if (eye != kSu && eye != kTu) return "12D6 advance (not SU/TU)";

    const uint8_t fn = st.readByte(node + 0x8E);
    const uint8_t f7a = st.readByte(node + 0x7A);
    const uint8_t f7b = st.readByte(node + 0x7B);
    const uint8_t f81 = st.readByte(node + 0x81);
    if (eye == kTu && fn == 0xFC && f99 != 0) return "111B special FC/nonzero-9A path (global-dependent; may post/17C9)";

    if (eye == kSu) {
        if ((f7a & 0x02) != 0) return "11A1 -> 12D6 advance (SU +7A.02 set)";
    } else {
        if (!canReadGuest(oc, 0x0F)) return "11AA -> 12D6 advance (TU OC null/invalid)";
        if ((st.readByte(oc + 0x0C) & 0x80) != 0) return "11B6 -> 12D6 advance (TU OC active already set)";
        if (st.readByte(node + 0x2C) == 0xFE) return "11C8 -> 1700 request $190D -> 1750";
    }

    if ((st.readByte(node + 0x79) & 0x04) != 0) {
        const bool p65Marked = canReadGuest(p65, 0x3D) && (st.readByte(p65 + 0x3C) & 0x80) != 0;
        if (!p65Marked && (f7b & 0x19) == 0)
            return eye == kSu ? "1212 -> 1700 request $1916 -> 1750" : "1212 -> 1700 request $1913 -> 1750";
    }

    if (eye != kTu) return "122F -> 12D6 advance (SU has no TU tail)";
    if ((st.readByte(oc + 0x0E) & 0x0C) != 0) return "1247 -> 16D9 clear/wake; resume classifier tail at 1251";
    if (fn == 0xF0 || (f7b & 0x80) != 0) return "1264 -> handler 1876";
    if (fn == 0xF7) return "1275 -> 1700 request $1904 -> 1750";
    if (fn == 0xFB && (((f7a & 0x02) != 0) || ((f81 & 0x04) != 0))) return "1297 -> handler 17C9 -> 1700 -> 1750";
    if (fn == 0xF2 || (st.readByte(node + 0x78) & 0x10) != 0) return "12D2 -> handler 17EC";

    if (canReadGuest(p62, 0x18) && st.readHalf(p62) == 0xE3C2) {
        const int p17 = st.readAddr24(p62 + 0x15);
        if (canReadGuest(p17, 0x48) && (st.readByte(p17 + 0x47) & 0x08) != 0) return "12D2 -> handler 17EC (P62/P17 tail)";
    }
    return "12D6 advance";
}

bool MonitorCli::cptcNodeIsActive(int node, std::string& detail)
{
    auto& st = m_.state;
    detail = "invalid node";
    if (!canReadGuest(node, 0x7B)) return false;
    const uint16_t eye = st.readHalf(node);
    if (eye == 0xE2E4) {   // EBCDIC "SU"
        const uint8_t flags = st.readByte(node + 0x7A);
        detail = fmt::format("SU +7A={:02X}, bit 02 set", flags);
        return (flags & 0x02) != 0;
    }
    if (eye != WorkStationIob::kUnitBlockEyecatcher) return false;   // not EBCDIC "TU"

    // The sub-block operand likewise names the rightmost byte of this
    // address.
    const int sub = st.readAddr24(node + 0x53);
    if (!canReadGuest(sub, 0x0D)) {
        detail = fmt::format("TU +55 sub-block {:06X} null/invalid", sub);
        return false;
    }
    const uint8_t subFlags = st.readByte(sub + 0x0C);
    detail = fmt::format("TU +55 sub-block {:06X}, sub+0C={:02X}, bit 80 set", sub, subFlags);
    return (subFlags & 0x80) != 0;
}

void MonitorCli::workstationPipeline(const std::string& requestedId)
{
    VirtualWorkstation* station = findDisplayStation(requestedId);
    if (station == nullptr) {
        fmt::print("show workstation: no configured display station {}\n", requestedId);
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    fmt::print("WORKSTATION PIPELINE {} (alias {}, unit {:02X})\n", station->id(), requestedId, unit);
    WorkStationSlot* nativeSlot = m_.devices().workStations().find(unit);
    fmt::print("  native: ownership={} configured={} renderer={}\n",
               station->signOnAtIpl() ? "STRM36/QNUACQDP IPL-acquired" : "OS/400-owned until TFRM36",
               boolText(nativeSlot != nullptr && nativeSlot->configured()),
               nativeSlot != nullptr && nativeSlot->transferRendererBound() ? "NuXpfDsp5250 transfer"
               : nativeSlot != nullptr && nativeSlot->internalRendererBound() ? "NuDsp5250 internal" : "unbound");
    const host::WorkstationBackend& b = station->backend();
    fmt::print("  transport: endpoint={} listening={} attached={} ready={} terminal={} device={} user={}\n", b.endpoint(),
               boolText(b.listening()), boolText(station->attached()), boolText(station->ready()),
               b.terminalType().empty() ? "-" : b.terminalType(), b.deviceName().empty() ? "-" : b.deviceName(),
               b.userName().empty() ? "-" : b.userName());
    fmt::print("  wire: accepted={} refused={} rejected={} sent={}/{}B received={}/{}B dropped={}\n", b.sessionsAccepted(),
               b.sessionsRefused(), b.sessionsRejected(), b.recordsSent(), b.bytesSent(), b.recordsReceived(),
               b.bytesReceived(), b.recordsDropped());
    fmt::print("  guest I/O: streams={}/{}B input-records={} invite={} pending-input={} last-output={}\n",
               station->outputDataStreams(), station->outputDataBytes(), station->inputRecords(),
               boolText(station->inviteOutstanding()), station->pendingInput(),
               station->hasLastOutputDataStream() ? std::to_string(station->lastOutputDataStream().size()) + "B" : "none");
    wsContractSnapshot(station->id(), "pipeline");
    cptcClassifierTraceForStation(*station);
}

// ---- the bounded work-station trace -------------------------------------

void MonitorCli::workstationTrace(const std::vector<std::string>& a)
{
    if (a.size() < 4 || a.size() > 6) {
        fmt::print("usage: trace workstation <station-id|W1..W7> <lifecycle|classifier> [on|off|show|clear] [limit]\n");
        return;
    }
    VirtualWorkstation* station = findDisplayStation(a[2]);
    const std::string kind = toLower(a[3]);
    if (station == nullptr || (kind != "lifecycle" && kind != "classifier")) {
        fmt::print("{}\n", station == nullptr ? "trace workstation: no configured display station " + a[2]
                                              : std::string("trace workstation: kind must be lifecycle or classifier"));
        return;
    }
    const std::string action = a.size() >= 5 ? toLower(a[4]) : "on";
    if (action != "on" && action != "off" && action != "show" && action != "clear") {
        fmt::print("trace workstation: action must be on, off, show, or clear\n");
        return;
    }
    int limit = 256;
    if (a.size() == 6 && (!tryDigits(a[5], limit) || limit < 8 || limit > 16384)) {
        fmt::print("trace workstation: limit must be 8..16384 records\n");
        return;
    }
    auto it = workstationTraces_.find(station->id());
    if (it == workstationTraces_.end()) {
        WorkstationLifecycleTrace t;
        t.stationId = station->id();
        t.unit = (station->port() << 4) | station->address();
        it = workstationTraces_.emplace(station->id(), std::move(t)).first;
    }
    WorkstationLifecycleTrace& trace = it->second;
    if (a.size() == 6) trace.limit = limit;

    if (action == "clear") {
        if (kind == "lifecycle") trace.lifecycleEvents.clear();
        else trace.classifierEvents.clear();
        trace.classifierNodeByTask.clear();
        fmt::print("workstation {} {} history cleared\n", station->id(), kind);
        return;
    }
    if (action == "show") {
        showWorkstationTrace(trace, kind);
        return;
    }
    const bool enabled = action == "on";
    if (kind == "lifecycle") trace.lifecycle = enabled;
    else trace.classifier = enabled;
    if (enabled) {
        installWorkstationObservers();
        refreshWorkstationTraceFields(trace);
        addWorkstationTraceEvent(trace, kind, fmt::format("trace armed; bounded to {} records", trace.limit));
        if (kind == "classifier") cptcClassifierTraceForStation(*station);
        else recordWorkstationNativeState("trace-arm");
    } else {
        trace.classifierNodeByTask.clear();
        removeWorkstationObserversIfIdle();
    }
    fmt::print("workstation {} {} trace {}; {} retained record(s), limit {}\n", station->id(), kind, enabled ? "ON" : "OFF",
               kind == "lifecycle" ? trace.lifecycleEvents.size() : trace.classifierEvents.size(), trace.limit);
}

void MonitorCli::showWorkstationTrace(const WorkstationLifecycleTrace& trace, const std::string& kind)
{
    const std::vector<std::string>& events = kind == "lifecycle" ? trace.lifecycleEvents : trace.classifierEvents;
    fmt::print("WORKSTATION TRACE {} {}: enabled={}, retained={}, limit={}\n", trace.stationId, kind,
               boolText(kind == "lifecycle" ? trace.lifecycle : trace.classifier), events.size(), trace.limit);
    for (const std::string& line : events) fmt::print("  {}\n", line);
    if (events.empty()) fmt::print("  (empty)\n");
}

bool MonitorCli::anyWorkstationTrace(bool lifecycle, bool classifier) const
{
    for (const auto& kv : workstationTraces_)
        if ((lifecycle && kv.second.lifecycle) || (classifier && kv.second.classifier)) return true;
    return false;
}

std::vector<MonitorCli::WorkstationLifecycleTrace*> MonitorCli::lifecycleTraces()
{
    std::vector<WorkstationLifecycleTrace*> out;
    for (auto& kv : workstationTraces_)
        if (kv.second.lifecycle) out.push_back(&kv.second);
    return out;
}

void MonitorCli::installWorkstationObservers()
{
    m_.state.onObservedWrite = [this](int address, int length, const std::vector<uint8_t>& before,
                                      const std::vector<uint8_t>& after) { observeWorkstationWrite(address, length, before, after); };
    m_.msp().onBeforeInstruction = [this](uint16_t iar) { observeWorkstationInstruction(iar); };
    m_.nativeControlStorage().workStationDiagnosticObserver = [this](const std::string& kind, int unit, int exactTub,
                                                                     int configuredTub) {
        observeWorkstationControllerBoundary(kind, unit, exactTub, configuredTub);
    };
}

void MonitorCli::removeWorkstationObserversIfIdle()
{
    if (anyWorkstationTrace(true, true)) return;
    m_.state.onObservedWrite = nullptr;
    m_.msp().onBeforeInstruction = nullptr;
    m_.nativeControlStorage().workStationDiagnosticObserver = nullptr;
}

void MonitorCli::observeWorkstationControllerBoundary(const std::string& kind, int unit, int exactTub, int configuredTub)
{
    for (WorkstationLifecycleTrace* trace : lifecycleTraces()) {
        // The bootstrap/published console is intentionally relevant to every
        // station trace: a duplicate unit-00 TU in front of the requested
        // station is the contract split this diagnostic exists to expose.
        if (unit != trace->unit && unit != 0 && (exactTub == 0 || !traceNodeIsRelevant(*trace, exactTub))) continue;
        const std::string identity = exactTub == 0 ? "no QH50 match"
                                     : configuredTub == 0 ? "no configured TU/OC association"
                                     : exactTub == configuredTub ? "same as configured TU"
                                     : fmt::format("SPLIT from configured TU {:06X}", configuredTub);
        addWorkstationTraceEvent(*trace, "lifecycle",
                                 kind == "cnfws-f7-target"
                                     ? fmt::format("SLIC cnfws success tail targets submitted command-81 IOB {:06X}, unit={:02X}; {}; "
                                                   "writes +8E=F7 then nupoic00", exactTub, unit, identity)
                                     : fmt::format("SLIC wsentry wsfstdub70 first QH50 match for unit {:02X} is {:06X}; {}", unit,
                                                   exactTub, identity));
    }
}

void MonitorCli::recordWorkstationNativeState(const char* boundary)
{
    if (workstationTraces_.empty()) return;
    for (WorkstationLifecycleTrace* trace : lifecycleTraces()) {
        VirtualWorkstation* station = findDisplayStation(trace->stationId);
        if (station == nullptr) continue;
        const std::string state = fmt::format(
            "attached={} ready={} attention={} invite={} input-wire={} input-device={} TU={:06X} wire={}/{} guest-output={}/{}B "
            "controller-invite={} retained-a7={} action0-status={}",
            boolText(station->attached()), boolText(station->ready()), boolText(station->attentionPending()),
            boolText(station->inviteOutstanding()), station->pendingWireInput(), boolText(station->retainedDeviceInput()),
            station->tubAddress, station->backend().recordsSent(), station->backend().recordsReceived(),
            station->outputDataStreams(), station->outputDataBytes(), m_.devices().pendingControllerInviteCount(),
            m_.devices().countPendingPutWithInvitesForUnit((station->port() << 4) | station->address()),
            m_.devices().pendingAction0ActivationCount());
        if (state == trace->lastNativeState) continue;
        trace->lastNativeState = state;
        addWorkstationTraceEvent(*trace, "lifecycle", std::string(boundary) + " native: " + state);
    }
}

void MonitorCli::addWorkstationTraceEvent(WorkstationLifecycleTrace& trace, const std::string& kind, const std::string& detail)
{
    auto& csp = m_.nativeControlStorage();
    const int tb = csp.currentTaskBlock();
    const int taskId = TaskBlock::isTaskBlock(m_.state, tb) ? m_.state.readHalf(tb + TaskBlock::kOffTaskId) : 0;
    const std::string where = csp.describeActiveMember(tb, m_.state.msp.iar);
    const std::string line = fmt::format("#{} insn={} {} task={:06X}/{:04X} IAR={:04X}{}: {}", ++workstationTraceSequence_,
                                         m_.msp().instructionsExecuted(), kind, tb, taskId, m_.state.msp.iar,
                                         where.empty() ? "" : " " + where, detail);
    std::vector<std::string>& events = kind == "lifecycle" ? trace.lifecycleEvents : trace.classifierEvents;
    events.push_back(line);
    while (static_cast<int>(events.size()) > trace.limit) events.erase(events.begin());
}

namespace {

bool intersects(int a, int n, int b, int m) { return a <= b + m - 1 && b <= a + n - 1; }

std::string sliceHex(const std::vector<uint8_t>& value, int writeAddress, int lo, int hi)
{
    return bytesHex(value.data() + (lo - writeAddress), hi - lo + 1);
}

}  // namespace

void MonitorCli::observeWorkstationWrite(int address, int length, const std::vector<uint8_t>& before,
                                         const std::vector<uint8_t>& after)
{
    for (WorkstationLifecycleTrace* trace : lifecycleTraces()) {
        bool refresh = false;
        const std::vector<WorkstationTraceField> fields = trace->fields;
        for (const WorkstationTraceField& field : fields) {
            if (!intersects(address, length, field.address, field.length)) continue;
            const int lo = std::max(address, field.address);
            const int hi = std::min(address + length - 1, field.address + field.length - 1);
            addWorkstationTraceEvent(*trace, "lifecycle",
                                     fmt::format("{} {:06X}..{:06X} <- {} (was {})", field.name, lo, hi,
                                                 sliceHex(after, address, lo, hi), sliceHex(before, address, lo, hi)));
            refresh |= field.topology;
        }
        if (refresh) refreshWorkstationTraceFields(*trace);
    }
}

void MonitorCli::observeWorkstationInstruction(uint16_t iar)
{
    if (!anyWorkstationTrace(true, true)) return;
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    processors::controlstorage::LoadedMember member;
    int memberOffset;
    const int tb = csp.currentTaskBlock();
    if (!csp.tryActiveMember(tb, iar, member, memberOffset)) return;

    // Retain the guest-owned console-qualification decisions themselves,
    // not merely the storage writes which happen to follow them.  These
    // stops are deliberately member-qualified: all three transients use the
    // same logical 0x1000 load window.
    if (anyWorkstationTrace(true, false)) observeConsoleQualificationInstruction(member, iar, tb);

    // The attach module can replace the automatically allocated task
    // identifier from the attach parameter's $ATTSKID at workbase+1C..1D.
    // Preserve both sides of that otherwise transient decision in the
    // bounded lifecycle trace: 11C2 is immediately before the copy and 11C6
    // is its join immediately afterwards (also reached when the copy was
    // skipped because the requested stable ID was zero).
    if (equalsIgnoreCase(stripHash(member.name), "SVAT") && (iar == 0x11C2 || iar == 0x11C6)) {
        const int work = st.msp.xr2;
        const uint16_t attachId = canReadGuest(work + 0x1C, 2) ? st.readHalf(work + 0x1C) : static_cast<uint16_t>(0);
        const uint16_t taskId = TaskBlock::isTaskBlock(st, tb) ? st.readHalf(tb + TaskBlock::kOffTaskId) : static_cast<uint16_t>(0);
        for (WorkstationLifecycleTrace* trace : lifecycleTraces())
            addWorkstationTraceEvent(*trace, "lifecycle",
                                     fmt::format("#SVAT {:04X} {} $ATTSKID={:04X} at workbase {:06X}+1C; TB {:06X} id={:04X}", iar,
                                                 iar == 0x11C2 ? "before task-id copy" : "after/skip task-id copy", attachId,
                                                 work, tb, taskId));
    }

    if (!anyWorkstationTrace(false, true) || !equalsIgnoreCase(stripHash(member.name), "CPTC")) return;
    if (iar != 0x110C && iar != 0x1700 && iar != 0x1750 && iar != 0x16C7 && iar != 0x1754 && iar != 0x1786) return;

    const int xr2 = st.msp.xr2;
    for (auto& kv : workstationTraces_) {
        WorkstationLifecycleTrace& trace = kv.second;
        if (!trace.classifier) continue;
        int node;
        if (iar == 0x110C) {
            if (!traceNodeIsRelevant(trace, xr2)) continue;
            const uint16_t eye = st.readHalf(xr2);
            const int oc = st.readAddr24(xr2 + 0x53);
            const int p62 = st.readAddr24(xr2 + 0x60);
            const int p65 = st.readAddr24(xr2 + 0x63);
            const uint16_t f99 = st.readHalf(xr2 + 0x99);
            const std::string route = cptcClassifierRoute(xr2, eye, oc, p62, p65, f99);
            addWorkstationTraceEvent(trace, "classifier",
                                     fmt::format("110C examines TU {:06X} unit={:02X} fn={:02X} OC={:06X}/+0C={} key={:04X} status+2A={:02X} "
                                                 "+78={:02X} +79={:02X}; {}",
                                                 xr2, st.readByte(xr2 + WorkStationIob::kOffUnitAddress), st.readByte(xr2 + 0x8E), oc,
                                                 canReadGuest(oc, 0x0D) ? fmt::format("{:02X}", st.readByte(oc + 0x0C)) : "--",
                                                 st.readHalf(xr2 + 0x4E), st.readByte(xr2 + 0x2A), st.readByte(xr2 + 0x78),
                                                 st.readByte(xr2 + 0x79), route));
            if (route.find("1700") != std::string::npos) trace.classifierNodeByTask[tb] = xr2;
            else trace.classifierNodeByTask.erase(tb);
            continue;
        }
        auto found = trace.classifierNodeByTask.find(tb);
        if (found == trace.classifierNodeByTask.end()) {
            if (iar != 0x1700) continue;
            if (!traceNodeIsRelevant(trace, xr2)) continue;
            node = xr2;
            trace.classifierNodeByTask[tb] = node;
        } else {
            node = found->second;
        }
        addWorkstationTraceEvent(trace, "classifier",
                                 fmt::format("{:04X} {}; selected TU {:06X}", iar,
                                             iar == 0x1700 ? "request handler reached"
                                             : iar == 0x1750 ? "nested CF/continuation call reached"
                                             : iar == 0x16C7 ? "completion-return handler reached"
                                             : iar == 0x1754 ? "saved continuation reached"
                                                             : "OC-active commit instruction reached (pre-execution)",
                                             node));
        if (iar == 0x1786) trace.classifierNodeByTask.erase(tb);
    }
}

void MonitorCli::observeConsoleQualificationInstruction(const processors::controlstorage::LoadedMember& member, uint16_t iar, int tb)
{
    auto& st = m_.state;
    const std::string name = upper(stripHash(member.name));
    int node = 0;
    std::string detail;

    if (name == "MSSC" && iar == 0x10F5) {
        node = st.msp.xr1;
        detail = "#MSSC candidate test at 10F5";
    } else if (name == "MSSC" && iar == 0x1127) {
        node = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
        detail = "#MSSC returns to #MSIPL with SCADMTUB";
    } else if (name == "SVTUB" && iar == 0x163E) {
        node = st.msp.xr1;
        detail = "#SVTUB clone copied old +75; 163E will set bit 10";
    } else if (name == "SVTUB" && iar == 0x166F) {
        node = st.msp.xr1;
        detail = "#SVTUB clone copied old +83; 166F will clear mask 8F";
    } else if (name == "SVTUB" && iar == 0x1689) {
        node = st.msp.xr1;
        detail = "#SVTUB publishes detached clone through old TU +8F..91";
    } else if (name == "CPON" && iar == 0x1C23) {
        node = st.msp.xr1;
        detail = "#CPON selected station after 1C20 set bit 10";
    } else if (name == "CPON" && iar == 0x20DE) {
        node = st.msp.xr1;
        detail = "#CPON predecessor test: bit 80 decides whether 20E4 demotes it";
    } else if (name == "CPON" && iar == 0x20EA) {
        node = st.msp.xr1;
        detail = "#CPON predecessor after 20E4/20E7 clear 80 and set 40";
    }
    (void)tb;

    if (detail.empty()) return;
    for (WorkstationLifecycleTrace* trace : lifecycleTraces()) {
        if (node != 0 && !traceNodeIsRelevant(*trace, node)) continue;
        const std::string state = node != 0 && canReadGuest(node, 0x84)
            ? fmt::format(" TU={:06X} unit={:02X} +75={:02X} +83={:02X} qualifies={}", node,
                          st.readByte(node + WorkStationIob::kOffUnitAddress), st.readByte(node + 0x75), st.readByte(node + 0x83),
                          ((st.readByte(node + 0x75) & 0x90) == 0x90 && (st.readByte(node + 0x83) & 0x08) == 0) ? "yes" : "no")
            : fmt::format(" TU={:06X} (not readable)", node);
        addWorkstationTraceEvent(*trace, "lifecycle", detail + state);
    }
}

bool MonitorCli::traceNodeIsRelevant(const WorkstationLifecycleTrace& trace, int node)
{
    auto& st = m_.state;
    if (!canReadGuest(node, 0x9B) || st.readHalf(node) != WorkStationIob::kUnitBlockEyecatcher) return false;
    const int published = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    const int unit = st.readByte(node + WorkStationIob::kOffUnitAddress);
    return node == published || unit == 0 || unit == trace.unit;
}

void MonitorCli::addWorkstationTraceField(WorkstationLifecycleTrace& trace, int address, int length, const std::string& name,
                                          bool topology)
{
    if (address == 0 || !canReadGuest(address, length)) return;
    for (const auto& x : trace.fields)
        if (x.address == address && x.length == length && x.name == name) return;
    WorkstationTraceField f;
    f.address = address;
    f.length = length;
    f.name = name;
    f.topology = topology;
    trace.fields.push_back(f);
}

void MonitorCli::refreshWorkstationTraceFields(WorkstationLifecycleTrace& trace)
{
    auto& st = m_.state;
    trace.fields.clear();
    addWorkstationTraceField(trace, GuestLowStorage::kSystemConsoleUnitBlockPointer, 3, "SCADMTUB", true);
    const int queues[] = {GuestLowStorage::kConsoleUnitBlockQueue, GuestLowStorage::kSharedUnitBlockQueue, 53,
                          GuestLowStorage::kWorkStationActivationQueue};
    for (int queue : queues)
        addWorkstationTraceField(trace, GuestLowStorage::queueHeader(queue), 3, "QH" + std::to_string(queue) + " head", true);

    // Task identities can be born automatically in the task builder and
    // then be replaced by the attach module from $ATTSKID.  Follow every
    // live task rather than only tasks already reachable from a JCB: the
    // missing-JCB case is precisely where that diagnostic is needed.
    int taskNode = st.readAddr24(GuestLowStorage::queueHeader(39));
    std::set<int> taskSeen;
    for (int guard = 0; taskNode != 0 && guard++ < 4096 && taskSeen.insert(taskNode).second;) {
        if (!TaskBlock::isTaskBlock(st, taskNode)) break;
        addWorkstationTraceField(trace, taskNode + TaskBlock::kOffTaskId, 2, fmt::format("QH39 TB {:06X} task id", taskNode), false);
        addWorkstationTraceField(trace, taskNode + TaskBlock::kOffJobControlBlock, 3, fmt::format("QH39 TB {:06X} JCB", taskNode), true);
        const int work = st.readAddr24(taskNode + TaskBlock::kOffWorkBase);
        if (work != 0) addWorkstationTraceField(trace, work + 0x1C, 2, fmt::format("QH39 TB {:06X} $ATTSKID source", taskNode), false);
        taskNode = st.readAddr24(taskNode + TaskBlock::kOffQueue39Link);
    }

    const int published = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    int node = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
    std::set<int> seen;
    for (int guard = 0; node != 0 && guard++ < 4096 && seen.insert(node).second;) {
        if (!canReadGuest(node, 0x9B)) break;
        addWorkstationTraceField(trace, node + 0x4B, 3, fmt::format("TU {:06X} QH50 link", node), true);
        addWorkstationTraceField(trace, node + 0x50, 3, fmt::format("TU {:06X} QH49 link", node), true);
        const bool relevant = st.readHalf(node) == WorkStationIob::kUnitBlockEyecatcher &&
                              (node == published || st.readByte(node + WorkStationIob::kOffUnitAddress) == 0 ||
                               st.readByte(node + WorkStationIob::kOffUnitAddress) == trace.unit);
        if (relevant) {
            const int nodeUnit = st.readByte(node + WorkStationIob::kOffUnitAddress);
            const std::string tag = node == published ? "published TU" : nodeUnit == 0 ? "unit-00 TU" : "station TU";
            addWorkstationTraceField(trace, node + 0x01, 1, tag + " +01", false);
            addWorkstationTraceField(trace, node + 0x08, 1, tag + " lifecycle +08", false);
            addWorkstationTraceField(trace, node + 0x0A, 0x0F, tag + " IOB +0A..18", false);
            addWorkstationTraceField(trace, node + 0x2A, 3, tag + " status +2A..2C", false);
            addWorkstationTraceField(trace, node + 0x4E, 8, tag + " key/link/OC +4E..55", true);
            addWorkstationTraceField(trace, node + 0x60, 6, tag + " task/JCB +60..65", true);
            addWorkstationTraceField(trace, node + 0x75, 0x0F, tag + " flags +75..83", false);
            addWorkstationTraceField(trace, node + 0x84, 4, tag + " display workspace +84..87", true);
            addWorkstationTraceField(trace, node + 0x8D, 1, tag + " request/AID +8D", false);
            addWorkstationTraceField(trace, node + 0x8E, 4, tag + " function/replacement +8E..91", true);
            addWorkstationTraceField(trace, node + 0x99, 2, tag + " classifier +99..9A", false);
            const int oc = st.readAddr24(node + 0x53);
            if (oc != 0) addWorkstationTraceField(trace, oc, 0x0F, fmt::format("TU {:06X} OC {:06X} +00..0E", node, oc), false);
            const int job = st.readAddr24(node + 0x63);
            if (job != 0) {
                addWorkstationTraceField(trace, job + 0x47, 1, fmt::format("JCB {:06X} +47", job), false);
                addWorkstationTraceField(trace, job + 0x4D, 3, fmt::format("JCB {:06X} task +4D..4F", job), true);
                addWorkstationTaskFields(trace, st.readAddr24(job + 0x4D));
            }
        }
        node = st.readAddr24(node + 0x4B);
    }

    node = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kWorkStationActivationQueue));
    seen.clear();
    for (int guard = 0; node != 0 && guard++ < 4096 && seen.insert(node).second;) {
        if (!canReadGuest(node, 0x50)) break;
        addWorkstationTraceField(trace, node + 0x0F, 3, fmt::format("QH112 JCB {:06X} link", node), true);
        addWorkstationTraceField(trace, node + 0x47, 1, fmt::format("QH112 JCB {:06X} +47", node), false);
        addWorkstationTraceField(trace, node + 0x4D, 3, fmt::format("QH112 JCB {:06X} task", node), true);
        addWorkstationTaskFields(trace, st.readAddr24(node + 0x4D));
        node = st.readAddr24(node + 0x0F);
    }
}

void MonitorCli::addWorkstationTaskFields(WorkstationLifecycleTrace& trace, int task)
{
    if (!TaskBlock::isTaskBlock(m_.state, task)) return;
    addWorkstationTraceField(trace, task + TaskBlock::kOffState, 2, fmt::format("TB {:06X} state", task), false);
    addWorkstationTraceField(trace, task + TaskBlock::kOffRequestBlock, 3, fmt::format("TB {:06X} current RB", task), true);
    addWorkstationTraceField(trace, task + TaskBlock::kOffCompleteQueue, 3, fmt::format("TB {:06X} completion queue", task), true);
    addWorkstationTraceField(trace, task + TaskBlock::kOffWorkBase, 3, fmt::format("TB {:06X} work base", task), false);
}

std::string MonitorCli::guestHex(int address, int length)
{
    if (!canReadGuest(address, length)) return "--";
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    m_.state.read(address, bytes.data(), length);
    return bytesHex(bytes.data(), length);
}

void MonitorCli::cptcClassifierTraceForStation(VirtualWorkstation& station)
{
    auto& st = m_.state;
    const int unit = (station.port() << 4) | station.address();
    const int published = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    int node = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
    fmt::print("  #CPTC classifier projection for station {}, unit {:02X}:\n", station.id(), unit);
    std::set<int> seen;
    int matched = 0;
    for (int ordinal = 0; node != 0 && ordinal < 4096 && seen.insert(node).second; ordinal++) {
        if (!canReadGuest(node, 0x9B)) break;
        const int next = st.readAddr24(node + 0x4B);
        if (st.readHalf(node) == WorkStationIob::kUnitBlockEyecatcher &&
            (node == published || st.readByte(node + WorkStationIob::kOffUnitAddress) == 0 ||
             st.readByte(node + WorkStationIob::kOffUnitAddress) == unit)) {
            const int oc = st.readAddr24(node + 0x53);
            const int p62 = st.readAddr24(node + 0x60);
            const int p65 = st.readAddr24(node + 0x63);
            const uint16_t f99 = st.readHalf(node + 0x99);
            fmt::print("    {:06X} {}: unit={:02X} OC={:06X} fn={:02X} status={:02X}/{:02X}/{:02X}; route={}\n", node,
                       node == published ? "PUBLISHED" : st.readByte(node + WorkStationIob::kOffUnitAddress) == 0 ? "UNIT-00  " : "TARGET   ",
                       st.readByte(node + WorkStationIob::kOffUnitAddress), oc, st.readByte(node + 0x8E), st.readByte(node + 0x2A),
                       st.readByte(node + 0x78), st.readByte(node + 0x79), cptcClassifierRoute(node, st.readHalf(node), oc, p62, p65, f99));
            fmt::print("      classifier inputs: +01={:02X} +14={:02X} +2A..2C={} QH50-link(+4D)={:06X} key(+4F)={:04X} +55(OC)={:06X} "
                       "+62={:06X} +65={:06X} +78..81={} +8E={:02X} +99..9A={:04X}\n",
                       st.readByte(node + 0x01), st.readByte(node + 0x14), guestHex(node + 0x2A, 3), st.readAddr24(node + 0x4B),
                       st.readHalf(node + 0x4E), oc, p62, p65, guestHex(node + 0x78, 10), st.readByte(node + 0x8E), f99);
            matched++;
        }
        node = next;
    }
    if (matched == 0) fmt::print("    no target or published-console TU on QH50\n");
}

// ---- the work-station contract and the unit-block topology --------------

// Read-only correlation of the host station, the controller's first-match
// rule, the QH50 topology and the work-station task's waiters.
void MonitorCli::wsContract(const std::vector<std::string>& a)
{
    if (a.size() > 1 && equalsIgnoreCase(a[1], "clear")) {
        if (a.size() != 2) {
            fmt::print("usage: wscontract clear\n");
            return;
        }
        watch({"watch", "off"});
        return;
    }
    if (a.size() > 1 && equalsIgnoreCase(a[1], "watch")) {
        if (a.size() > 3) {
            fmt::print("usage: wscontract watch [station-id]\n");
            return;
        }
        const std::string requested = a.size() == 3 ? a[2] : "0.0";
        VirtualWorkstation* station = findDisplayStation(requested);
        const std::string stationId = station == nullptr ? requested : station->id();
        wsContractSnapshot(stationId, "watch-arm");
        armWsContractWatches(stationId);
        return;
    }
    if (a.size() > 1 && equalsIgnoreCase(a[1], "trace")) {
        if (a.size() != 4 || (!equalsIgnoreCase(a[3], "on") && !equalsIgnoreCase(a[3], "off"))) {
            fmt::print("usage: wscontract trace <station-id> <on|off>\n");
            return;
        }
        VirtualWorkstation* station = findDisplayStation(a[2]);
        wsContractTraceStation_ = equalsIgnoreCase(a[3], "on") ? (station == nullptr ? a[2] : station->id()) : std::string();
        if (wsContractTraceStation_.empty()) fmt::print("WS contract boundary tracing OFF\n");
        else fmt::print("WS contract boundary tracing ON for station {}\n", wsContractTraceStation_);
        if (!wsContractTraceStation_.empty()) wsContractSnapshot(wsContractTraceStation_, "trace-enabled");
        return;
    }
    if (a.size() > 2) {
        fmt::print("usage: wscontract [station-id]\n");
        return;
    }
    wsContractSnapshot(a.size() == 2 ? a[1] : "0.0", "operator");
}

// Arm ordinary storage watches from the live QH112 graph.  The addresses
// are deliberately not supplied by the operator: QH112 owns the JCB and
// JCB+4D..4F owns the associated task.  This distinguishes a writer acting
// on the activation object from the same member touching an unrelated
// task/JCB.  It is read-only apart from debugger state.
void MonitorCli::armWsContractWatches(const std::string& stationId)
{
    wsContractAutoWatch_ = true;
    wsContractWatchStation_ = stationId;
    wsContractDerivedWatchKeys_.clear();
    installWatchReporter();
    const int added = refreshWsContractWatches();
    const int elements = countWsContractElements();
    fmt::print("WS-CONTRACT watches armed from QH112 ({} activation element(s), {} derived range(s), {} total watchpoint(s)); "
               "use `wscontract clear` to clear\n", elements, added, m_.state.watchCount());
}

int MonitorCli::addWsContractWatch(int address, int length)
{
    const long long key = (static_cast<long long>(address) << 32) | static_cast<unsigned int>(length);
    if (!wsContractDerivedWatchKeys_.insert(key).second) return 0;
    m_.state.addWatch(address, address + length - 1);
    return 1;
}

int MonitorCli::refreshWsContractWatches()
{
    auto& st = m_.state;
    const int qh = GuestLowStorage::queueHeader(GuestLowStorage::kWorkStationActivationQueue);
    int added = addWsContractWatch(qh, 3);
    int node = st.readAddr24(qh);
    int elements = 0;
    std::set<int> seen;
    while (node != 0 && elements < 4096 && seen.insert(node).second) {
        if (!canReadGuest(node, 0x50)) break;
        added += addWsContractWatch(node + 0x47, 1);
        added += addWsContractWatch(node + 0x4D, 3);
        const int task = st.readAddr24(node + 0x4D);
        if (TaskBlock::isTaskBlock(st, task)) {
            added += addWsContractWatch(task + 0x20, 1);
            added += addWsContractWatch(task + 0x30, 1);
        }
        node = st.readAddr24(node + 0x0F);
        elements++;
    }

    // Follow the workstation topology as well as the activation queue.  The
    // attach module's first-pass predicates live on the replacement TU,
    // which does not exist when this command is normally armed.  Watching
    // the QH50 TU's +8F link lets the ordinary watch callback discover that
    // replacement as soon as the guest publishes it; no operator has to know
    // or supply an allocator-dependent address.
    VirtualWorkstation* station = findDisplayStation(wsContractWatchStation_.empty() ? "0.0" : wsContractWatchStation_);
    if (station != nullptr) {
        const int unit = (station->port() << 4) | station->address();
        node = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
        seen.clear();
        while (node != 0 && seen.insert(node).second && seen.size() < 4096) {
            if (!canReadGuest(node, 0x92)) break;
            if (st.readHalf(node) == WorkStationIob::kUnitBlockEyecatcher && st.readByte(node + WorkStationIob::kOffUnitAddress) == unit) {
                // The TU is also the SVC 43 IOB.  Follow its request
                // descriptor so a later 27/A7 PUT makes the guest output
                // buffer discoverable before the device set consumes it.
                added += addWsContractWatch(node + WorkStationIob::kOffClass, 8);
                added += addWsContractWatch(node + UnitBlock::kOffStatus, 1);
                added += addWsContractWatch(node + 0x78, 1);
                added += addWsContractWatch(node + 0x79, 1);
                added += addWsContractWatch(node + 0x8F, 3);
                const int command = WorkStationIob::command(st, node);
                const int outputLength = WorkStationIob::length(st, node);
                const int outputBuffer = st.readAddr24(node + WorkStationIob::kOffDataBuffer);
                if ((command == WorkStationIob::kCmdPut || command == WorkStationIob::kCmdPutWithInvite) && outputLength > 0 &&
                    outputLength <= 4096 && canReadGuest(outputBuffer, outputLength))
                    added += addWsContractWatch(outputBuffer, outputLength);
                const int replacement = st.readAddr24(node + 0x8F);
                if (canReadGuest(replacement, UnitBlock::kTubMinimumSize) &&
                    st.readHalf(replacement) == WorkStationIob::kUnitBlockEyecatcher) {
                    added += addWsContractWatch(replacement + UnitBlock::kOffStatus, 1);
                    added += addWsContractWatch(replacement + 0x78, 1);
                    added += addWsContractWatch(replacement + 0x79, 1);
                }
            }
            node = st.readAddr24(node + 0x4B);
        }
    }
    return added;
}

int MonitorCli::countWsContractElements()
{
    auto& st = m_.state;
    const int qh = GuestLowStorage::queueHeader(GuestLowStorage::kWorkStationActivationQueue);
    int node = st.readAddr24(qh);
    int elements = 0;
    std::set<int> seen;
    while (node != 0 && elements < 4096 && seen.insert(node).second) {
        if (!canReadGuest(node, 0x12)) break;
        node = st.readAddr24(node + 0x0F);
        elements++;
    }
    return elements;
}

void MonitorCli::wsContractSnapshot(const std::string& stationId, const char* boundary)
{
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    VirtualWorkstation* station = findDisplayStation(stationId);
    if (station == nullptr) {
        fmt::print("wscontract: no display station {}\n", stationId);
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    const int q50 = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue));
    const int slicTub = csp.resolveWsEntryTubByUnit(unit);
    const int configuredTub = csp.resolveConfiguredTubByUnit(unit);
    const int published = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    const int activation = st.readAddr24(GuestLowStorage::queueHeader(GuestLowStorage::kWorkStationActivationQueue));

    fmt::print("WS-CONTRACT {} #{}: station {}, unit {:02X}\n", boundary, ++wsContractSequence_, station->id(), unit);
    fmt::print("  host attached={} ready={} attention={} invite={} input-wire={} input-device={}; assigned TU={:06X}\n",
               boolText(station->attached()), boolText(station->ready()), boolText(station->attentionPending()),
               boolText(station->inviteOutstanding()), station->pendingWireInput(), boolText(station->retainedDeviceInput()),
               station->tubAddress);
    fmt::print("  native controller: legacy guest unit-FF IOB(s)={}; retained A7 Put/Get(s)={}; unconsumed action-0 BA.20 "
               "activation status(es)={}\n", m_.devices().pendingControllerInviteCount(),
               m_.devices().countPendingPutWithInvitesForUnit(unit), m_.devices().pendingAction0ActivationCount());
    fmt::print("  guest QH50={:06X}; published console={:06X}; configured TU={:06X}\n", q50, published, configuredTub);
    fmt::print("  SLIC wsfstdub70 first unit-{:02X} match={:06X}{}\n", unit, slicTub,
               slicTub != 0 && configuredTub != 0 && slicTub != configuredTub ? "  SPLIT from configured TU" : "");
    fmt::print("  guest QH112 workstation-activation head={:06X}{}\n", activation,
               activation == 0 ? "  EMPTY - CPTC activate_chain has no station/session work" : "");

    int node = activation, ordinal = 0;
    std::set<int> seen;
    while (node != 0 && ordinal < 4096 && seen.insert(node).second) {
        if (!canReadGuest(node, 0x50)) {
            fmt::print("    QH112[{}] {:06X}: outside readable guest storage\n", ordinal, node);
            break;
        }
        const int nextActivation = st.readAddr24(node + 0x0F);
        const int associatedTask = st.readAddr24(node + 0x4D);
        const uint8_t task20 = canReadGuest(associatedTask, 0x31) ? st.readByte(associatedTask + 0x20) : static_cast<uint8_t>(0);
        const uint8_t task30 = canReadGuest(associatedTask, 0x31) ? st.readByte(associatedTask + 0x30) : static_cast<uint8_t>(0);
        fmt::print("    QH112[{}] JCB {:06X}: next={:06X} JCB+47={:02X} associated-task={:06X} task+20={:02X} task+30={:02X}\n", ordinal,
                   node, nextActivation, st.readByte(node + 0x47), associatedTask, task20, task30);
        node = nextActivation;
        ordinal++;
    }
    if (node != 0) fmt::print("    QH112 walk stopped: corrupt or longer than 4096 elements\n");

    node = q50;
    ordinal = 0;
    seen.clear();
    while (node != 0 && ordinal < 4096 && seen.insert(node).second) {
        if (!canReadGuest(node, 0x92)) break;
        const int next = st.readAddr24(node + 0x4B);
        if (st.readHalf(node) == WorkStationIob::kUnitBlockEyecatcher && st.readByte(node + WorkStationIob::kOffUnitAddress) == unit) {
            const int oc = st.readAddr24(node + 0x53);
            const uint8_t ocActive = canReadGuest(oc, 0x0D) ? st.readByte(oc + 0x0C) : static_cast<uint8_t>(0);
            const int job = st.readAddr24(node + 0x63);
            fmt::print("    QH50[{}] TU {:06X}: class={:02X} OC={:06X} active={:02X} +8={:02X} +2A={:02X} +75={:02X} +83={:02X} +78={:02X} "
                       "+79={:02X} +8E={:02X} JCB={:06X} replacement={:06X}{}\n",
                       ordinal, node, st.readByte(node + 0x0A), oc, ocActive, st.readByte(node + 8), st.readByte(node + UnitBlock::kOffStatus),
                       st.readByte(node + 0x75), st.readByte(node + 0x83), st.readByte(node + 0x78), st.readByte(node + 0x79),
                       st.readByte(node + 0x8E), job, st.readAddr24(node + 0x8F), node == slicTub ? "  <- SLIC SELECTS" : "");
            const uint8_t mssc75 = st.readByte(node + 0x75);
            const uint8_t mssc83 = st.readByte(node + 0x83);
            const bool msscQualifies = (mssc75 & 0x90) == 0x90 && (mssc83 & 0x08) == 0;
            fmt::print("      #MSSC 10F5/10FB: (+75 & 90)==90 is {}; +83.08 clear is {}; qualifies={}\n",
                       (mssc75 & 0x90) == 0x90 ? "true" : "false", (mssc83 & 0x08) == 0 ? "true" : "false",
                       msscQualifies ? "yes" : "no");
            const int replacement = st.readAddr24(node + 0x8F);
            if (canReadGuest(replacement, 0x66) && st.readHalf(replacement) == WorkStationIob::kUnitBlockEyecatcher)
                fmt::print("      replacement TU {:06X}: JCB={:06X} +2A={:02X} +78={:02X} +79={:02X} +8E={:02X}\n", replacement,
                           st.readAddr24(replacement + 0x63), st.readByte(replacement + UnitBlock::kOffStatus),
                           st.readByte(replacement + 0x78), st.readByte(replacement + 0x79), st.readByte(replacement + 0x8E));
        }
        node = next;
        ordinal++;
    }

    int tasks = 0;
    node = st.readAddr24(GuestLowStorage::queueHeader(39));
    seen.clear();
    while (node != 0 && seen.insert(node).second && tasks < 4096) {
        const int next = st.readAddr24(node + TaskBlock::kOffQueue39Link);
        if (TaskBlock::isTaskBlock(st, node) && st.readHalf(node + TaskBlock::kOffTaskId) == 0x0109) {
            const int rb = st.readAddr24(node + TaskBlock::kOffRequestBlock);
            const uint16_t iar = rb != 0 ? st.readHalf(rb + RequestBlock::kOffIar) : static_cast<uint16_t>(0);
            const uint16_t wr6 = rb != 0 ? RequestBlock::readWr(st, rb, 6) : static_cast<uint16_t>(0);
            fmt::print("    CPTC {:06X}: state={:02X} stat2={:02X} RB={:06X} resume={:04X} WR6={:04X} CQ={:06X} B={:06X}\n", node,
                       st.readByte(node + TaskBlock::kOffState), st.readByte(node + TaskBlock::kOffStat2), rb, iar, wr6,
                       st.readAddr24(node + TaskBlock::kOffCompleteQueue), st.readAddr24(node + TaskBlock::kOffWorkBase));
            tasks++;
        }
        node = next;
    }
    if (tasks == 0) fmt::print("    CPTC: none on QH39\n");
    fmt::print("  MSP stopped={} idle-event-wait={}; queued wsentry scans={}\n", boolText(m_.msp().stopped()),
               boolText(csp.idleEventWait()), pendingWsEntryUnits_.size());
}

// Command-only deferred wsentry experiment.  The optional CPTC2B terminal is
// inferred and refuses zero or ambiguous waiters.
void MonitorCli::wsEntry(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() >= 2 && equalsIgnoreCase(a[1], "drain")) {
        const std::string terminal = a.size() == 3 ? toLower(a[2]) : "scan";
        if (a.size() > 3 || (terminal != "scan" && terminal != "cptc2b" && terminal != "autosignon")) {
            fmt::print("usage: wsentry drain [scan|cptc2b|autosignon]\n");
            return;
        }
        drainWsEntry(csp, terminal);
        return;
    }
    if (a.size() < 3 || a.size() > 4) {
        fmt::print("usage: wsentry <station-id> <schedule|full> [scan|cptc2b|autosignon]\n");
        return;
    }
    VirtualWorkstation* station = stationById(a[1], true);
    const std::string operation = toLower(a[2]);
    const std::string mode = a.size() == 4 ? toLower(a[3]) : "scan";
    if (station == nullptr || (operation != "schedule" && operation != "full") ||
        (mode != "scan" && mode != "cptc2b" && mode != "autosignon")) {
        fmt::print("usage: wsentry <station-id> <schedule|full> [scan|cptc2b|autosignon]\n");
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    pendingWsEntryUnits_.push_back(unit);
    fmt::print("WSENTRY scheduled NuSetAction 0x17 station {}, unit {:02X}; depth {}\n", station->id(), unit, pendingWsEntryUnits_.size());
    if (operation == "full") drainWsEntry(csp, mode);
}

// Expose the recovered HRI-ready producer as a monitor command so its
// timing can be compared without mutating any SSP object.  The control
// processor enforces the native prerequisites and performs only the
// action-20 response contract.
void MonitorCli::workStationHriReport(const std::vector<std::string>& a)
{
    if (a.size() != 2) {
        fmt::print("usage: wshri <station-id>\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    VirtualWorkstation* station = stationById(a[1], true);
    if (station == nullptr) {
        fmt::print("wshri: no such display station or control storage processor\n");
        return;
    }
    const int unit = (station->port() << 4) | station->address();
    const bool delivered = csp.reportWorkStationHriReady(unit, "monitor wshri " + station->id());
    if (delivered) fmt::print("wshri {}: reported HRI ran NuWsIoAction(20) for unit {:02X}\n", station->id(), unit);
    else fmt::print("wshri {}: native HRI/NuWs prerequisites not satisfied\n", station->id());
}

void MonitorCli::drainWsEntry(As36ControlStorageProcessor& csp, const std::string& terminal)
{
    auto& st = m_.state;
    if (pendingWsEntryUnits_.empty()) {
        fmt::print("WSENTRY drain: no deferred station scan queued\n");
        return;
    }
    const int unit = pendingWsEntryUnits_.front();
    pendingWsEntryUnits_.pop_front();
    const int tub = csp.resolveWsEntryTubByUnit(unit);
    fmt::print("WSENTRY drain NuSetAction 0x17: unit {:02X}, wsfstdub70 -> {:06X}, terminal {}\n", unit, tub, upper(terminal));
    if (pendingWsEntryUnits_.empty() && csp.completePendingM36WorkStationTransfer("wsentry drain -> schedulePendedXferMachActions"))
        fmt::print("WSENTRY queue drained; pending TFRM36 action-0 bind completed ({})\n", csp.m36WorkStationTransferState());
    if (tub == 0 || terminal == "scan") return;

    if (terminal == "autosignon") {
        // Bounded correlation experiment: the transfer has already selected
        // and lent the configured work station, but the controller's guest
        // operand is the FIRST matching QH50 TU.  Exercise that exact
        // identity without changing the normal transfer path.
        bool ok = csp.raiseDeviceAttention(tub, GuestLowStorage::kTaskBlock, "wsentry exact autosignon present");
        // The reference also raises its class map-gate experiment switch
        // here; SIM/36 does not carry that experiment.
        ok = csp.postConsoleSignOnRequest(tub, "wsentry exact autosignon request") && ok;
        ok = csp.postConsoleSignOnStatement(tub, "wsentry exact autosignon statement") && ok;
        if (ok) fmt::print("WSENTRY AUTOSIGNON posted through exact SLIC TU {:06X}; run to observe #CPON/#SVAT/QH112\n", tub);
        else fmt::print("WSENTRY AUTOSIGNON could not post the complete experiment\n");
        return;
    }

    std::vector<int> candidates;
    int task = st.readAddr24(GuestLowStorage::queueHeader(39));
    std::set<int> seen;
    while (task != 0 && seen.insert(task).second && seen.size() < 4096) {
        const int next = st.readAddr24(task + TaskBlock::kOffQueue39Link);
        if (TaskBlock::isTaskBlock(st, task) && st.readHalf(task + TaskBlock::kOffTaskId) == 0x0109 &&
            (st.readByte(task + TaskBlock::kOffStat2) & 0x80) != 0) {
            const int rb = st.readAddr24(task + TaskBlock::kOffRequestBlock);
            if (rb != 0 && RequestBlock::readWr(st, rb, 6) == 0x002B) candidates.push_back(task);
        }
        task = next;
    }
    if (candidates.size() != 1) {
        fmt::print("WSENTRY CPTC2B refused: expected exactly one id-0109 waiter with WR6=002B, found {}{}\n", candidates.size(),
                   candidates.empty() ? "" : " (" + hexJoin(candidates) + ")");
        return;
    }
    csp.postTypedEventToTask(candidates[0], 0x002B, tub, fmt::format("wsentry inferred terminal for unit {:02X}", unit));
}

// Report the recovered controller device scan against live guest storage.
// Read-only: it evaluates the guest lock byte, the QH50 walk, the per-DUB
// predicates and the queue search, and prints what each would decide.  It
// runs no state machine and posts nothing.
void MonitorCli::wsEntryScan(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("usage: wsscan\n");
        return;
    }
    m_.nativeControlStorage().wsEntryScanSurvey("wsscan", [](const std::string& line) { fmt::print("{}\n", line); });
}

// Walk both work-station unit-block chains without conflating their link
// fields.  The console chain link is at TU+0x50..0x52 (QH49) and the shared
// chain link at TU+0x4B..0x4D (QH50).  Read-only; every address is derived
// from low storage or a live TU.
void MonitorCli::tuTopology(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("usage: tutopology\n");
        return;
    }
    auto& st = m_.state;
    const int published = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    fmt::print("Workstation TU topology (read-only)\n");
    fmt::print("  SCADMTUB 092A..092C = {:06X}{}\n", published,
               published != 0 && canReadGuest(published, 2) && st.readHalf(published) == WorkStationIob::kUnitBlockEyecatcher
                   ? " (valid TU)" : published == 0 ? " (null)" : " (not a readable TU)");
    fmt::print("  configured listeners:\n");
    std::vector<WorkStationSlot*> displays;
    for (WorkStationSlot* slot : m_.devices().workStations().slots())
        if (!slot->isPrinter) displays.push_back(slot);
    std::stable_sort(displays.begin(), displays.end(),
                     [](const WorkStationSlot* x, const WorkStationSlot* y) { return x->unitAddress() < y->unitAddress(); });
    for (WorkStationSlot* slot : displays)
        fmt::print("    unit {:02X} {}.{}: configured={} attached={} recorded-TU={:06X}\n", slot->unitAddress(), slot->port, slot->address,
                   slot->configured() ? "yes" : "no", slot->backendAttached() ? "yes" : "no", slot->unitBlockAddress);

    printTuChain("QH49 console", GuestLowStorage::kConsoleUnitBlockQueue, 0x50, published);
    printTuChain("QH50 shared", GuestLowStorage::kSharedUnitBlockQueue, 0x4B, published);
}

void MonitorCli::printTuChain(const char* name, int queueNumber, int nextOffset, int published)
{
    auto& st = m_.state;
    const int header = GuestLowStorage::queueHeader(queueNumber);
    int node = st.readAddr24(header);
    fmt::print("  {}: header {:04X} head {:06X}, next TU+{:02X}..+{:02X}\n", name, header, node, nextOffset, nextOffset + 2);
    fmt::print("    #  TU      eye  unit class +75 +83 +91ptr +97ptr SCAD listener                 next\n");
    if (node == 0) {
        fmt::print("    (empty)\n");
        return;
    }

    std::set<int> seen;
    for (int ordinal = 0; ordinal < 4096; ordinal++) {
        if (!seen.insert(node).second) {
            fmt::print("    cycle -> {:06X} (already visited)\n", node);
            return;
        }
        if (!canReadGuest(node, 0x98)) {
            fmt::print("    {:2} {:06X} truncated/unreadable (need through TU+97)\n", ordinal, node);
            return;
        }

        const uint16_t eye = st.readHalf(node);
        const uint8_t unit = st.readByte(node + WorkStationIob::kOffUnitAddress);
        const uint8_t cls = st.readByte(node + WorkStationIob::kOffClass);
        const uint8_t f75 = st.readByte(node + 0x75);
        const uint8_t f83 = st.readByte(node + 0x83);
        const int p91 = st.readAddr24(node + UnitBlock::kOffActiveSessionPointer);
        const int p97 = st.readAddr24(node + UnitBlock::kOffCpetTablePointer);
        const int next = st.readAddr24(node + nextOffset);
        WorkStationSlot* slot = m_.devices().workStations().find(unit);
        std::string association;
        if (slot == nullptr || slot->isPrinter) association = "no listener";
        else if (slot->unitBlockAddress == node) association = fmt::format("{}.{} exact", slot->port, slot->address);
        else if (slot->unitBlockAddress == 0) association = fmt::format("{}.{} unlearned", slot->port, slot->address);
        else association = fmt::format("{}.{} recorded {:06X}", slot->port, slot->address, slot->unitBlockAddress);

        fmt::print("    {:2} {:06X} {:04X}  {:02X}   {:02X}   {:02X}  {:02X}  {:06X} {:06X} {:<4} {:<24} {:06X}\n", ordinal, node, eye, unit, cls,
                   f75, f83, p91, p97, node == published ? "yes" : "no", association, next);
        if (next == 0) {
            fmt::print("    end (null link after {} node(s))\n", ordinal + 1);
            return;
        }
        node = next;
    }
    fmt::print("    truncated: exceeded 4096-node guard\n");
}

// Decode the work-station task's TU/session predicate without asking the
// operator for a storage address.  The unit-block builder's operand 55
// names the RIGHTMOST byte, so its three-byte OC association physically
// occupies TU+53..55; QH50's link ending at +4D physically begins at +4B.
// The optional watch form arms the ordinary storage watch mechanism over
// the derived association and OC active byte.  It never changes guest
// state.
void MonitorCli::workstationSessionState(const std::vector<std::string>& a)
{
    const bool arm = a.size() == 2 && equalsIgnoreCase(a[1], "watch");
    const bool clear = a.size() == 2 && equalsIgnoreCase(a[1], "clear");
    if (a.size() > 2 || (a.size() == 2 && !arm && !clear)) {
        fmt::print("usage: wsstate [watch|clear]\n");
        return;
    }
    if (clear) {
        watch({"watch", "off"});
        return;
    }
    auto& st = m_.state;
    const int q49Field = GuestLowStorage::queueHeader(GuestLowStorage::kConsoleUnitBlockQueue);
    const int q50Field = GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue);
    const int q49 = st.readAddr24(q49Field);
    const int q50 = st.readAddr24(q50Field);
    const int publishedConsole = st.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    VirtualWorkstation* hostConsole = nullptr;
    for (auto& s : m_.stations())
        if (s->isConsole()) { hostConsole = s.get(); break; }
    fmt::print("Workstation session topology: QH49 head {:06X}, QH50 head {:06X} ({})\n", q49, q50,
               q49 == q50 ? "same head" : "different heads");
    fmt::print("  published console {:06X}; host console {} recorded TU {:06X}\n", publishedConsole,
               hostConsole == nullptr ? "-" : hostConsole->id(), hostConsole == nullptr ? 0 : hostConsole->tubAddress);
    fmt::print("  TU      unit station host                 recorded OC      key   W-id eye match q49 q53 del repl   +8E +0C active\n");

    std::set<int> seen;
    std::vector<int> unitZeroNodes, f7Nodes, f7WithoutOc;
    int count = 0;
    int tu = q50;
    bool cycle = false;
    const int watchCountBefore = st.watchCount();
    auto hexArg = [](int v) { return fmt::format("{:X}", v); };
    if (arm) {
        watch({"watch", hexArg(q49Field), "3"});
        watch({"watch", hexArg(q50Field), "3"});
    }
    for (; tu != 0 && count < 4096; count++) {
        if (!seen.insert(tu).second) { cycle = true; break; }
        if (!canReadGuest(tu, 0x92)) {
            fmt::print("  {:06X}  --   -       invalid/truncated QH50 node\n", tu);
            break;
        }

        const uint16_t eye = st.readHalf(tu);
        if (eye != WorkStationIob::kUnitBlockEyecatcher) {
            fmt::print("  {:06X}  --   -       non-TU eye {:04X}\n", tu, eye);
            tu = st.readAddr24(tu + 0x4B);
            continue;
        }

        const int unit = st.readByte(tu + 0x0C);
        const int port = WorkStationController::portOf(unit);
        const int address = WorkStationController::addressOf(unit);
        VirtualWorkstation* station = nullptr;
        for (auto& s : m_.stations())
            if (s->port() == port && s->address() == address) { station = s.get(); break; }
        WorkStationSlot* slot = m_.devices().workStations().find(unit);
        const std::string stationId = station == nullptr ? "-" : station->id();
        const std::string host = slot == nullptr ? "no-slot"
                                                 : fmt::format("{}/{}", slot->configured() ? "configured" : "unconfigured",
                                                               slot->backendAttached() ? "attached" : "detached");
        const int recorded = slot == nullptr ? 0 : slot->unitBlockAddress;

        // Physical fields: TU+4E..4F is the W-id/key and TU+53..55 is the
        // OC pointer.  The builder writes both from the same WSC record.
        const uint16_t tuKey = st.readHalf(tu + 0x4E);
        const int widNumber = (tuKey & 0xFF) - 0xF0;
        const bool widOk = (tuKey >> 8) == 0xE6 && widNumber >= 1 && widNumber <= 7;
        const std::string wid = widOk ? "W" + std::to_string(widNumber) : "--";
        const int oc = st.readAddr24(tu + 0x53);
        const bool ocReadable = canReadGuest(oc, 0x0F);
        const uint16_t ocEye = ocReadable ? st.readHalf(oc) : static_cast<uint16_t>(0);
        const uint16_t ocKey = ocReadable ? st.readHalf(oc + 2) : static_cast<uint16_t>(0);
        const bool eyeOk = ocEye == 0xD6C3;   // EBCDIC "OC"
        const bool keyOk = eyeOk && ocKey == tuKey;
        const bool onQ49 = occursOnQueue(tu, q49Field, 0x50);
        const bool onQ53 = eyeOk && occursOnQueue(oc, GuestLowStorage::queueHeader(53), 5);
        const uint8_t lifecycle = st.readByte(tu + 8);
        const int replacement = st.readAddr24(tu + 0x8F);
        const uint8_t function = st.readByte(tu + 0x8E);
        const uint8_t active = ocReadable ? st.readByte(oc + 0x0C) : static_cast<uint8_t>(0);
        const uint8_t mssc75 = st.readByte(tu + 0x75);
        const uint8_t mssc83 = st.readByte(tu + 0x83);
        // The command router saves this TU into low storage 092A..092C only
        // when +75 carries 90 and +83 bit 08 is clear.  Showing that exact
        // predicate explains a cleared SCADMTUB without searching a
        // transient module or guessing from the eyecatcher.
        const bool msscCandidate = (mssc75 & 0x90) == 0x90 && (mssc83 & 0x08) == 0;
        if (unit == 0) unitZeroNodes.push_back(tu);
        if (function == 0xF7) {
            f7Nodes.push_back(tu);
            if (oc == 0) f7WithoutOc.push_back(tu);
        }

        fmt::print("  {:06X}  {:02X}   {:<7} {:<21} {:06X} {:06X}  {:04X}  {:<3}  {}  {}   {}  {}  {}   {:06X} {:02X}  {:02X}  {}\n", tu, unit,
                   stationId, host, recorded, oc, tuKey, wid, eyeOk ? "OC" : (oc == 0 ? "--" : "BAD"), keyOk ? "yes" : "no ",
                   onQ49 ? "yes" : "no ", onQ53 ? "yes" : "no ", (lifecycle & 0x01) != 0 ? "yes" : "no ", replacement, function,
                   active, (active & 0x80) != 0 ? "ACTIVE" : "clear");
        fmt::print("          #MSSC scan: +75={:02X} +83={:02X} -> {} SCADMTUB candidate\n", mssc75, mssc83,
                   msscCandidate ? "save as" : "skip as");

        if (arm) {
            watch({"watch", hexArg(tu + 0x4B), "3"});
            watch({"watch", hexArg(tu + 0x50), "3"});
            watch({"watch", hexArg(tu + 0x53), "3"});
            watch({"watch", hexArg(tu + 8), "1"});
            // The router's console-return decision is made from these two
            // bytes, not from the OC/JCB association fields.  Keep them in
            // the symbolic topology watch so `wsstate watch` follows the
            // exact qualifying predicate for every discovered TU.
            watch({"watch", hexArg(tu + 0x75), "1"});
            watch({"watch", hexArg(tu + 0x83), "1"});
            watch({"watch", hexArg(tu + 0x8F), "3"});
            watch({"watch", hexArg(tu + 0x8E), "1"});
            if (ocReadable) watch({"watch", hexArg(oc + 0x0C), "1"});
        }

        tu = st.readAddr24(tu + 0x4B);
    }

    if (q50 == 0) fmt::print("  (empty)\n");
    else if (cycle || count >= 4096) fmt::print("  warning: QH50 walk stopped at its cycle/length guard\n");
    if (unitZeroNodes.size() > 1)
        fmt::print("  identity warning: {} QH50 TUs claim unit 00: {}\n", unitZeroNodes.size(), hexJoin(unitZeroNodes));
    if (!f7WithoutOc.empty())
        fmt::print("  classifier warning: F7 is already on {}, but its OC association is null; #CPTC advances at +11AA before the F7 handler\n",
                   hexJoin(f7WithoutOc));
    else if (!f7Nodes.empty())
        fmt::print("  classifier: F7 node(s) with a non-null OC: {}\n", hexJoin(f7Nodes));
    if (arm)
        fmt::print("symbolic TU association/OC active-byte watches armed ({} derived, {} total); use `wsstate clear` to clear\n",
                   st.watchCount() - watchCountBefore, st.watchCount());
}

bool MonitorCli::occursOnQueue(int wanted, int headerField, int nextField)
{
    auto& st = m_.state;
    int at = st.readAddr24(headerField);
    std::set<int> seen;
    for (int guard = 0; at != 0 && guard++ < 4096 && seen.insert(at).second;) {
        if (at == wanted) return true;
        if (!canReadGuest(at, nextField + 3)) return false;
        at = st.readAddr24(at + nextField);
    }
    return false;
}

// Read-only view of the register/config contract at the IPL's unit-block
// replacement call and the builder's clone decisions.  The active-member
// attribution is essential: most SSP transients alias logical 1000, so
// register values at a bare IAR are otherwise not evidence that these
// fields have the meanings printed here.
void MonitorCli::svtubDecisionState(const std::vector<std::string>& a)
{
    if (a.size() != 1) { fmt::print("usage: svtubstate\n"); return; }
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    const int old = st.msp.xr1;
    const int cfg = st.msp.xr2;
    const int tb = csp.currentTaskBlock();
    const int rb = csp.currentRequestBlock();
    const std::string where = csp.describeActiveMember(tb, st.msp.iar);
    fmt::print("SVTUB decision: task {:06X}, rb {:06X}, IAR {:04X}{}\n", tb, rb, st.msp.iar,
               where.empty() ? std::string(" (member not attributed)") : " = " + where);
    fmt::print("  XR1 old {:06X}, XR2 cfg {:06X}\n", old, cfg);
    if (!canReadGuest(cfg, 21)) {
        fmt::print("  config is null/invalid\n");
        return;
    }
    const bool oldReadable = canReadGuest(old, 0x84);
    const uint16_t eye = canReadGuest(old, 2) ? st.readHalf(old) : static_cast<uint16_t>(0);
    const uint8_t c0 = st.readByte(cfg), c3 = st.readByte(cfg + 3);
    const uint8_t c13 = st.readByte(cfg + 13), c20 = st.readByte(cfg + 20);
    const uint16_t station = st.readHalf(cfg + 1);
    if (oldReadable)
        fmt::print("  old eye {:04X}, +75={:02X}, +83={:02X}; cfg +0={:02X} id={:04X} +3={:02X} +13={:02X} +20={:02X}\n", eye,
                   st.readByte(old + 0x75), st.readByte(old + 0x83), c0, station, c3, c13, c20);
    else
        fmt::print("  old eye {:04X}, +75=--, +83=--; cfg +0={:02X} id={:04X} +3={:02X} +13={:02X} +20={:02X}\n", eye, c0, station, c3,
                   c13, c20);

    const bool rebuild = (c20 & 0x01) != 0;
    const bool terminal = c3 == 0xC0;
    const bool cloneKind = (c0 & 0x01) != 0;
    const bool cloneRequested = (c13 & 0x02) != 0;
    const bool cloneSuppressed = (c13 & 0x01) != 0;
    fmt::print("  exact clone gates: +20.01={} [1573], +3=C0={} [157A], +0.01={} [1581], +13.02={} [159D], +13.01 clear={} [15AF]\n",
               rebuild ? "yes" : "no", terminal ? "yes" : "no", cloneKind ? "yes" : "no", cloneRequested ? "yes" : "no",
               !cloneSuppressed ? "yes" : "no");
    const char* decision = !rebuild ? "first failure +20.01 clear: 1576 exits the rebuild tail (1054 takes the ordinary build path)"
                           : !terminal ? "first failure +3 != C0: printer path exits before cloning"
                           : !cloneKind ? "first failure +0.01 clear: 1581 rejects the clone"
                           : !cloneRequested ? "first failure +13.02 clear: 15A3 exits before cloning"
                           : cloneSuppressed ? "first failure +13.01 set: 15B2 diverts around the clone body"
                                             : "all five pass: clone body at 15B6 is eligible";
    fmt::print("  clone decision: {}\n", decision);
    fmt::print("  cfg+13.02 is also tested by the #MSIPL 12BE caller before its second #SVTUB call\n");
}

}  // namespace sim36::monitor
