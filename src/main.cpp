// sim36 - the SIM/36 System/36 emulator, command-line entry point.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmt/format.h>

#include "Configuration/EmulatorConfig.h"
#include "Storage/FileNotFoundError.h"
#include "Monitor/SimulatorSession.h"

namespace {

void usage()
{
    fmt::print("sim36 [-c startup-file] [-s script] [-t classes]\n"
               "\n"
               "  -c, --config FILE   startup command file (default etc/sim36.sim)\n"
               "  -s, --script FILE   run monitor commands from FILE and exit\n"
               "  -t, --trace LIST    msp,svc,csp,disk,ws,ace,sched,all\n"
               "      --check FILE... report unknown commands in each FILE (and the files it\n"
               "                      includes with 'do') without executing anything\n");
}

// A startup file whose first meaningful line begins with '[' is a legacy INI
// definition; everything else is a monitor command file.
bool looksLikeLegacyConfig(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw sim36::storage::FileNotFoundError::forPath(path);
    std::string raw;
    while (std::getline(in, raw)) {
        std::size_t b = 0;
        while (b < raw.size() && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
        if (b == raw.size() || raw[b] == '#' || raw[b] == ';') continue;
        return raw[b] == '[';
    }
    return false;
}

// The default startup file is used only when it exists, so a fresh checkout
// starts with the built-in machine definition.
std::string defaultStartupFile()
{
    const char* candidate = "etc/sim36.sim";
    return std::filesystem::exists(candidate) ? candidate : std::string();
}

}  // namespace

int main(int argc, char** argv)
{
    std::string cfgPath = defaultStartupFile();
    std::string script;
    std::string trace;
    bool cfgGiven = false;

    try {
        cxxopts::Options options("sim36", "SIM/36 - System/36 emulator");
        options.add_options()
            ("c,config", "startup command file", cxxopts::value<std::string>())
            ("s,script", "run monitor commands from FILE and exit", cxxopts::value<std::string>())
            ("t,trace", "initial trace classes", cxxopts::value<std::string>())
            ("check", "check that command files parse, without executing them")
            ("files", "files", cxxopts::value<std::vector<std::string>>())
            ("h,help", "show this help");
        options.parse_positional({"files"});
        cxxopts::ParseResult result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            usage();
            return 0;
        }
        if (!result.unmatched().empty() ||
            (result.count("files") != 0 && result.count("check") == 0)) {
            fmt::print(stderr, "unknown option {}\n", !result.unmatched().empty()
                       ? result.unmatched().front()
                       : result["files"].as<std::vector<std::string>>().front());
            usage();
            return 2;
        }
        if (result.count("config") != 0) {
            cfgPath = result["config"].as<std::string>();
            cfgGiven = true;
        }
        if (result.count("script") != 0) script = result["script"].as<std::string>();
        if (result.count("trace") != 0) trace = result["trace"].as<std::string>();
        if (result.count("check") != 0) {
            if (result.count("files") == 0) {
                fmt::print(stderr, "--check needs at least one command file\n");
                return 2;
            }
            int failures = 0;
            for (const std::string& file : result["files"].as<std::vector<std::string>>())
                failures += sim36::monitor::SimulatorSession::checkFile(file) ? 0 : 1;
            return failures == 0 ? 0 : 1;
        }
    } catch (const cxxopts::exceptions::exception& e) {
        fmt::print(stderr, "{}\n", e.what());
        usage();
        return 2;
    }

    try {
        std::unique_ptr<sim36::monitor::SimulatorSession> session;
        const bool haveStartup = cfgGiven || !cfgPath.empty();
        if (haveStartup && looksLikeLegacyConfig(cfgPath)) {
            session = std::make_unique<sim36::monitor::SimulatorSession>(
                sim36::configuration::EmulatorConfig::load(cfgPath));
        } else {
            session = std::make_unique<sim36::monitor::SimulatorSession>();
            if (haveStartup) session->executeFile(cfgPath, false);
            session->activateConfiguredServices();
        }
        // Kept as a compatibility option while configuration lives in the
        // monitor language.  New command files use `trace`.
        if (!trace.empty()) session->execute("trace " + trace);
        if (!script.empty())
            session->executeFile(script, true);
        else if (!session->quitRequested())
            session->run();
        std::fflush(stdout);
        std::fflush(stderr);
        return 0;
    } catch (const sim36::storage::FileNotFoundError& e) {
        std::fflush(stdout);
        fmt::print(stderr, "not found: {}\n", e.path());
        return 2;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        fmt::print(stderr, "{}\n", e.what());
        return 2;
    }
}
