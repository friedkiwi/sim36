#include "Monitor/MonitorCli.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

#include <fmt/format.h>

#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Storage/Ebcdic.h"
#include "Storage/FileNotFoundError.h"
#include "Storage/Library.h"

namespace sim36::monitor {

using storage::DiskBackend;
using storage::Vtoc;

namespace {

// Convert.ToInt32(s, 16): optional 0x prefix, hex digits only.
int parseHex32(const std::string& s)
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

long long parseDecimal(const std::string& s)
{
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    std::size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i == s.size()) throw MonitorError("Input string was not in a correct format.");
    for (std::size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k])))
            throw MonitorError("Input string was not in a correct format.");
    return std::strtoll(s.c_str(), nullptr, 10);
}

int parseInt(const std::string& s)
{
    long long v = parseDecimal(s);
    if (v > 2147483647LL || v < -2147483648LL)
        throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

const char* boolText(bool v) { return v ? "True" : "False"; }

}  // namespace

std::string MonitorCli::hexDump(const uint8_t* b, int len, int baseAddr)
{
    std::string out;
    for (int i = 0; i < len; i += 16) {
        const int n = std::min(16, len - i);
        std::string hex;
        for (int j = 0; j < n; ++j) hex += fmt::format("{:02x} ", b[i + j]);
        std::string txt;
        for (int j = 0; j < n; ++j) {
            const uint16_t cp = storage::Ebcdic::toUnicode(b[i + j]);
            txt.push_back(cp >= ' ' && cp < 127 ? static_cast<char>(cp) : '.');
        }
        out += fmt::format("{:06x}  {:<48} |{}|\n", baseAddr + i, hex, txt);
    }
    return out;
}

void MonitorCli::executeTokens(const std::vector<std::string>& a)
{
    if (a.empty()) return;
    const std::string verb = toLower(a[0]);
    if (verb == "show") show(a);
    else if (verb == "vtoc") showVtoc(a);
    else if (verb == "lib") showLibrary(a);
    else if (verb == "dump") dump(a);
    else if (verb == "sector") dumpSector(a);
    else if (verb == "set") setRegister(a);
    else if (verb == "trace") setTrace(a);
    else if (verb == "boot") boot();
    else if (verb == "ipl") ipl(a);
    else if (verb == "load") load(a);
    else if (verb == "loadfile") loadFile(a);
    else if (verb == "diskread") diskRead(a);
    else if (verb == "step" || verb == "dis" || verb == "selftest" || verb == "break" || verb == "breakm" ||
             verb == "addrmap" || verb == "findmem" || verb == "poke" || verb == "watch")
        throw MonitorError("'" + a[0] + "' is not ported yet (milestone 3)");
    else if (verb == "start" || verb == "stop" || verb == "wait" || verb == "sched" || verb == "ace" ||
             verb == "iob" || verb == "tu" || verb == "conformance" || verb == "timers" || verb == "actions" ||
             verb == "tasklist" || verb == "mapstate" || verb == "sqsstate" || verb == "residency" ||
             verb == "modules" || verb == "modstorage" || verb == "allocchain" || verb == "whereis" ||
             verb == "patch" || verb == "stations" || verb == "listener-auto-signon" || verb == "xferid" ||
             verb == "xferterm" || verb == "nuptermscan" || verb == "smf" || verb == "wddqstate")
        throw MonitorError("'" + a[0] + "' is not ported yet (milestone 4 or 5)");
    else if (verb == "diskette" || verb == "tape" || verb == "dsktread" || verb == "dsktwrite" ||
             verb == "tapetest" || verb == "tapesvc" || verb == "savemain")
        throw MonitorError("'" + a[0] + "' is not ported yet (milestone 7)");
    else
        throw MonitorError("'" + a[0] + "' is not ported yet (milestone 6)");
}

void MonitorCli::show(const std::vector<std::string>& a)
{
    const std::string what = a.size() > 1 ? toLower(a[1]) : "cpu";
    if (what == "workstation") {
        throw MonitorError("'show workstation' is not ported yet (milestone 6)");
    } else if (what == "cpu") {
        const machine::MspRegisters& r = m_.state.msp;
        fmt::print("IAR {:04X}  ARR {:04X}  XR1 {:04X}  XR2 {:04X}  PSR {:02X}\n", r.iar, r.arr, r.xr1, r.xr2, r.psr());
        fmt::print("PMR {:02X}  CMR {:02X}  translated={}\n", r.pmr(), r.cmr, boolText(r.translatedAddressing()));
        // The prefixes are part of the addresses above, not decoration.
        fmt::print("PACT iar {:02X}  dir {:02X}  xr1 {:02X}  xr2 {:02X}   WR4 {:04X} WR5 {:04X} WR6 {:04X} WR7 {:04X}\n",
                   r.pactIar, r.pactDir, r.pactXr1, r.pactXr2, r.wr[4], r.wr[5], r.wr[6], r.wr[7]);
        fmt::print("stopped={}  instructions={}  preemptable={}\n", boolText(m_.msp().stopped()),
                   m_.msp().instructionsExecuted(), boolText(m_.msp().atPreemptionPoint()));
        if (m_.msp().stopped() && !m_.msp().stopReason().empty()) fmt::print("reason: {}\n", m_.msp().stopReason());
    } else if (what == "storage") {
        fmt::print("main storage {} KB ({} bytes)\n", m_.state.installedMainStorageBytes() / 1024,
                   m_.state.installedMainStorageBytes());
        const DiskBackend& d = m_.diskBackend();
        fmt::print("volume {}: {} sectors, {} blocks, {}\n", d.path(), d.sectorCount(), d.blockCount(),
                   d.readOnly() ? "read-only" : "writable");
    } else if (what == "atr") {
        // The task's 32 address translation registers.  FFFF is "not mapped".
        for (int i = 0; i < 32; i += 8) {
            std::string line = fmt::format("ATR {:>2}:", i);
            for (int j = 0; j < 8; ++j) line += fmt::format(" {:04X}", m_.state.atr[machine::MachineState::kAtrTaskGroup0 + i + j]);
            fmt::print("{}\n", line);
        }
    } else if (what == "ptt") {
        throw MonitorError("'show ptt' is not ported yet (milestone 5)");
    } else if (what == "csp") {
        fmt::print("model {}\n", m_.controlStorage().modelName());
        fmt::print("transient area busy={} queue={}\n", boolText(m_.controlStorage().transients().busy()),
                   m_.controlStorage().transients().queueDepth());
    } else {
        fmt::print("show what? config|status|terminal|cpu|storage|csp|atr|ptt|workstation\n");
    }
}

void MonitorCli::showVtoc(const std::vector<std::string>& a)
{
    if (!m_.vtocsRead()) m_.readVtocs();
    const std::string which = a.size() > 1 ? toLower(a[1]) : "both";
    if (which == "system" || which == "both") {
        fmt::print("-- system-area VTOC (block {}) --\n", Vtoc::systemBlockOf(&m_.diskBackend()));
        for (const auto& e : m_.systemVtoc()) fmt::print("  {}\n", e.toString());
    }
    if (which == "user" || which == "both") {
        fmt::print("-- user VTOC (block {}), {} entries --\n",
                   Vtoc::systemBlockOf(&m_.diskBackend()) + (Vtoc::kUserVtocBlock - Vtoc::kSystemVtocBlock),
                   m_.userVtoc().size());
        std::vector<storage::VtocEntry> sorted = m_.userVtoc();
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const storage::VtocEntry& x, const storage::VtocEntry& y) { return x.extentSector < y.extentSector; });
        for (const auto& e : sorted) fmt::print("  {}\n", e.toString());
    }
}

void MonitorCli::showLibrary(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("lib <name> [count]\n"); return; }
    if (!m_.vtocsRead()) m_.readVtocs();
    const storage::VtocEntry* lib = m_.find(a[1]);
    if (lib == nullptr) { fmt::print("no such entry\n"); return; }
    if (lib->type != storage::VtocEntryType::Library) {
        fmt::print("{} is a {}\n", lib->name, storage::vtocEntryTypeName(lib->type));
        return;
    }
    const int count = a.size() > 2 ? parseInt(a[2]) : 20;
    auto members = storage::LibraryDirectory::read(m_.diskBackend(), *lib, 64);
    fmt::print("{}: extent {}, {} sectors, {} members found in the first 64 directory sectors\n",
               lib->name, lib->extentSector, lib->allocatedSectors, members.size());
    int shown = 0;
    for (const auto& mm : members) {
        if (shown++ >= count) break;
        fmt::print("  {} {:<9} sector {:<7} {:>3} sectors  link {:04X}  attr {:02X}\n",
                   mm.kind, mm.name, mm.relativeSector, mm.sectors, mm.linkAddress, mm.attributes);
    }
}

void MonitorCli::dumpSector(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("sector <n> [count]\n"); return; }
    const long long s = parseDecimal(a[1]);
    const int n = a.size() > 2 ? parseInt(a[2]) : 1;
    for (long long i = 0; i < n; ++i) {
        uint8_t b[DiskBackend::kSectorBytes];
        if (!m_.diskBackend().readSector(s + i, b))
            throw MonitorError(fmt::format("outside the volume\nParameter name: sector\nActual value was {}.", s + i));
        fmt::print("-- sector {} --\n", s + i);
        fmt::print("{}", hexDump(b, DiskBackend::kSectorBytes, 0));
    }
}

void MonitorCli::dump(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("dump <addr|xr1|xr2> [len]\n"); return; }
    int addr;
    if (equalsIgnoreCase(a[1], "xr1")) addr = m_.state.msp.xr1;
    else if (equalsIgnoreCase(a[1], "xr2")) addr = m_.state.msp.xr2;
    else addr = parseHex32(a[1]);
    const int len = a.size() > 2 ? parseHex32(a[2]) : 64;
    if (len < 0 || !m_.state.inRange(addr, len))
        throw MonitorError(fmt::format("guest access {:06X}+{} outside {:06X} bytes of main storage",
                                       addr, len, m_.state.backingBytes()));
    std::vector<uint8_t> b(static_cast<std::size_t>(len));
    m_.state.read(addr, b.data(), len);
    fmt::print("{}", hexDump(b.data(), len, addr));
}

void MonitorCli::setRegister(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("set <reg> <hex>\n"); return; }
    const int parsed = parseHex32(a[2]);
    if (parsed < 0 || parsed > 0xFFFF) throw MonitorError("Value was either too large or too small for a UInt16.");
    const uint16_t v = static_cast<uint16_t>(parsed);
    machine::MspRegisters& r = m_.state.msp;
    const std::string reg = toLower(a[1]);
    if (reg == "iar") r.iar = v;
    else if (reg == "xr1") r.xr1 = v;
    else if (reg == "xr2") r.xr2 = v;
    else if (reg == "arr") r.arr = v;
    else if (reg == "psr") r.loadPsr(static_cast<uint8_t>(v));
    // The PACT prefixes are the top third of an index register and the mode
    // selector for the path it addresses through.
    else if (reg == "pxr1") r.pactXr1 = static_cast<uint8_t>(v);
    else if (reg == "pxr2") r.pactXr2 = static_cast<uint8_t>(v);
    else if (reg == "pdir") r.pactDir = static_cast<uint8_t>(v);
    else if (reg == "piar") r.pactIar = static_cast<uint8_t>(v);
    else if (reg == "wr4" || reg == "wr5" || reg == "wr6" || reg == "wr7") r.wr[reg[2] - '0'] = v;
    else { fmt::print("unknown register\n"); return; }
    fmt::print("{} = {:04X}\n", a[1], v);
}

void MonitorCli::setTrace(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("trace is {}\n", traceFlagsToString(m_.trace.flags)); return; }
    if (equalsIgnoreCase(a[1], "workstation")) throw MonitorError("'trace workstation' is not ported yet (milestone 6)");
    if (equalsIgnoreCase(a[1], "member")) throw MonitorError("'trace member' is not ported yet (milestone 3)");
    if (equalsIgnoreCase(a[1], "off")) {
        m_.trace.flags = TraceNone;
    } else {
        std::string joined;
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (i != 1) joined += ",";
            joined += a[i];
        }
        try {
            m_.trace.flags = parseTraceFlags(joined);
        } catch (const std::invalid_argument& e) {
            throw MonitorError(e.what());
        }
    }
    fmt::print("trace = {}\n", traceFlagsToString(m_.trace.flags));
}

void MonitorCli::ipl(const std::vector<std::string>& a)
{
    if (a.size() > 2) throw MonitorError("usage: ipl [pause]");
    const bool pause = a.size() == 2 && equalsIgnoreCase(a[1], "pause");
    if (a.size() == 2 && !pause)
        throw MonitorError("usage: ipl [pause] (IPL is non-blocking; use 'wait idle [seconds]' to synchronize)");
    m_.reset();
    if (!m_.vtocsRead()) m_.readVtocs();
    // No machine-security clause here: the emulator writes no sign-on mode
    // to the guest, so the only true statement is which volume was attached.
    fmt::print("IPL started: {}, disk {}, console W1 (monitor)\n", m_.config.model,
               std::filesystem::path(m_.config.volumePath).filename().string());
    if (pause) {
        fmt::print("IPL paused before instruction 1; use 'step N' or 'start'\n");
        return;
    }
    throw MonitorError("start: continuous execution is not ported yet (milestone 4)");
}

void MonitorCli::boot()
{
    m_.reset();
    if (!m_.vtocsRead()) m_.readVtocs();
    // The boot record is the first sector of #SYSWORK, immediately after
    // VOL1.  It names the phase 1 and phase 2 members and the library.
    const storage::VtocEntry* sysWork = m_.find("SYSWORK");
    if (sysWork == nullptr) { fmt::print("no #SYSWORK in the system VTOC\n"); return; }
    uint8_t rec[DiskBackend::kSectorBytes];
    if (!m_.diskBackend().readSector(sysWork->extentSector, rec))
        throw MonitorError(fmt::format("outside the volume\nParameter name: sector\nActual value was {}.", sysWork->extentSector));
    fmt::print("boot record at sector {}:\n", sysWork->extentSector);
    fmt::print("{}", hexDump(rec, 128, 0));
    const storage::VtocEntry* lib = m_.find("LIBRARY");
    if (lib != nullptr) fmt::print("#LIBRARY extent {}, {} sectors\n", lib->extentSector, lib->allocatedSectors);
    fmt::print("(phase 1 execution is not implemented - see docs/s36/gap-analysis.md)\n");
}

// Load a type-O member at its link address.  Member sectors in a library
// directory are relative to the library extent.
void MonitorCli::load(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("load <library> <member>\n"); return; }
    if (!m_.vtocsRead()) m_.readVtocs();
    const storage::VtocEntry* lib = m_.find(a[1]);
    if (lib == nullptr || lib->type != storage::VtocEntryType::Library) { fmt::print("no such library\n"); return; }
    auto members = storage::LibraryDirectory::read(m_.diskBackend(), *lib, 512);
    std::string want = a[2];
    for (char& c : want) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const storage::LibraryMember* mem = nullptr;
    for (const auto& mm : members) {
        std::string n = mm.name;
        for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n == want) { mem = &mm; break; }
    }
    if (mem == nullptr) { fmt::print("no member {} in {}\n", want, lib->name); return; }
    if (mem->sectors <= 0 || mem->relativeSector <= 0) {
        fmt::print("{}: directory entry carries no usable extent - the member "
                   "sector and length fields are not decoded yet "
                   "(docs/s36/library-format.md)\n", mem->name);
        return;
    }
    const int at = mem->linkAddress;
    std::vector<uint8_t> buf(static_cast<std::size_t>(mem->sectors) * DiskBackend::kSectorBytes);
    for (int i = 0; i < mem->sectors; ++i)
        m_.diskBackend().readSector(mem->absoluteSector(*lib) + i, buf.data() + static_cast<std::size_t>(i) * DiskBackend::kSectorBytes);
    const int n = std::min(static_cast<int>(buf.size()), m_.state.backingBytes() - at);
    m_.state.write(at, buf.data(), n);
    m_.state.msp.iar = static_cast<uint16_t>(at);
    fmt::print("loaded {} ({} sectors) at {:04X}; IAR set\n", mem->name, mem->sectors, at);
}

// Load raw bytes, so a decode run can be diffed over exactly the same input.
void MonitorCli::loadFile(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("loadfile <path> <hexaddr>\n"); return; }
    std::ifstream in(a[1], std::ios::binary);
    if (!in) {
        std::error_code ec;
        std::filesystem::path full = std::filesystem::absolute(a[1], ec);
        throw storage::FileNotFoundError(ec ? a[1] : full.lexically_normal().string());
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const int at = parseHex32(a[2]);
    const int n = std::min(static_cast<int>(b.size()), m_.state.backingBytes() - at);
    if (n < 0 || !m_.state.inRange(at, n))
        throw MonitorError(fmt::format("guest access {:06X}+{} outside {:06X} bytes of main storage",
                                       at, n, m_.state.backingBytes()));
    m_.state.write(at, b.data(), n);
    fmt::print("loaded {} bytes at {:04X}\n", b.size(), at);
}

void MonitorCli::diskRead(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("diskread <sector> [count]\n"); return; }
    // The SVC 40 path through the control storage processor - the ACE, the
    // device dispatch and the completion post - is milestone 4.
    throw MonitorError("'diskread' is not ported yet (milestone 4): it issues SVC 40 through the control storage processor");
}

}  // namespace sim36::monitor
