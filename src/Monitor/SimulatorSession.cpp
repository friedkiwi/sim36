#include "Monitor/SimulatorSession.h"
#include <filesystem>
#include "Monitor/PanicDump.h"
#include "Monitor/MachineSnapshot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>

#include <fmt/format.h>

#include "Configuration/ConfigError.h"
#include "Configuration/IplSourceTable.h"
#include "Host/Console.h"
#include "Monitor/CommandLine.h"
#include "Monitor/CommandRegistry.h"
#include "Monitor/ConfigurationRenderer.h"
#include "Storage/DiskBackend.h"
#include "Storage/FileNotFoundError.h"
#include "Storage/Vtoc.h"

namespace sim36::monitor {

using configuration::EmulatorConfig;
using configuration::StationConfig;
using configuration::TapeConfig;

namespace {

const char* const kPrompt = "sim36> ";

void need(const std::vector<std::string>& a, std::size_t count, const std::string& usage)
{
    if (a.size() < count) throw MonitorError(usage);
}

bool eq(const std::string& a, const char* b) { return equalsIgnoreCase(a, b); }

std::string normal(const std::string& value)
{
    std::string s = toLower(value);
    for (char& c : s) if (c == '_') c = '-';
    return s;
}

// int.Parse: optional sign and decimal digits over the whole string.
int parseInt(const std::string& value)
{
    std::string s = value;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) i = 1;
    if (i == s.size()) throw MonitorError("Input string was not in a correct format.");
    for (std::size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k])))
            throw MonitorError("Input string was not in a correct format.");
    // Overflow past Int32 is a distinct message in the reference runtime.
    long long n = 0;
    for (std::size_t k = i; k < s.size(); ++k) {
        n = n * 10 + (s[k] - '0');
        if (n > 21474836480LL) break;
    }
    if (s[0] == '-') n = -n;
    if (n > 2147483647LL || n < -2147483648LL)
        throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(n);
}

int parseHex(const std::string& value)
{
    std::string s = value;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            throw MonitorError("Input string was not in a correct format.");
    if (s.size() > 8) throw MonitorError("Value was either too large or too small for an Int32.");
    unsigned long v = std::strtoul(s.c_str(), nullptr, 16);
    if (v > 0x7FFFFFFFUL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

int parseMemoryKb(const std::string& value)
{
    std::string s = toLower(value);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    int multiplier = 1;
    auto endsWith = [&](const char* suffix) {
        std::string suf(suffix);
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (endsWith("kb")) s = s.substr(0, s.size() - 2);
    else if (endsWith("k")) s = s.substr(0, s.size() - 1);
    else if (endsWith("mb")) { s = s.substr(0, s.size() - 2); multiplier = 1024; }
    else if (endsWith("m")) { s = s.substr(0, s.size() - 1); multiplier = 1024; }
    return parseInt(s) * multiplier;
}

bool parseBool(const std::string& value)
{
    if (eq(value, "on") || eq(value, "yes") || eq(value, "true") || value == "1") return true;
    if (eq(value, "off") || eq(value, "no") || eq(value, "false") || value == "0") return false;
    throw MonitorError("boolean value is on or off");
}

bool parseMode(const std::string& value)
{
    if (eq(value, "ro") || eq(value, "readonly")) return true;
    if (eq(value, "rw") || eq(value, "writable")) return false;
    throw MonitorError("medium mode is ro, rw, or (disk0 only) overlay");
}

void parseStationId(const std::string& id, int& port, int& address)
{
    auto fail = []() {
        // Both halves are three bits.  The port bound is not cosmetic: the
        // controller packs it as ((port & 7) << 4), so port 8 used to alias
        // silently onto port 0 rather than being refused.
        throw MonitorError("station needs port.address, with port 0-7 and address 0-6");
    };
    std::size_t dot = id.find('.');
    if (dot == std::string::npos || id.find('.', dot + 1) != std::string::npos) fail();
    auto tryParse = [](const std::string& s, int& out) {
        if (s.empty()) return false;
        std::size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (i == s.size()) return false;
        for (std::size_t k = i; k < s.size(); ++k)
            if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
        long long n = std::strtoll(s.c_str(), nullptr, 10);
        if (n > 2147483647LL || n < -2147483648LL) return false;
        out = static_cast<int>(n);
        return true;
    };
    if (!tryParse(id.substr(0, dot), port) || !tryParse(id.substr(dot + 1), address) ||
        port < 0 || port > 7 || address < 0 || address > 6)
        fail();
}

void setListen(StationConfig& s, const std::string& value)
{
    if (eq(value, "off")) { s.listenPort = 0; return; }
    std::size_t colon = value.rfind(':');
    int port = 0;
    bool ok = colon != std::string::npos && colon >= 1;
    if (ok) {
        std::string p = value.substr(colon + 1);
        ok = !p.empty();
        for (char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) ok = false;
        if (ok) {
            long long n = std::strtoll(p.c_str(), nullptr, 10);
            ok = n >= 0 && n <= 65535;
            port = static_cast<int>(n);
        }
    }
    if (!ok) throw MonitorError("listen needs host:port or off");
    s.listenHost = value.substr(0, colon);
    s.listenPort = port;
}

std::string fullPath(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec) return path;
    return p.lexically_normal().string();
}

std::string fileName(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string randomHex32()
{
    static std::mt19937_64 rng{std::random_device{}()};
    std::string s;
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) s.push_back(digits[rng() & 15]);
    return s;
}

}  // namespace

SimulatorSession::SimulatorSession() = default;

SimulatorSession::SimulatorSession(EmulatorConfig definition) : definition_(std::move(definition))
{
    if (!definition_.volumePath.empty()) reportVolume(definition_.volumePath);
    if (!definition_.stations.empty()) reconcileListeners();
    if (definition_.stationMultiplex) startMultiplexer();
}

SimulatorSession::~SimulatorSession()
{
    if (multiplexer_) multiplexer_->reclaim();
    multiplexer_.reset();
    if (machine_) releaseMachine();
    disposeStationBackends();
}

void SimulatorSession::activateConfiguredServices()
{
    reconcileListeners();
    if (definition_.stationMultiplex) startMultiplexer();
}

bool SimulatorSession::inputRedirected() const { return !host::Console::isInteractive(); }

void SimulatorSession::run()
{
    fmt::print("SIM/36 - System/36 emulator\n");
    fmt::print("type 'help' for commands, 'quit' to exit\n");
    host::Console console;
    console.setCompleter([](const std::vector<std::string>& preceding) {
        CommandRegistry::Completion c = CommandRegistry::complete(preceding);
        return host::Console::Completion{std::move(c.words), c.paths};
    });
    while (!quitRequested_) {
        std::optional<std::string> line = console.readLine(kPrompt);
        if (!line) break;
        try {
            execute(*line);
        } catch (const std::exception& e) {
            fmt::print("error: {}\n", e.what());
        }
    }
}

void SimulatorSession::executeFile(const std::string& path, bool echo)
{
    const std::string resolved = resolvePath(path);
    const std::string key = fullPath(resolved);
    std::ifstream in(key);
    if (!in) throw storage::FileNotFoundError::forPath(key);
    if (!activeFiles_.insert(key).second) throw MonitorError("recursive command file: " + key);
    sourceDirectories_.push_back(std::filesystem::path(key).parent_path());
    try {
        int lineNo = 0;
        std::string line;
        while (std::getline(in, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (echo) fmt::print("{}{}\n", kPrompt, line);
            try {
                execute(line);
            } catch (const CommandFileError&) {
                throw;
            } catch (const std::exception& e) {
                throw CommandFileError(key, lineNo, e.what());
            }
            if (quitRequested_) break;
        }
    } catch (...) {
        sourceDirectories_.pop_back();
        activeFiles_.erase(key);
        throw;
    }
    sourceDirectories_.pop_back();
    activeFiles_.erase(key);
}

void SimulatorSession::execute(const std::string& line)
{
    executeTokens(CommandLine::tokenize(line));
}

void SimulatorSession::executeTokens(std::vector<std::string> a)
{
    if (a.empty()) return;
    a = CommandRegistry::canonicalize(std::move(a));
    // With a guest thread owning the machine, every command that touches
    // machine state runs on that thread at its next safe boundary, so a
    // live `console send` sees exactly what a scripted one sees.  Host
    // control verbs (`stop`, `wait`, `quit`, ...) stay on the monitor
    // thread.
    if (monitor_ && monitor_->shouldMarshal() && !CommandRegistry::isHostControl(a)) {
        const Args copy = a;
        monitor_->runOnGuestThread([this, &copy] { executeTokensCore(copy); });
        return;
    }
    executeTokensCore(a);
}

void SimulatorSession::executeTokensCore(const Args& a)
{
    if (a.empty()) return;
    const std::string verb = toLower(a[0]);
    const Command* command = CommandRegistry::find(verb);
    if (command == nullptr && verb == "power")
        throw MonitorError("unknown command 'power'; use 'reset' to release a constructed machine and 'ipl' to construct one");
    if (command == nullptr && (verb == "run" || verb == "go" || verb == "halt" ||
                               verb == "live" || verb == "bg"))
        throw MonitorError("unknown command '" + a[0] +
                           "'; use ipl/start/stop, step N, wait idle, or show status");
    if (command == nullptr && verb == "security")
        throw MonitorError(
            "'security' has been removed: it never reached guest storage. Use "
            "'listener-auto-signon [on|off]' (inverted sense: old 'security on' is "
            "'listener-auto-signon off'). docs/s36/listener-auto-signon-knob.md");
    if (command == nullptr) throw MonitorError("unknown command '" + a[0] + "' - try help");

    if (verb == "quit") { quitRequested_ = true; return; }
    if (verb == "panic") { panic(a); return; }
    if (verb == "help") { CommandRegistry::printHelp(); return; }
    if (verb == "do") {
        need(a, 2, "do <command-file>");
        executeFile(a[1], true);
        return;
    }
    if (verb == "reset") { resetMachine(a); return; }
    if (verb == "snapshot") { snapshot(a); return; }
    if (verb == "show" && (a.size() < 2 || !CommandRegistry::isShowTarget(a[1])))
        throw MonitorError(a.size() < 2
            ? std::string("show what? config|status|terminal|cpu|storage|csp|atr|ptt|workstation")
            : "unknown show target '" + a[1] + "'");
    if (verb == "show" && a.size() > 1 && eq(a[1], "config")) {
        if (a.size() != 2) throw MonitorError("show config");
        fmt::print("{}", ConfigurationRenderer::renderHuman(definition_, machineConstructed()));
        return;
    }
    if (verb == "save") { saveConfig(a); return; }
    if (verb == "show" && a.size() > 1 && eq(a[1], "status")) {
        if (a.size() != 2) throw MonitorError("show status");
        showStatus();
        return;
    }
    if (verb == "show" && a.size() > 1 && eq(a[1], "terminal")) {
        if (a.size() != 2) throw MonitorError("show terminal");
        showTerminals();
        return;
    }
    if (verb == "get") {
        if (a.size() == 2 && eq(a[1], "terminal")) getTerminalConfiguration();
        else throw MonitorError(a.size() < 2
            ? std::string("get what? terminal")
            : "unknown get target '" + a[1] + "'; use 'get terminal'");
        return;
    }
    if (verb == "set" && a.size() > 1 &&
        (eq(a[1], "machine") || eq(a[1], "disk0") || eq(a[1], "diskette0") ||
         eq(a[1], "tape0") || eq(a[1], "station") || eq(a[1], "terminal"))) {
        setDefinition(a);
        return;
    }
    if (verb == "remove" && a.size() > 1 && eq(a[1], "station")) { removeStation(a); return; }
    if (verb == "attach" || verb == "detach") { media(a); return; }
    if (verb == "trace" && !machineConstructed()) {
        if (a.size() > 1 && eq(a[1], "workstation"))
            throw MonitorError("'trace workstation' requires a constructed machine; use 'ipl pause' for a zero-instruction boundary");
        if (a.size() == 1) {
            fmt::print("trace is {}\n", traceFlagsToString(pendingTrace_));
        } else {
            std::string joined;
            for (std::size_t i = 1; i < a.size(); ++i) {
                if (i != 1) joined += ",";
                joined += a[i];
            }
            try {
                pendingTrace_ = eq(a[1], "off") ? TraceNone : parseTraceFlags(joined);
            } catch (const std::invalid_argument& e) {
                throw MonitorError(e.what());
            }
            fmt::print("trace = {} (applies at next IPL)\n", traceFlagsToString(pendingTrace_));
        }
        return;
    }
    if (verb == "listener-auto-signon" && !machineConstructed()) {
        if (a.size() == 1) {
            fmt::print("listener-auto-signon {}\n", definition_.listenerAutoSignOn ? "on" : "off");
        } else {
            need(a, 2, "listener-auto-signon [on|off]");
            definition_.listenerAutoSignOn = parseBool(a[1]);
            fmt::print("listener-auto-signon {}; applies at next IPL\n",
                       definition_.listenerAutoSignOn ? "on" : "off");
        }
        return;
    }
    if (verb == "stations" && !machineConstructed()) { showConfigurableStations(); return; }

    if (!machineConstructed() && verb == "ipl") constructMachine();
    if (!machineConstructed()) {
        if (verb == "show") {
            if (a.size() > 1 && CommandRegistry::isMachineShowTarget(a[1]))
                throw MonitorError("'show " + a[1] +
                                   "' requires a constructed machine; use 'ipl' or 'ipl pause' first");
            throw MonitorError(a.size() < 2
                ? std::string("show what? config|status|terminal (guest-state views require IPL)")
                : "unknown show target '" + a[1] + "'; use 'show status', "
                  "'show terminal', or 'show config'");
        }
        if (verb == "set") {
            std::string target = a.size() > 1 ? toLower(a[1]) : "";
            for (const char* r : {"iar", "xr1", "xr2", "arr", "psr", "pxr1", "pxr2",
                                  "pdir", "piar", "wr4", "wr5", "wr6", "wr7"})
                if (target == r)
                    throw MonitorError("'set " + target + "' requires a constructed machine; use 'ipl pause' first");
            throw MonitorError(a.size() < 2 ? std::string("set what? machine|station|terminal")
                : "unknown set target '" + a[1] + "'; use machine, station, or terminal");
        }
        if (command->lifecycle == Lifecycle::Machine)
            throw MonitorError("'" + a[0] + "' requires a constructed machine; use 'ipl' or 'ipl pause' first");
        throw MonitorError("command '" + a[0] + "' is incomplete or unavailable before IPL - try help");
    }

    monitor_->executeTokens(resolveRuntimePaths(a));
}

std::vector<std::string> SimulatorSession::resolveRuntimePaths(const Args& a) const
{
    std::vector<std::string> b = a;
    const std::string v = toLower(b[0]);
    if (v == "loadfile" && b.size() > 1) b[1] = resolvePath(b[1]);
    else if (v == "snapshot" && b.size() > 2) b[2] = resolvePath(b[2]);
    else if (v == "diskette" && b.size() > 2 && eq(b[1], "insert")) b[2] = resolvePath(b[2]);
    else if (v == "tape" && b.size() > 2 && (eq(b[1], "load") || eq(b[1], "init"))) b[2] = resolvePath(b[2]);
    else if ((v == "tapetest" || v == "tapesvc") && b.size() > 1) b[1] = resolvePath(b[1]);
    else if (v == "wswrite" && b.size() > 2 && !b[2].empty() && b[2][0] == '@') b[2] = "@" + resolvePath(b[2].substr(1));
    return b;
}

void SimulatorSession::constructMachine()
{
    if (machine_) throw MonitorError("machine is already constructed");
    // A configuration that declares no stations gets the default
    // seven-station controller; declaring any station opts out entirely.
    definition_.applyDefaultStationsIfNoneDeclared();
    try {
        definition_.validate("machine definition");
    } catch (const configuration::ConfigError& e) {
        throw MonitorError(e.what());
    }
    reconcileListeners();
    std::unique_ptr<machine::Machine> candidate;
    try {
        candidate = std::make_unique<machine::Machine>(definition_, &stationBackends_);
    } catch (const storage::FileNotFoundError&) {
        for (auto& kv : stationBackends_) kv.second->bindMachine(&listenerTrace_, [this] { signalConstructedMachine(); });
        throw;
    } catch (const std::runtime_error& e) {
        for (auto& kv : stationBackends_) kv.second->bindMachine(&listenerTrace_, [this] { signalConstructedMachine(); });
        throw MonitorError(e.what());
    }
    candidate->trace.flags = pendingTrace_;
    candidate->readVtocs();
    machine_ = std::move(candidate);
    monitor_ = std::make_unique<MonitorCli>(*machine_);
    for (auto& kv : stationBackends_) kv.second->resetForMachine();
    // Put every client the multiplexer already placed into this new
    // machine's backends, before anything is IPLed: a client that selected
    // W3 while the machine was stopped must BE on W3 when the guest looks,
    // without reconnecting.
    if (multiplexer_) multiplexer_->rebind();
    machine_->startListeners();
}

void SimulatorSession::releaseMachine()
{
    if (!machine_) return;
    monitor_->stopExecutionForTeardown();
    pendingTrace_ = machine_->trace.flags;
    if (multiplexer_) multiplexer_->reclaim();
    monitor_.reset();
    machine_.reset();
    for (auto& kv : stationBackends_) kv.second->bindMachine(&listenerTrace_, [this] { signalConstructedMachine(); });
}

void SimulatorSession::resetMachine(const Args& a)
{
    if (a.size() > 2 || (a.size() == 2 && !eq(a[1], "--yes")))
        throw MonitorError("reset [--yes]");
    if (!machineConstructed()) {
        fmt::print("reset complete; machine stopped and configuration editable\n");
        return;
    }
    const bool running = monitor_ && monitor_->executionActive();
    if (running && a.size() == 1) {
        if (!sourceDirectories_.empty() || inputRedirected())
            throw MonitorError("reset would discard a running emulation; use 'reset --yes' in non-interactive input");
        fmt::print("The emulation is running; reset will discard its volatile state. Continue? [y/N] ");
        std::fflush(stdout);
        std::string answer;
        std::getline(std::cin, answer);
        if (!eq(answer, "y") && !eq(answer, "yes")) {
            fmt::print("reset cancelled\n");
            return;
        }
    }
    releaseMachine();
    fmt::print("reset complete; machine stopped and configuration editable\n");
}

void SimulatorSession::setDefinition(const Args& a)
{
    if (eq(a[1], "machine")) setMachine(a);
    else if (eq(a[1], "station")) setStation(a);
    else if (eq(a[1], "terminal")) setTerminal(a);
    else setDevice(a);
}

void SimulatorSession::setTerminal(const Args& a)
{
    if (a.size() < 3 || !eq(a[2], "multiplex"))
        throw MonitorError("set terminal multiplex on|off | "
                           "set terminal multiplex listen <host>:<port>");
    setMultiplex(a);
}

void SimulatorSession::setMachine(const Args& a)
{
    need(a, 4, "set machine <property> <value>");
    const std::string key = normal(a[2]);
    requireConfigurable();
    if (key == "model") {
        if (!configuration::ModelTable::isKnown(a[3])) throw MonitorError("unknown model '" + a[3] + "'");
        definition_.model = a[3];
    } else if (key == "memory" || key == "main-storage") definition_.mainStorageKb = parseMemoryKb(a[3]);
    else if (key == "task-work-area" || key == "task-work-area-sectors") definition_.taskWorkAreaSectors = parseInt(a[3]);
    else if (key == "host-model") definition_.hostModel = a[3];
    else if (key == "host-processor-model") definition_.hostProcessorModel = a[3];
    else if (key == "host-processor-feature") definition_.hostProcessorFeature = parseHex(a[3]);
    else if (key == "ipl-type") {
        std::string type;
        if (!configuration::IplSourceTable::tryNormalizeType(a[3], type))
            throw MonitorError("IPL type must be attend/attended or unattend/unattended");
        definition_.iplType = type;
    } else if (key == "ipl-source") definition_.iplSourceName = a[3];
    else if (key == "load-source") {
        // Where the CONTROL PROCESSOR reads phase 1 from, a different question
        // from ipl-source (the reload source phase 1 consults once running).
        if (!eq(a[3], "disk") && !eq(a[3], "diskette"))
            throw MonitorError("load-source must be 'disk' or 'diskette'");
        definition_.loadSourceName = a[3];
    } else if (key == "listener-auto-signon") definition_.listenerAutoSignOn = parseBool(a[3]);
    else if (key == "security")
        throw MonitorError(
            "'set machine security' has been removed: it never reached guest storage. "
            "Use 'set machine listener-auto-signon on|off' (inverted sense: old "
            "'security on' is 'listener-auto-signon off'). "
            "docs/s36/listener-auto-signon-knob.md");
    else if (key == "signon-use-router") definition_.consoleSignOnUseRouter = parseBool(a[3]);
    else if (key == "signon-statement") definition_.consoleSignOnStatement = parseBool(a[3]);
    else if (key == "signon-request") definition_.consoleSignOnRequest = parseBool(a[3]);
    else if (key == "signon-router-key") definition_.consoleSignOnRouterKey = parseHex(a[3]);
    else if (key == "ws-interactive") definition_.wsInteractive = parseBool(a[3]);
    else throw MonitorError("unknown machine property '" + a[2] + "'");
}

void SimulatorSession::setDevice(const Args& a)
{
    need(a, 3, "set disk0 <overlay|readonly|writable> | "
               "set <diskette0|tape0> <readonly|writable>");
    bool ro;
    bool overlay = false;
    if (eq(a[2], "overlay") || eq(a[2], "cow") || eq(a[2], "snapshot")) { overlay = true; ro = true; }
    else if (eq(a[2], "readonly") || eq(a[2], "ro")) ro = true;
    else if (eq(a[2], "writable") || eq(a[2], "rw")) ro = false;
    else throw MonitorError("device mode is readonly or writable (disk0 also accepts overlay)");

    if (eq(a[1], "disk0")) {
        requireConfigurable();
        definition_.volumeReadOnly = ro;
        definition_.volumeOverlay = overlay;
    } else if (eq(a[1], "diskette0")) {
        definition_.disketteReadOnly = ro;
    } else {
        if (!definition_.tape) definition_.tape = std::make_unique<TapeConfig>();
        definition_.tape->readOnly = ro;
    }
}

void SimulatorSession::setStation(const Args& a)
{
    need(a, 5, "set station <port.address> <role|device-code|listen|signon-at-ipl> <value>");
    requireConfigurable();
    StationConfig& s = findOrCreateStation(a[2]);
    const std::string key = normal(a[3]);
    if (key == "role") {
        bool known = false;
        for (const std::string& r : StationConfig::roles()) if (equalsIgnoreCase(r, a[4])) known = true;
        if (!known) throw MonitorError("station role is console, display, or printer");
        s.role = a[4];
    } else if (key == "device-code") { s.deviceCode = a[4]; s.deviceCodeGiven = true; }
    else if (key == "listen") setListen(s, a[4]);
    else if (key == "signon-at-ipl") s.signOnAtIpl = parseBool(a[4]);
    else throw MonitorError("station property is role, device-code, listen, or signon-at-ipl");
    reconcileListeners();
}

void SimulatorSession::setMultiplex(const Args& a)
{
    requireConfigurable();
    const std::string prefix = "set " + a[1] + " multiplex";
    need(a, 4, prefix + " on|off | " + prefix + " listen <host>:<port>");
    if (eq(a[3], "listen")) {
        need(a, 5, prefix + " listen <host>:<port>");
        const std::string& v = a[4];
        std::size_t colon = v.rfind(':');
        int port = 0;
        bool ok = colon != std::string::npos && colon >= 1;
        if (ok) {
            std::string p = v.substr(colon + 1);
            ok = !p.empty();
            for (char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) ok = false;
            if (ok) {
                long long n = std::strtoll(p.c_str(), nullptr, 10);
                ok = n >= 1 && n <= 65535;
                port = static_cast<int>(n);
            }
        }
        if (!ok) throw MonitorError(prefix + " listen needs host:port");
        definition_.multiplexHost = v.substr(0, colon);
        definition_.multiplexPort = port;
        // Moving the listener while it is up is a rebind, not a toggle.
        if (multiplexer_) {
            multiplexer_->rebind(definition_.multiplexHost, definition_.multiplexPort);
            fmt::print("station multiplexer: now listening on {}\n", multiplexer_->endpoint());
        }
        return;
    }
    bool on = parseBool(a[3]);
    definition_.stationMultiplex = on;
    if (on) {
        bool alreadyRunning = multiplexer_ != nullptr;
        startMultiplexer();
        if (alreadyRunning) reportMultiplexer();
        for (const StationConfig& s : definition_.stations)
            if (!s.isPrinter() && !s.isConsole() && s.listenPort != 0)
                fmt::print("station {}: per-station listener disabled (terminal multiplex is on)\n", s.id());
    } else {
        stopMultiplexer();
    }
    reconcileListeners();
}

void SimulatorSession::showConfigurableStations()
{
    if (definition_.stationMultiplex)
        fmt::print("  station multiplexer on {}:{}: one listener serves every display station\n",
                   definition_.multiplexHost, definition_.multiplexPort);
    for (const StationConfig& s : definition_.stations) {
        auto it = stationBackends_.find(s.id());
        host::StationBackend* backend = it == stationBackends_.end() ? nullptr : it->second.get();
        bool multiplexed = definition_.stationMultiplex && !s.isPrinter() && !s.isConsole();
        bool attached = multiplexed && multiplexer_ ? !multiplexer_->isFree(s.id())
                                                    : backend != nullptr && backend->attached();
        std::string endpoint = multiplexed
            ? "mux " + definition_.multiplexHost + ":" + std::to_string(definition_.multiplexPort)
            : backend != nullptr && backend->listening() ? backend->endpoint() : "(no listener)";
        fmt::print("  {}  device {}  {:<20} {}{}\n", s.id(), s.deviceCode, endpoint,
                   attached ? "attached" : "idle", s.isConsole() ? "  (console)" : "");
    }
}

void SimulatorSession::showStatus()
{
    fmt::print("machine {}\n", !machine_ ? "configurable (not constructed)"
                                : monitor_ && monitor_->executionActive() ? "running" : "stopped");
}

void SimulatorSession::getTerminalConfiguration()
{
    fmt::print("terminal multiplex {}\n", definition_.stationMultiplex ? "on" : "off");
    fmt::print("terminal multiplex listen {}:{}\n", definition_.multiplexHost, definition_.multiplexPort);
    for (const StationConfig& s : definition_.stations) {
        std::string transport = s.isConsole() ? "operator"
            : s.listenPort == 0 ? "listen off"
            : "listen " + s.listenHost + ":" + std::to_string(s.listenPort) +
              (definition_.stationMultiplex && !s.isPrinter() ? " (inactive while multiplex is on)" : "");
        fmt::print("terminal {} {} {}\n", s.id(),
                   s.isConsole() ? "console" : s.isPrinter() ? "printer" : "display", transport);
    }
}

void SimulatorSession::showTerminals()
{
    showStatus();
    fmt::print("terminal transport {}\n", definition_.stationMultiplex
        ? "multiplex " + definition_.multiplexHost + ":" + std::to_string(definition_.multiplexPort)
        : std::string("per-station listeners"));
    for (const StationConfig& s : definition_.stations) {
        auto it = stationBackends_.find(s.id());
        host::StationBackend* backend = it == stationBackends_.end() ? nullptr : it->second.get();
        bool console = s.isConsole();
        bool multiplexed = definition_.stationMultiplex && !s.isPrinter() && !console;
        bool attached = multiplexed && multiplexer_ ? !multiplexer_->isFree(s.id())
                                                    : backend != nullptr && backend->attached();
        std::string endpoint = console ? "operator console"
            : multiplexed ? definition_.multiplexHost + ":" + std::to_string(definition_.multiplexPort)
            : backend == nullptr || !backend->listening() ? "off" : backend->endpoint();
        std::string state = attached
            ? (backend != nullptr && backend->attached() && !backend->ready() ? "attached, negotiating" : "attached")
            : multiplexed ? "available"
            : backend != nullptr && backend->listening() ? "listening" : "inactive";
        std::string type = backend != nullptr && !backend->terminalType().empty()
            ? "  " + backend->terminalType() : "";
        fmt::print("  {:<3} {:<7} {:<24} {}{}\n", s.id(),
                   console ? "console" : s.isPrinter() ? "printer" : "display", endpoint, state, type);
    }
}

void SimulatorSession::reconcileListeners()
{
    if (machineConstructed()) return;
    std::set<std::string> wanted;
    for (const StationConfig& s : definition_.stations) {
        wanted.insert(s.id());
        bool printer = s.isPrinter();
        bool console = s.isConsole();
        bool shouldListen = !console && (printer || !definition_.stationMultiplex) && s.listenPort != 0;
        std::string endpoint = s.listenHost + ":" + std::to_string(s.listenPort);
        auto it = stationBackends_.find(s.id());
        host::StationBackend* backend = it == stationBackends_.end() ? nullptr : it->second.get();
        bool compatible = backend != nullptr &&
            backend->kind() == (printer ? host::StationKind::Printer : host::StationKind::Display) &&
            backend->endpoint() == endpoint &&
            (console ? static_cast<host::WorkstationBackend*>(backend)->isConsoleAttachment()
                     : backend->listening() == shouldListen);
        if (compatible) continue;
        if (backend != nullptr) {
            if (backend->attached())
                fmt::print("warning: station {} has a connected client; listener "
                           "reconfiguration will disconnect it\n", s.id());
            stationBackends_.erase(it);
        }
        std::unique_ptr<host::StationBackend> fresh;
        if (printer)
            fresh = std::make_unique<host::PrinterBackend>(s.listenHost, s.listenPort, "printer " + s.id(),
                                                           &listenerTrace_, [this] { signalConstructedMachine(); });
        else
            fresh = std::make_unique<host::WorkstationBackend>(s.listenHost, s.listenPort, "station " + s.id(),
                                                               &listenerTrace_, [this] { signalConstructedMachine(); });
        if (console) static_cast<host::WorkstationBackend*>(fresh.get())->attachConsole();
        else if (shouldListen) fresh->listen();
        stationBackends_[s.id()] = std::move(fresh);
    }
    std::vector<std::string> gone;
    for (const auto& kv : stationBackends_)
        if (wanted.find(kv.first) == wanted.end()) gone.push_back(kv.first);
    for (const std::string& id : gone) {
        if (stationBackends_[id]->attached())
            fmt::print("warning: removed station {} has a connected client; it will be disconnected\n", id);
        stationBackends_.erase(id);
    }
}

void SimulatorSession::disposeStationBackends() { stationBackends_.clear(); }

// The doorbell a listener rings: a machine, if one is latched, wakes its
// parked driver loop.
void SimulatorSession::signalConstructedMachine()
{
    if (machine_) machine_->signalNativeEvent();
}

// ---- IStationMultiplexerHost

std::vector<host::MultiplexStationView> SimulatorSession::multiplexStations()
{
    if (machine_) return machine_->multiplexStations();

    std::vector<const StationConfig*> ordered;
    for (const StationConfig& s : definition_.stations) ordered.push_back(&s);
    std::stable_sort(ordered.begin(), ordered.end(), [](const StationConfig* x, const StationConfig* y) {
        return x->port != y->port ? x->port < y->port : x->address < y->address;
    });
    std::vector<host::MultiplexStationView> view;
    int n = 0;
    for (const StationConfig* s : ordered) {
        if (s->isPrinter()) continue;
        n++;
        host::MultiplexStationView v;
        v.number = n;
        v.id = s->id();
        v.isConsole = s->port == 0 && s->address == 0;
        // No machine, so no backend and nothing attached.  Whether a
        // multiplexer client is already parked here is the multiplexer's own
        // bookkeeping, which it applies itself.
        v.available = true;
        v.backend = nullptr;
        view.push_back(v);
    }
    return view;
}

std::string SimulatorSession::machineStatusText()
{
    if (!machine_) return "stopped";
    return monitor_ && monitor_->executionActive() ? "running" : "stopped";
}

std::vector<std::string> SimulatorSession::mediaLines()
{
    if (machine_) return machine_->mediaLines();

    // Before IPL construction the definition is all there is, and it is
    // enough.  The image is measured off the file rather than guessed at,
    // and a path that does not exist says so instead of inventing a size.
    std::vector<std::string> lines;
    lines.push_back("Drive 1: " + mediaName(definition_.volumePath) + "  " + fileSize(definition_.volumePath) +
                    (definition_.volumeOverlay ? " overlay" : definition_.volumeReadOnly ? " readonly" : ""));
    if (!definition_.diskettePath.empty())
        lines.push_back("Diskette: " + mediaName(definition_.diskettePath) + "  " + fileSize(definition_.diskettePath) +
                        (definition_.disketteReadOnly ? " readonly" : ""));
    if (definition_.tape && !definition_.tape->folderPath.empty()) {
        std::string folder = definition_.tape->folderPath;
        while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
        lines.push_back("Tape: " + mediaName(folder) + (definition_.tape->readOnly ? " readonly" : ""));
    }
    return lines;
}

std::string SimulatorSession::mediaName(const std::string& path)
{
    if (path.empty()) return "(none)";
    std::string trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\')) trimmed.pop_back();
    return std::filesystem::path(trimmed).filename().string();
}

std::string SimulatorSession::fileSize(const std::string& path)
{
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec) || ec) return "(not attached)";
    const auto b = static_cast<long long>(std::filesystem::file_size(path, ec));
    if (ec) return "(unreadable)";
    if (b >= 1024LL * 1024 * 1024) return std::to_string(b / (1024LL * 1024 * 1024)) + "G";
    if (b >= 1024LL * 1024) return std::to_string(b / (1024LL * 1024)) + "M";
    if (b >= 1024) return std::to_string(b / 1024) + "K";
    return std::to_string(b) + "B";
}

void SimulatorSession::startMultiplexer()
{
    if (multiplexer_) return;
    auto mux = std::make_unique<host::StationMultiplexer>(definition_.multiplexHost,
                                                          definition_.multiplexPort, &multiplexerTrace_, this);
    mux->listen();
    multiplexer_ = std::move(mux);
    reportMultiplexer();
}

int SimulatorSession::multiplexDisplayStations() const
{
    int n = 0;
    for (const StationConfig& s : definition_.stations)
        if (!s.isPrinter() && !s.isConsole()) ++n;
    return n;
}

void SimulatorSession::reportMultiplexer()
{
    fmt::print("terminal multiplex listening on {}; {} display stations available\n",
               multiplexer_->endpoint(), multiplexDisplayStations());
}

void SimulatorSession::stopMultiplexer()
{
    if (!multiplexer_) return;
    multiplexer_->reclaim();
    multiplexer_.reset();
    fmt::print("terminal multiplex off; connected terminals disconnected\n");
}

void SimulatorSession::removeStation(const Args& a)
{
    need(a, 3, "remove station <port.address>");
    requireConfigurable();
    StationConfig* s = findStation(a[2]);
    if (s == nullptr) throw MonitorError("no station " + a[2]);
    for (auto it = definition_.stations.begin(); it != definition_.stations.end(); ++it) {
        if (&*it == s) { definition_.stations.erase(it); break; }
    }
    reconcileListeners();
}

void SimulatorSession::media(const Args& a)
{
    need(a, 2, "attach|detach <disk0|diskette0|tape0> [path] [ro|rw]");
    bool attach = eq(a[0], "attach");
    const std::string target = toLower(a[1]);
    if (target == "disk0") {
        requireConfigurable();
        if (attach) {
            need(a, 3, "attach disk0 <image> [ro|rw]");
            std::string path = resolvePath(a[2]);
            bool overlay = definition_.volumeOverlay;
            bool readOnly = definition_.volumeReadOnly;
            if (a.size() > 3) {
                // `overlay` accepts writes and holds them in memory, so the
                // guest sees a writable disk while the image is never touched.
                overlay = eq(a[3], "overlay") || eq(a[3], "cow") || eq(a[3], "snapshot");
                readOnly = overlay || parseMode(a[3]);
            }
            reportVolume(path);
            definition_.volumePath = path;
            definition_.volumeOverlay = overlay;
            definition_.volumeReadOnly = readOnly;
        } else {
            definition_.volumePath.clear();
        }
        return;
    }
    if (target == "diskette0") {
        if (attach) {
            need(a, 3, "attach diskette0 <image> [ro|rw]");
            std::string path = resolvePath(a[2]);
            if (a.size() > 3) definition_.disketteReadOnly = parseMode(a[3]);
            definition_.diskettePath = path;
            if (machine_) monitor_->executeTokens({"diskette", "insert", path});
        } else {
            definition_.diskettePath.clear();
            if (machine_) monitor_->executeTokens({"diskette", "eject"});
        }
        return;
    }
    if (target == "tape0") {
        if (!definition_.tape) definition_.tape = std::make_unique<TapeConfig>();
        if (attach) {
            need(a, 3, "attach tape0 <folder> [ro|rw]");
            std::string path = resolvePath(a[2]);
            if (a.size() > 3) definition_.tape->readOnly = parseMode(a[3]);
            definition_.tape->folderPath = path;
            if (machine_) {
                if (definition_.tape->readOnly) monitor_->executeTokens({"tape", "load", path, "ro"});
                else monitor_->executeTokens({"tape", "load", path});
            }
        } else {
            definition_.tape->folderPath.clear();
            if (machine_) monitor_->executeTokens({"tape", "unload"});
        }
        return;
    }
    throw MonitorError("unknown device '" + a[1] + "'");
}

void SimulatorSession::reportVolume(const std::string& path)
{
    std::unique_ptr<storage::DiskBackend> disk;
    try {
        disk = std::make_unique<storage::DiskBackend>(path, storage::VolumeMode::ReadOnly);
    } catch (const storage::FileNotFoundError&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw MonitorError(e.what());
    }
    auto system = storage::Vtoc::readSystem(*disk);
    auto user = storage::Vtoc::readUser(*disk);
    fmt::print("volume {}: {} sectors, {} system and {} user VTOC entries\n",
               fileName(path), disk->sectorCount(), system.size(), user.size());
}

void SimulatorSession::saveConfig(const Args& a)
{
    if (a.size() < 3 || a.size() > 4 || !eq(a[1], "config") || (a.size() == 4 && !eq(a[3], "--force")))
        throw MonitorError("save config stdout|<file> [--force]");

    std::string replay;
    try {
        replay = ConfigurationRenderer::renderReplay(definition_);
    } catch (const std::invalid_argument& e) {
        throw MonitorError(e.what());
    }
    if (eq(a[2], "stdout")) {
        if (a.size() != 3) throw MonitorError("save config stdout");
        fmt::print("{}", replay);
        return;
    }

    const std::string path = fullPath(resolvePath(a[2]));
    const bool force = a.size() == 4;
    if (std::filesystem::exists(path) && !force) {
        // A command file must never consume its next command as an overwrite
        // response.  Scripts opt in explicitly with --force.
        if (!sourceDirectories_.empty() || inputRedirected())
            throw MonitorError(path + " exists; use --force to overwrite from non-interactive input");
        fmt::print("{} exists. Overwrite? [y/N] ", path);
        std::fflush(stdout);
        std::string answer;
        std::getline(std::cin, answer);
        if (!eq(answer, "y") && !eq(answer, "yes")) {
            fmt::print("save config cancelled\n");
            return;
        }
    }

    std::filesystem::path directory = std::filesystem::path(path).parent_path();
    if (directory.empty()) directory = std::filesystem::current_path();
    if (!std::filesystem::is_directory(directory))
        throw MonitorError("Could not find a part of the path \"" + directory.string() + "\".");
    std::filesystem::path temporary = directory /
        ("." + std::filesystem::path(path).filename().string() + "." + randomHex32() + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary);
        if (!out) throw MonitorError("cannot write " + temporary.string());
        out << replay;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        throw MonitorError("cannot replace " + path + ": " + ec.message());
    }
    fmt::print("configuration saved to {}\n", path);
}

bool SimulatorSession::checkFile(const std::string& path)
{
    std::set<std::string> active;
    std::function<bool(const std::string&)> check = [&](const std::string& file) -> bool {
        const std::string key = fullPath(file);
        std::ifstream in(key);
        if (!in) {
            fmt::print("{}: not found\n", file);
            return false;
        }
        if (!active.insert(key).second) {
            fmt::print("{}: recursive command file\n", file);
            return false;
        }
        const std::filesystem::path dir = std::filesystem::path(key).parent_path();
        bool ok = true;
        int lineNo = 0;
        std::string line;
        while (std::getline(in, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> a;
            try {
                a = CommandLine::tokenize(line);
            } catch (const std::exception& e) {
                fmt::print("{}:{}: {}\n", key, lineNo, e.what());
                ok = false;
                continue;
            }
            if (a.empty()) continue;
            if (CommandRegistry::find(a[0]) == nullptr) {
                fmt::print("{}:{}: unknown command '{}'\n", key, lineNo, a[0]);
                ok = false;
                continue;
            }
            if (equalsIgnoreCase(a[0], "do") && a.size() > 1) {
                std::filesystem::path inc(a[1]);
                if (!inc.is_absolute()) inc = dir / inc;
                if (!check(inc.lexically_normal().string())) ok = false;
            }
        }
        active.erase(key);
        return ok;
    };
    return check(path);
}

std::string SimulatorSession::resolvePath(const std::string& path) const
{
    if (std::filesystem::path(path).is_absolute() || sourceDirectories_.empty()) return path;
    return (sourceDirectories_.back() / path).lexically_normal().string();
}

StationConfig& SimulatorSession::findOrCreateStation(const std::string& id)
{
    StationConfig* s = findStation(id);
    if (s != nullptr) return *s;
    StationConfig fresh;
    parseStationId(id, fresh.port, fresh.address);
    definition_.stations.push_back(fresh);
    return definition_.stations.back();
}

StationConfig* SimulatorSession::findStation(const std::string& id)
{
    int port = 0, address = 0;
    parseStationId(id, port, address);
    return definition_.findStation(port, address);
}

void SimulatorSession::requireConfigurable() const
{
    if (machineConstructed())
        throw MonitorError("machine definition is latched by the current IPL; use 'reset' before changing it");
}

}  // namespace sim36::monitor

namespace sim36::monitor {

// Freeze first: an operator may need minutes to describe the fault, and
// the evidence must describe the instant panic was requested, not whatever
// state the guest reaches while the questions are open.
void SimulatorSession::panic(const Args& a)
{
    if (a.size() != 1) throw MonitorError("panic");
    if (monitor_) monitor_->stopExecutionForTeardown();

    fmt::print("What happened? ");
    std::fflush(stdout);
    std::string description;
    std::getline(std::cin, description);
    fmt::print("How can it be reproduced? ");
    std::fflush(stdout);
    std::string reproduction;
    std::getline(std::cin, reproduction);

    std::vector<host::StationBackend*> backends;
    for (auto& kv : stationBackends_) backends.push_back(kv.second.get());
    std::string path = PanicDump::create(panicAnswer(description), panicAnswer(reproduction), definition_, machine_.get(),
                                         pendingTrace_, backends);
    fmt::print("panic dump created: {}\n", path);
    quitRequested_ = true;
}

std::string SimulatorSession::panicAnswer(const std::string& answer)
{
    std::size_t b = answer.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "(not provided)";
    std::size_t e = answer.find_last_not_of(" \t\r\n");
    return answer.substr(b, e - b + 1);
}

void SimulatorSession::snapshot(const Args& a)
{
    need(a, 3, "snapshot save|load <checkpoint>");
    if (a.size() != 3 || (!eq(a[1], "save") && !eq(a[1], "load"))) throw MonitorError("snapshot save|load <checkpoint>");
    std::string path = resolvePath(a[2]);
    if (eq(a[1], "save")) {
        try {
            MachineSnapshot::save(path, definition_, machine_.get(), pendingTrace_);
        } catch (const std::runtime_error& e) {
            throw MonitorError(e.what());
        }
        fmt::print("snapshot: saved {} ({})\n", std::filesystem::absolute(path).string(),
                   machine_ ? "constructed" : "configurable");
        return;
    }

    MachineSnapshot::Loaded saved;
    try {
        saved = MachineSnapshot::load(path);
    } catch (const std::runtime_error& e) {
        throw MonitorError(e.what());
    }
    if (machine_) releaseMachine();
    if (multiplexer_) stopMultiplexer();
    disposeStationBackends();
    deleteSnapshotMedia();
    definition_ = saved.config;
    pendingTrace_ = saved.trace;
    snapshotMediaDirectory_ = saved.mediaDirectory;
    if (!definition_.volumePath.empty()) reportVolume(definition_.volumePath);
    if (!saved.powered) {
        reconcileListeners();
        if (definition_.stationMultiplex) startMultiplexer();
        fmt::print("snapshot: loaded configurable machine definition and media from {}\n",
                   std::filesystem::absolute(path).string());
        return;
    }
    try {
        reconcileListeners();
        if (definition_.stationMultiplex) startMultiplexer();
        constructMachine();
        std::string failure;
        if (!machine_->restoreCheckpoint(*saved.runtime, failure)) throw MonitorError(failure);
        fmt::print("snapshot: restored constructed machine at IAR {:04X} after {} guest instruction(s)\n",
                   machine_->state.msp.iar, machine_->msp().instructionsExecuted());
    } catch (...) {
        if (machine_) releaseMachine();
        throw;
    }
}

void SimulatorSession::deleteSnapshotMedia()
{
    if (snapshotMediaDirectory_.empty()) return;
    std::string owned = snapshotMediaDirectory_;
    snapshotMediaDirectory_.clear();
    std::error_code ec;
    if (std::filesystem::exists(owned, ec)) std::filesystem::remove_all(owned, ec);
}

}  // namespace sim36::monitor
