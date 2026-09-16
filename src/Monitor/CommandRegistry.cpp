#include "Monitor/CommandRegistry.h"

#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <fmt/format.h>

namespace sim36::monitor {

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string toLower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

namespace {

const std::vector<Command>& table()
{
    using L = Lifecycle;
    static const std::vector<Command> commands = {
        {"help", L::Any, "session", "help", "show this command list", {"?"}},
        {"quit", L::Any, "session", "quit", "leave the emulator", {"exit"}},
        {"panic", L::Any, "session", "panic", "capture diagnostics and leave the emulator", {}},
        {"do", L::Any, "session", "do <file>", "execute a command file", {}},
        {"show", L::Any, "session", "show config|status|terminal|...", "show configuration or machine state", {}},
        {"save", L::Any, "session", "save config stdout|<file> [--force]", "save a replayable configuration", {}},
        {"get", L::Any, "session", "get terminal", "show terminal listener settings", {}},
        {"set", L::Any, "session", "set machine|station|terminal|...", "change configuration or machine state", {}},
        {"attach", L::Any, "session", "attach <device> <path> [mode]", "attach media", {}},
        {"detach", L::Any, "session", "detach <device>", "detach media", {}},
        {"remove", L::Any, "session", "remove station <id>", "remove a configured station", {}},
        {"reset", L::Any, "session", "reset [--yes]", "release the constructed machine", {}},
        {"snapshot", L::Any, "session", "snapshot save|load <file>", "save or restore a native checkpoint", {}},
        {"ipl", L::ConstructsMachine, "execution", "ipl [pause]", "construct, reset, IPL and start", {}},
        {"start", L::Machine, "execution", "start", "resume continuous execution", {}},
        {"stop", L::Machine, "execution", "stop", "stop execution at a safe boundary", {}},
        {"step", L::Machine, "execution", "step [count]", "advance a bounded number of instructions", {}},
        {"wait", L::Machine, "execution", "wait idle [seconds]|<seconds>", "wait for idle or elapsed time", {}},
        {"listener-auto-signon", L::Any, "configuration", "listener-auto-signon [on|off]", "show or change listener AUTOSIGNON", {}},
        {"stations", L::Any, "inspection", "stations", "show station definitions and state", {}},
        {"trace", L::Any, "inspection", "trace <classes|off>", "show or change tracing", {}},

        {"vtoc", L::Machine, "inspection", "vtoc [system|user]", "list volume tables of contents", {}},
        {"lib", L::Machine, "inspection", "lib <name> [count]", "list library members", {}},
        {"sector", L::Machine, "inspection", "sector <n> [count]", "dump disk sectors", {}},
        {"dump", L::Machine, "inspection", "dump <address> [length]", "dump guest storage", {}},
        {"addrmap", L::Machine, "inspection", "addrmap <kind> [disp] [read|write]", "resolve an MSP address", {}},
        {"findmem", L::Machine, "inspection", "findmem <hexbytes>", "search guest storage", {}},
        {"watch", L::Machine, "debug", "watch <address> [length]|off|list", "watch guest writes", {}},
        {"break", L::Machine, "debug", "break <address>|list|clear", "manage MSP breakpoints", {"b"}},
        {"breakm", L::Machine, "debug", "breakm <member> <offset>|list|clear", "manage member-relative breakpoints", {}},
        {"poke", L::Machine, "debug", "poke <address> <bytes>", "write guest storage", {}},
        {"patch", L::Machine, "debug", "patch ...", "manage research patches", {}},
        {"boot", L::Machine, "debug", "boot", "run control-storage IPL and read boot record", {}},
        {"load", L::Machine, "debug", "load <library> <member>", "load an SSP member", {}},
        {"dis", L::Machine, "inspection", "dis [address] [count]", "disassemble guest storage", {}},
        {"loadfile", L::Machine, "debug", "loadfile <path> <address>", "load raw bytes into guest storage", {}},
        {"savemain", L::Machine, "inspection", "savemain <file>", "export current guest main storage", {}},
        {"ace", L::Machine, "inspection", "ace <address>|queue <n>", "decode action control elements", {}},
        {"iob", L::Machine, "inspection", "iob <address>", "decode an I/O block", {}},
        {"tu", L::Machine, "inspection", "tu <address>", "decode a terminal unit block", {"tub"}},
        {"diskread", L::Machine, "device", "diskread <sector> [count]", "issue an SVC 40 disk read", {}},
        {"diskette", L::Machine, "device", "diskette [insert <image>|eject]", "inspect or change the diskette", {}},
        {"tape", L::Machine, "device", "tape ...", "inspect or operate the tape drive", {}},
        {"dsktread", L::Machine, "device", "dsktread <c> <h> <r> [count]", "issue a diskette read", {}},
        {"dsktwrite", L::Machine, "device", "dsktwrite <c> <h> <r> <hex>", "issue a diskette write", {}},
        {"wsconfig", L::Machine, "workstation", "wsconfig ...", "inspect workstation configuration", {}},
        {"wsioch", L::Machine, "workstation", "wsioch 42|43 <cmd> [len] [unit]", "issue workstation I/O", {}},
        {"wsoutput", L::Machine, "workstation", "wsoutput <id> ...", "select workstation rendering", {}},
        {"wswrite", L::Machine, "workstation", "wswrite <id> <data>", "issue workstation output", {}},
        {"console", L::Machine, "workstation", "console ...", "inspect or drive the operator console", {}},
        {"wsformat", L::Machine, "research", "wsformat <id> ...", "render an SSP display format", {}},
        {"wsinvite", L::Machine, "workstation", "wsinvite <id>", "invite workstation input", {}},
        {"wsinput", L::Machine, "workstation", "wsinput <id> <opcode> [hex]", "inject workstation input", {}},
        {"wsread", L::Machine, "workstation", "wsread <id>", "read workstation input", {}},
        {"prtwrite", L::Machine, "device", "prtwrite <id> <data>", "write a printer record", {}},
        {"prtend", L::Machine, "device", "prtend <id>", "end a print job", {}},
        {"sched", L::Machine, "inspection", "sched", "show scheduler events", {}},
        {"timers", L::Machine, "inspection", "timers [service]", "show native timers", {}},
        {"actions", L::Machine, "inspection", "actions", "show SVC 0B action-controller coverage", {}},
        {"tasklist", L::Machine, "inspection", "tasklist [current|id|address]", "show SSP tasks", {}},
        {"mapstate", L::Machine, "inspection", "mapstate [current|id|address]", "show task address mapping", {}},
        {"sqsstate", L::Machine, "inspection", "sqsstate [all|address]", "audit system queue space", {}},
        {"residency", L::Machine, "inspection", "residency program|transient ...", "query SSP residency", {}},
        {"wddqstate", L::Machine, "inspection", "wddqstate", "show #WDDQ state", {}},
        {"smf", L::Machine, "research", "smf ...", "inspect system measurement", {}},
        {"allocchain", L::Machine, "inspection", "allocchain <id|address>", "decode task allocations", {}},
        {"modules", L::Machine, "inspection", "modules [active|loaded|name]", "show loaded SSP modules", {}},
        {"modstorage", L::Machine, "inspection", "modstorage", "show module storage arena", {}},
        {"cptcstate", L::Machine, "research", "cptcstate <id|address> [classify]", "decode #CPTC state", {}},
        {"tutopology", L::Machine, "workstation", "tutopology", "show terminal-unit topology", {}},
        {"wsstate", L::Machine, "workstation", "wsstate [watch|clear]", "show workstation sessions", {}},
        {"wsscan", L::Machine, "research", "wsscan", "run the recovered wsentry scan", {}},
        {"wscontract", L::Machine, "research", "wscontract ...", "inspect workstation lifecycle contracts", {}},
        {"wsentry", L::Machine, "research", "wsentry ...", "schedule workstation-entry experiments", {}},
        {"wshri", L::Machine, "research", "wshri <station>", "deliver reported-HRI action 20", {}},
        {"svtubstate", L::Machine, "research", "svtubstate", "show #SVTUB decision state", {}},
        {"whereis", L::Machine, "inspection", "whereis [task] [iar]", "name the SSP member at an IAR", {}},
        {"wspresent", L::Machine, "research", "wspresent <task> [ublk]", "post a workstation-present event", {}},
        {"wspresentst", L::Machine, "research", "wspresentst <station>", "present a station by id", {}},
        {"tfrm36", L::Machine, "research", "tfrm36 ...", "exercise TFRM36 transfer", {}},
        {"wsaid", L::Machine, "research", "wsaid ...", "exercise workstation AID delivery", {}},
        {"wsoc", L::Machine, "research", "wsoc <station> <active|inactive>", "set workstation OC state", {}},
        {"wsuser", L::Machine, "research", "wsuser ...", "inspect workstation user state", {}},
        {"wspresentws", L::Machine, "research", "wspresentws ...", "exercise workstation-present path", {}},
        {"wspost", L::Machine, "research", "wspost ...", "exercise SLIC workstation posting", {}},
        {"postrk", L::Machine, "research", "postrk ...", "post a router-key element", {}},
        {"callssp", L::Machine, "research", "callssp ...", "exercise NuCallSSP", {}},
        {"signonstmt", L::Machine, "research", "signonstmt ...", "post a sign-on statement", {}},
        {"signoncmd", L::Machine, "research", "signoncmd ...", "post a sign-on command", {}},
        {"signonreq", L::Machine, "research", "signonreq ...", "exercise sign-on request state", {}},
        {"wsattach", L::Machine, "research", "wsattach ...", "exercise experimental TU attachment", {}},
        {"condbelem", L::Machine, "research", "condbelem ...", "post an experimental console DB element", {}},
        {"msscmsg", L::Machine, "research", "msscmsg ...", "post an #MSSC message", {}},
        {"signonexp", L::Machine, "research", "signonexp ...", "run sign-on experiments", {}},
        {"xferid", L::Machine, "research", "xferid <id>", "issue SVC 04 from the current task", {}},
        {"xferterm", L::Machine, "research", "xferterm", "enter nupterm termination continuation", {}},
        {"nuptermscan", L::Machine, "research", "nuptermscan", "run nupterm dependency scan", {}},
        {"selftest", L::Machine, "test", "selftest", "run monitor worked examples", {}},
        {"tapetest", L::Machine, "test", "tapetest [dir]", "test the folder-tape backend", {}},
        {"tapesvc", L::Machine, "test", "tapesvc [dir]", "test tape SVCs", {}},
        {"conformance", L::Machine, "test", "conformance", "check control-storage IPL vectors", {}},
    };
    return commands;
}

}  // namespace

const std::vector<Command>& CommandRegistry::all() { return table(); }

const Command* CommandRegistry::find(const std::string& name)
{
    for (const Command& c : table()) {
        if (equalsIgnoreCase(name, c.name)) return &c;
        for (const char* alias : c.aliases)
            if (equalsIgnoreCase(name, alias)) return &c;
    }
    return nullptr;
}

std::vector<std::string> CommandRegistry::canonicalize(std::vector<std::string> tokens)
{
    if (tokens.empty()) return tokens;
    const Command* c = find(tokens[0]);
    if (c == nullptr || equalsIgnoreCase(tokens[0], c->name)) return tokens;
    tokens[0] = c->name;
    return tokens;
}

bool CommandRegistry::isMachineShowTarget(const std::string& target)
{
    for (const char* t : {"cpu", "storage", "csp", "atr", "ptt", "workstation"})
        if (equalsIgnoreCase(target, t)) return true;
    return false;
}

bool CommandRegistry::isShowTarget(const std::string& target)
{
    return equalsIgnoreCase(target, "config") || equalsIgnoreCase(target, "status") ||
           equalsIgnoreCase(target, "terminal") || isMachineShowTarget(target);
}

CommandRegistry::Completion CommandRegistry::complete(const std::vector<std::string>& preceding)
{
    Completion out;
    auto words = [&out](std::initializer_list<const char*> ws) {
        for (const char* w : ws) out.words.push_back(w);
    };
    if (preceding.empty()) {
        for (const Command& c : table()) out.words.push_back(c.name);
        return out;
    }
    const Command* command = find(preceding[0]);
    if (command == nullptr) return out;
    const std::string verb = command->name;
    const std::size_t pos = preceding.size();  // index of the word being completed
    auto at = [&preceding](std::size_t i, const char* w) {
        return i < preceding.size() && equalsIgnoreCase(preceding[i], w);
    };

    if (verb == "show" && pos == 1) {
        words({"config", "status", "terminal", "cpu", "storage", "csp", "atr", "ptt", "workstation"});
    } else if (verb == "get" && pos == 1) {
        words({"terminal"});
    } else if (verb == "set") {
        if (pos == 1) {
            words({"machine", "station", "terminal", "disk0", "diskette0", "tape0",
                   "iar", "xr1", "xr2", "arr", "psr", "pxr1", "pxr2", "pdir", "piar",
                   "wr4", "wr5", "wr6", "wr7"});
        } else if (at(1, "machine") && pos == 2) {
            words({"model", "csp-type", "task-work-area", "host-model", "host-processor-model",
                   "host-processor-feature", "ipl-type", "ipl-source", "load-source",
                   "listener-auto-signon", "signon-use-router", "signon-statement",
                   "signon-request", "signon-router-key", "ws-interactive"});
        } else if (at(1, "machine") && pos == 3 &&
                   (at(2, "listener-auto-signon") || at(2, "signon-use-router") ||
                    at(2, "signon-statement") || at(2, "signon-request") || at(2, "ws-interactive"))) {
            words({"on", "off"});
        } else if (at(1, "machine") && pos == 3 && at(2, "model")) {
            words({"advanced36", "5363", "5364"});
        } else if (at(1, "machine") && pos == 3 && at(2, "csp-type")) {
            words({"advanced36"});
        } else if (at(1, "station") && pos == 3) {
            words({"role", "device-code", "listen", "signon-at-ipl"});
        } else if (at(1, "station") && pos == 4 && at(3, "role")) {
            words({"console", "display", "printer"});
        } else if (at(1, "station") && pos == 4 && at(3, "signon-at-ipl")) {
            words({"on", "off"});
        } else if (at(1, "terminal") && pos == 2) {
            words({"multiplex"});
        } else if (at(1, "terminal") && pos == 3) {
            words({"on", "off", "listen"});
        }
    } else if (verb == "save") {
        if (pos == 1) words({"config"});
        else if (pos == 2) { words({"stdout"}); out.paths = true; }
        else if (pos == 3) words({"--force"});
    } else if (verb == "snapshot") {
        if (pos == 1) words({"save", "load"});
        else if (pos == 2) out.paths = true;
    } else if (verb == "do" || verb == "loadfile" || verb == "savemain" ||
               verb == "tapetest" || verb == "tapesvc") {
        if (pos == 1) out.paths = true;
    } else if (verb == "attach") {
        if (pos == 1) words({"disk0", "diskette0", "tape0"});
        else if (pos == 2) out.paths = true;
    } else if (verb == "detach" && pos == 1) {
        words({"disk0", "diskette0", "tape0"});
    } else if (verb == "remove" && pos == 1) {
        words({"station"});
    } else if (verb == "reset" && pos == 1) {
        words({"--yes"});
    } else if (verb == "ipl" && pos == 1) {
        words({"pause"});
    } else if (verb == "wait" && pos == 1) {
        words({"idle"});
    } else if (verb == "listener-auto-signon" && pos == 1) {
        words({"on", "off"});
    } else if (verb == "trace" && pos == 1) {
        words({"off", "workstation"});
    } else if (verb == "watch" && pos == 1) {
        words({"off", "list"});
    } else if ((verb == "break" || verb == "breakm") && pos == 1) {
        words({"list", "clear"});
    } else if (verb == "diskette") {
        if (pos == 1) words({"insert", "eject"});
        else if (pos == 2 && at(1, "insert")) out.paths = true;
    } else if (verb == "vtoc" && pos == 1) {
        words({"system", "user"});
    } else if ((verb == "tasklist" || verb == "mapstate") && pos == 1) {
        words({"current"});
    } else if (verb == "sqsstate" && pos == 1) {
        words({"all"});
    } else if (verb == "modules" && pos == 1) {
        words({"active", "loaded"});
    } else if (verb == "residency" && pos == 1) {
        words({"program", "transient"});
    } else if (verb == "wsstate" && pos == 1) {
        words({"watch", "clear"});
    } else if (verb == "addrmap" && pos == 3) {
        words({"read", "write"});
    } else if (verb == "wsioch" && pos == 1) {
        words({"42", "43"});
    } else if (verb == "wsoc" && pos == 2) {
        words({"active", "inactive"});
    } else if (verb == "ace" && pos == 1) {
        words({"queue"});
    } else if (verb == "cptcstate" && pos == 2) {
        words({"classify"});
    }
    return out;
}

void CommandRegistry::printHelp()
{
    // Groups are printed in order of first appearance, as the reference
    // emulator's GroupBy does.
    std::vector<std::string> groups;
    for (const Command& c : table()) {
        bool seen = false;
        for (const std::string& g : groups) if (g == c.group) { seen = true; break; }
        if (!seen) groups.push_back(c.group);
    }
    for (const std::string& group : groups) {
        fmt::print("{}:\n", group);
        for (const Command& c : table()) {
            if (group != c.group) continue;
            std::string aliases;
            if (!c.aliases.empty()) {
                aliases = " (alias: ";
                for (std::size_t i = 0; i < c.aliases.size(); ++i) {
                    if (i != 0) aliases += ", ";
                    aliases += c.aliases[i];
                }
                aliases += ")";
            }
            fmt::print("  {:<42} {}{}\n", c.usage, c.summary, aliases);
        }
        fmt::print("\n");
    }
}

bool CommandRegistry::isHostControl(const std::vector<std::string>& a)
{
    if (a.empty()) return true;
    const std::string verb = toLower(a[0]);
    if (verb == "quit" || verb == "panic" || verb == "help" || verb == "do" ||
        verb == "reset" || verb == "save" || verb == "start" || verb == "stop" ||
        verb == "wait" || verb == "ipl" || verb == "get")
        return true;
    if (verb == "snapshot") return a.size() > 1 && equalsIgnoreCase(a[1], "load");
    // Ordinary trace-category changes only update an atomic logging mask.
    // Keep them off the guest-thread command queue: queueing rings the same
    // doorbell as a terminal and can otherwise advance a pending workstation
    // transition.  The two trace forms below touch guest-owned structures and
    // must still be marshalled at a safe instruction boundary.
    if (verb == "trace")
        return a.size() < 2 || (!equalsIgnoreCase(a[1], "workstation") && !equalsIgnoreCase(a[1], "member"));
    if (verb == "show")
        return a.size() > 1 && (equalsIgnoreCase(a[1], "config") ||
                                equalsIgnoreCase(a[1], "status") ||
                                equalsIgnoreCase(a[1], "terminal"));
    if (verb == "set") return a.size() > 1 && equalsIgnoreCase(a[1], "terminal");
    return false;
}

}  // namespace sim36::monitor
