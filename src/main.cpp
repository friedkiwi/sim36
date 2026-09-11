// sim36 - the SIM/36 System/36 emulator, command-line entry point.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmt/core.h>

#include "Monitor/SimulatorSession.h"

namespace {

void usage()
{
    fmt::print("sim36 [-c startup-file] [-s script] [-t classes]\n"
               "\n"
               "  -c, --config FILE   startup command file (default etc/sim36.sim)\n"
               "  -s, --script FILE   run monitor commands from FILE and exit\n"
               "  -t, --trace LIST    msp,svc,csp,disk,ws,ace,sched,all\n");
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
            ("h,help", "show this help");
        cxxopts::ParseResult result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            usage();
            return 0;
        }
        if (!result.unmatched().empty()) {
            fmt::print(stderr, "unknown option {}\n", result.unmatched().front());
            usage();
            return 2;
        }
        if (result.count("config") != 0) {
            cfgPath = result["config"].as<std::string>();
            cfgGiven = true;
        }
        if (result.count("script") != 0) script = result["script"].as<std::string>();
        if (result.count("trace") != 0) trace = result["trace"].as<std::string>();
    } catch (const cxxopts::exceptions::exception& e) {
        fmt::print(stderr, "{}\n", e.what());
        usage();
        return 2;
    }

    try {
        sim36::monitor::SimulatorSession session;
        if (cfgGiven || !cfgPath.empty()) session.executeFile(cfgPath, false);
        if (!trace.empty()) session.execute("trace " + trace);
        if (!script.empty())
            session.executeFile(script, true);
        else if (!session.quitRequested())
            session.run();
        std::fflush(stdout);
        std::fflush(stderr);
        return 0;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        fmt::print(stderr, "{}\n", e.what());
        return 2;
    }
}
