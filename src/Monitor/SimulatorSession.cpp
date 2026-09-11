#include "Monitor/SimulatorSession.h"

#include <fstream>
#include <iostream>

#include <fmt/core.h>

#include "Host/Console.h"
#include "Monitor/CommandLine.h"
#include "Monitor/CommandRegistry.h"

namespace sim36::monitor {

namespace {

const char* const kPrompt = "sim36> ";

void need(const std::vector<std::string>& a, std::size_t count, const char* usage)
{
    if (a.size() < count) throw MonitorError(usage);
}

}  // namespace

SimulatorSession::SimulatorSession() = default;

void SimulatorSession::run()
{
    fmt::print("SIM/36 - System/36 emulator\n");
    fmt::print("type 'help' for commands, 'quit' to exit\n");
    host::Console console;
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
    namespace fs = std::filesystem;
    const std::string resolved = resolvePath(path);
    std::error_code ec;
    fs::path full = fs::absolute(resolved, ec);
    if (ec) full = fs::path(resolved);
    full = full.lexically_normal();
    const std::string key = full.string();

    std::ifstream in(full);
    if (!in) throw MonitorError("not found: " + resolved);
    if (!activeFiles_.insert(key).second)
        throw MonitorError("recursive command file: " + key);
    sourceDirectories_.push_back(full.parent_path());
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
    executeTokensCore(a);
}

void SimulatorSession::executeTokensCore(const std::vector<std::string>& a)
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
            "'listener-auto-signon off').");
    if (command == nullptr)
        throw MonitorError("unknown command '" + a[0] + "' - try help");

    if (verb == "quit") {
        quitRequested_ = true;
        return;
    }
    if (verb == "help") {
        CommandRegistry::printHelp();
        return;
    }
    if (verb == "do") {
        need(a, 2, "do <command-file>");
        executeFile(a[1], true);
        return;
    }

    // Nothing else exists yet.  The refusals below are the reference's own
    // "no machine constructed" answers, which is exactly the situation a
    // skeleton is in.
    if (verb == "show") {
        if (a.size() > 1 && CommandRegistry::isMachineShowTarget(a[1]))
            throw MonitorError("'show " + a[1] +
                               "' requires a constructed machine; use 'ipl' or 'ipl pause' first");
        throw MonitorError(a.size() < 2
            ? "show what? config|status|terminal (guest-state views require IPL)"
            : "unknown show target '" + a[1] + "'; use 'show status', "
              "'show terminal', or 'show config'");
    }
    if (command->lifecycle == Lifecycle::Machine)
        throw MonitorError("'" + a[0] + "' requires a constructed machine; use 'ipl' or 'ipl pause' first");
    throw MonitorError("command '" + a[0] + "' is incomplete or unavailable before IPL - try help");
}

std::string SimulatorSession::resolvePath(const std::string& path) const
{
    namespace fs = std::filesystem;
    if (fs::path(path).is_absolute() || sourceDirectories_.empty()) return path;
    return (sourceDirectories_.back() / path).lexically_normal().string();
}

}  // namespace sim36::monitor
