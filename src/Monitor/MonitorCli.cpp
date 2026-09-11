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
    else if (verb == "dis") disassemble(a);
    else if (verb == "step") step(a);
    else if (verb == "break") breakCommand(a);
    else if (verb == "watch") watch(a);
    else if (verb == "poke") poke(a);
    else if (verb == "patch") patch(a);
    else if (verb == "findmem") findMemory(a);
    else if (verb == "addrmap") addressMap(a);
    else if (verb == "selftest") selfTest();
    else if (verb == "ace") showAce(a);
    else if (verb == "iob") showIob(a);
    else if (verb == "tu") showUnitBlock(a);
    else if (verb == "sched") sched();
    else if (verb == "conformance") conformance();
    else if (verb == "breakm")
        throw MonitorError("'breakm' is not ported yet (milestone 5): it resolves members through the loader");
    else if (verb == "start" || verb == "stop" || verb == "wait" || verb == "timers" || verb == "actions" ||
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
    // `trace member <name>` restricts the full instruction trace to one module
    // past the load-0x1000 aliasing; `trace member off` clears it.
    if (equalsIgnoreCase(a[1], "member")) {
        if (a.size() > 2 && !equalsIgnoreCase(a[2], "off")) {
            std::string name = a[2];
            while (!name.empty() && name[0] == '#') name.erase(name.begin());
            for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            m_.msp().traceMemberFilter = name;
            fmt::print("instruction trace restricted to member matching \"{}\"\n", name);
        } else {
            m_.msp().traceMemberFilter.clear();
            fmt::print("instruction trace member filter cleared\n");
        }
        return;
    }
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
    // The reference runs the IPL on a driver thread and returns to the
    // prompt at once; the driver (and `wait idle`) is milestone 6.  Until
    // then the machine is driven in the foreground until it stops, which
    // yields the same trace and the same stop, printed before the prompt
    // instead of after it.
    driveMachine(1LL << 40);
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
    if (!in) throw storage::FileNotFoundError::forPath(a[1]);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const int at = parseHex32(a[2]);
    const int n = std::min(static_cast<int>(b.size()), m_.state.backingBytes() - at);
    if (n < 0 || !m_.state.inRange(at, n))
        throw MonitorError(fmt::format("guest access {:06X}+{} outside {:06X} bytes of main storage",
                                       at, n, m_.state.backingBytes()));
    m_.state.write(at, b.data(), n);
    m_.state.msp.iar = static_cast<uint16_t>(at);
    fmt::print("loaded {} bytes at {:04X}\n", b.size(), at);
}

void MonitorCli::disassemble(const std::vector<std::string>& a)
{
    int addr = a.size() > 1 ? parseHex32(a[1]) : m_.state.msp.iar;
    const int count = a.size() > 2 ? parseInt(a[2]) : 16;
    for (int i = 0; i < count; ++i) {
        const processors::Instruction insn = m_.msp().decode(addr);
        if (m_.state.faulted())
            throw MonitorError(m_.state.faultMessage());
        std::string hex;
        for (uint8_t b : insn.raw) hex += fmt::format("{:02x}", b);
        fmt::print("{:04x}  {:<14} {}\n", addr, hex, insn.toString());
        addr += insn.length;
    }
}

long long MonitorCli::driveMachine(long long cap)
{
    // The host-event pump at preemption points and the driver-idle park are
    // milestones 4 and 6; until then this is the bounded instruction loop.
    long long total = 0;
    while (total < cap && m_.msp().step()) ++total;
    return total;
}

void MonitorCli::step(const std::vector<std::string>& a)
{
    if (a.size() > 2) throw MonitorError("usage: step [instructions]");
    long long n = 1;
    if (a.size() > 1) {
        const std::string& s = a[1];
        const bool hex = s.size() >= 2 && s[0] == '0' && s[1] == 'x';
        std::string digits = hex ? s.substr(2) : s;
        if (digits.empty()) throw MonitorError("Could not find any recognizable digits.");
        for (char c : digits)
            if (!(hex ? std::isxdigit(static_cast<unsigned char>(c)) : std::isdigit(static_cast<unsigned char>(c)) || c == '-'))
                throw MonitorError("Could not find any recognizable digits.");
        n = std::strtoll(digits.c_str(), nullptr, hex ? 16 : 10);
    }
    if (n < 1) throw MonitorError("step count must be positive");
    // A fault or refused SVC deliberately sets the processor stop latch;
    // stepping is the bounded way to continue after inspecting that state.
    m_.msp().start();
    const long long total = driveMachine(n);
    if (m_.msp().stopped())
        fmt::print("stopped after {} instruction(s): {}\n", total,
                   m_.msp().stopReason().empty() ? "stopped" : m_.msp().stopReason());
    else
        fmt::print("stepped {} instruction(s): limit reached\n", total);
}

// Execution breakpoints: `break <hexaddr> [name]`, `break list`, `break
// clear [addr]`, `break member <name|off>`.  The MSP halts when the guest
// IAR reaches the address, before that instruction runs.
void MonitorCli::breakCommand(const std::vector<std::string>& a)
{
    if (a.size() < 2) {
        fmt::print("break <hexaddr> [name] | break list | break clear [hexaddr]\n");
        return;
    }
    const std::string sub = toLower(a[1]);
    if (sub == "list") {
        const auto& bps = m_.msp().breakpoints();
        if (bps.empty()) { fmt::print("no breakpoints\n"); return; }
        fmt::print("{} breakpoint(s):\n", bps.size());
        for (const auto& bp : bps) {
            // The member at that IAR for the current task is named once the
            // loader (milestone 5) can attribute it.
            fmt::print("  {:04X}{}\n", bp.first, bp.second.empty() ? "" : "  " + bp.second);
        }
        return;
    }
    if (sub == "member") {
        if (a.size() > 2 && toLower(a[2]) != "off") {
            std::string name = a[2];
            while (!name.empty() && name[0] == '#') name.erase(name.begin());
            for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            m_.msp().breakMemberFilter = name;
            fmt::print("breakpoints now fire only in a member matching \"{}\"\n", name);
        } else {
            m_.msp().breakMemberFilter.clear();
            fmt::print("breakpoint member filter cleared\n");
        }
        return;
    }
    if (sub == "clear") {
        if (a.size() > 2) {
            const int at = parseHex32(a[2]);
            fmt::print("{}\n", m_.msp().clearBreakpoint(static_cast<uint16_t>(at))
                                   ? fmt::format("cleared breakpoint at {:04X}", at)
                                   : fmt::format("no breakpoint at {:04X}", at));
        } else {
            m_.msp().clearAllBreakpoints();
            fmt::print("all breakpoints cleared\n");
        }
        return;
    }
    const int addr = parseHex32(a[1]);
    if (addr < 0 || addr > 0xFFFF) {
        fmt::print("break: address must be a 16-bit guest IAR (0000-FFFF)\n");
        return;
    }
    std::string name;
    for (std::size_t i = 2; i < a.size(); ++i) {
        if (i != 2) name += " ";
        name += a[i];
    }
    m_.msp().setBreakpoint(static_cast<uint16_t>(addr), name);
    fmt::print("breakpoint set at {:04X}{}\n", addr, name.empty() ? "" : " (" + name + ")");
}

// Watch guest storage: every write to the range is reported with the value
// before and after and the instruction address that did it.  A watchpoint
// sees the write however it was addressed.
void MonitorCli::watch(const std::vector<std::string>& a)
{
    if (a.size() >= 2 && a[1] == "off") {
        m_.state.clearWatches();
        m_.state.onWatchWrite = nullptr;
        fmt::print("watchpoints cleared\n");
        return;
    }
    if (a.size() < 2) { fmt::print("watch <addr> [len] | off\n"); return; }
    const int at = parseHex32(a[1]);
    const int len = a.size() > 2 ? parseHex32(a[2]) : 1;
    m_.state.addWatch(at, at + len - 1);
    installWatchReporter();
    fmt::print("watching {:04X}..{:04X} ({} watchpoint(s))\n", at, at + len - 1, m_.state.watchCount());
}

void MonitorCli::installWatchReporter()
{
    m_.state.onWatchWrite = [this](int addr, int n, const std::vector<uint8_t>& before,
                                   const std::vector<uint8_t>& after) {
        auto hex = [](const std::vector<uint8_t>& v) {
            std::string s;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i != 0) s += " ";
                s += fmt::format("{:02X}", v[i]);
            }
            return s;
        };
        // The writing instruction is attributed to a member once the loader
        // (milestone 5) can name the one running at the aliased 0x1000.
        const std::string where = m_.msp().memberResolver ? m_.msp().memberResolver() : std::string();
        fmt::print("watch {:04X}..{:04X} <- {} (was {})  at IAR {:04X}{}\n", addr, addr + n - 1, hex(after),
                   hex(before), m_.state.msp.iar, where.empty() ? "" : "  in " + where);
    };
}

bool MonitorCli::parseHexBytes(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out)
{
    std::string text;
    for (std::size_t i = from; i < a.size(); ++i) text += a[i];
    if (text.size() % 2 != 0) { fmt::print("hex needs an even number of digits\n"); return false; }
    out.clear();
    for (std::size_t i = 0; i < text.size(); i += 2) {
        for (int k = 0; k < 2; ++k)
            if (!std::isxdigit(static_cast<unsigned char>(text[i + k])))
                throw MonitorError("Could not find any recognizable digits.");
        out.push_back(static_cast<uint8_t>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
    }
    return true;
}

void MonitorCli::poke(const std::vector<std::string>& a)
{
    if (a.size() < 3) { fmt::print("poke <hexaddr> <hexbyte...>\n"); return; }
    const int at = parseHex32(a[1]);
    std::vector<uint8_t> b;
    if (!parseHexBytes(a, 2, b)) return;
    for (std::size_t i = 0; i < b.size(); ++i) {
        m_.state.writeByte(at + static_cast<int>(i), b[i]);
        if (m_.state.faulted()) throw MonitorError(m_.state.faultMessage());
    }
    fmt::print("poked {} byte(s) at {:04X}\n", b.size(), at);
}

// Patch-on-reach: force a value at a specific instruction and CONTINUE.
void MonitorCli::patch(const std::vector<std::string>& a)
{
    if (a.size() < 2 || a[1] == "list") {
        bool any = false;
        for (const auto& kv : m_.msp().patchList())
            for (const auto& p : kv.second) {
                fmt::print("patch @{:04X}: kind {} target {:04X} := {:04X}\n", kv.first, p.kind, p.a, p.v);
                any = true;
            }
        if (!any) fmt::print("no patches\n");
        return;
    }
    if (a[1] == "off") { m_.msp().clearPatches(); fmt::print("patches cleared\n"); return; }
    if (a.size() < 4) {
        fmt::print("usage: patch <iar> mem|mem16 <addr> <val> | patch <iar> wr <n> <val> | "
                   "patch <iar> xr1|xr2 <val> | patch list|off\n");
        return;
    }
    const int iar = parseHex32(a[1]);
    const std::string kind = toLower(a[2]);
    processors::MainStorageProcessor::IarPatch pp;
    auto arg = [&](std::size_t i) {
        if (i >= a.size()) throw MonitorError("Index was outside the bounds of the array.");
        return parseHex32(a[i]);
    };
    if (kind == "mem") { pp.kind = 0; pp.a = arg(3); pp.v = arg(4); }
    else if (kind == "mem16") { pp.kind = 1; pp.a = arg(3); pp.v = arg(4); }
    else if (kind == "wr") { pp.kind = 2; pp.a = arg(3); pp.v = arg(4); }
    else if (kind == "xr1") { pp.kind = 3; pp.v = arg(3); }
    else if (kind == "xr2") { pp.kind = 4; pp.v = arg(3); }
    else { fmt::print("unknown patch kind '{}'\n", kind); return; }
    m_.msp().addPatch(static_cast<uint16_t>(iar), pp);
    fmt::print("patch armed at IAR {:04X} ({})\n", iar, kind);
}

// Read-only byte-pattern search over real guest storage.
void MonitorCli::findMemory(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("findmem <hexbytes>\n"); return; }
    std::vector<uint8_t> pattern;
    if (!parseHexBytes(a, 1, pattern) || pattern.empty()) return;
    const uint8_t* storage = m_.state.raw();
    const int size = m_.state.backingBytes();
    int found = 0;
    for (int at = 0; at <= size - static_cast<int>(pattern.size()); ++at) {
        std::size_t i = 0;
        while (i < pattern.size() && storage[at + static_cast<int>(i)] == pattern[i]) ++i;
        if (i != pattern.size()) continue;
        fmt::print("findmem {:06X}\n", at);
        ++found;
    }
    fmt::print("findmem: {} match(es) for {} byte(s)\n", found, pattern.size());
}

// Explain one MSP address-path calculation without reading or changing
// guest storage: the 16-bit wrap in the register, then the PACT prefix's
// choice of translated or real addressing.
void MonitorCli::addressMap(const std::vector<std::string>& a)
{
    if (a.size() < 2 || a.size() > 4) {
        fmt::print("addrmap <direct|xr1|xr2|iar> [hex-disp] [read|write]\n");
        return;
    }
    const std::string path = toLower(a[1]);
    const machine::MspRegisters& r = m_.state.msp;
    uint16_t basis;
    uint8_t pact;
    if (path == "direct") { basis = 0; pact = r.pactDir; }
    else if (path == "xr1") { basis = r.xr1; pact = r.pactXr1; }
    else if (path == "xr2") { basis = r.xr2; pact = r.pactXr2; }
    else if (path == "iar") { basis = r.iar; pact = r.pactIar; }
    else { fmt::print("addrmap: path must be direct, xr1, xr2, or iar\n"); return; }

    int displacement = 0;
    bool forWrite = false;
    for (std::size_t i = 2; i < a.size(); ++i) {
        if (equalsIgnoreCase(a[i], "read")) forWrite = false;
        else if (equalsIgnoreCase(a[i], "write")) forWrite = true;
        else displacement = parseHex32(a[i]);
    }
    if (displacement < 0 || displacement > 0xFFFF) {
        fmt::print("addrmap: displacement must be 0000-FFFF\n");
        return;
    }
    const int sum = basis + displacement;
    const uint16_t logical = static_cast<uint16_t>(sum);
    const bool wrapped = sum > 0xFFFF;
    const char* operation = forWrite ? "write" : "read";
    fmt::print("addrmap {}: {:02X}:{:04X} + {:04X} -> {:02X}:{:04X}{} ({})\n", path, pact, basis, displacement, pact,
               logical, wrapped ? " [16-bit wrap]" : "", operation);

    using machine::MachineState;
    if ((pact & machine::MspRegisters::kPactPmrBit) == 0) {
        const int real = ((pact & MachineState::kPactAddressBits) << 16) | logical;
        fmt::print("  untranslated: PACT address nibble {:X} -> real {:06X}{}\n", pact & MachineState::kPactAddressBits, real,
                   (pact & MachineState::kPactFlagBits) != 0
                       ? fmt::format("; flag bits {:02X} are not address", pact & MachineState::kPactFlagBits) : "");
        return;
    }
    const int page = logical >> MachineState::kPageShift;
    const uint16_t atr = m_.state.atr[MachineState::kAtrTaskGroup0 + page];
    fmt::print("  translated: PACT bit 80 on; logical page {:02X} -> ATR[{}]={:04X}{}\n", page, page, atr,
               (pact & MachineState::kPactFlagBits) != 0
                   ? fmt::format("; PACT flag bits {:02X} do not change the mode", pact & MachineState::kPactFlagBits) : "");
    int real = 0;
    if (m_.state.resolve(logical, pact, MachineState::kAtrTaskGroup0, forWrite, real))
        fmt::print("  resolves to real {:06X}\n", real);
    else
        fmt::print("  STORAGE PROTECTION: ATR[{}]={:04X} rejects {} at logical {:04X}\n", page, atr, operation, logical);
}


}  // namespace sim36::monitor
