// The monitor-level chassis.  The editable definition and socket listeners
// exist independently; IPL latches them into a lazily constructed machine.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Host/StationBackend.h"
#include "Host/StationMultiplexer.h"
#include "Monitor/Tracer.h"

namespace sim36::monitor {

// An operator error: bad arguments, unknown command, refused operation.
// Reported as "error: <message>" interactively and as "<file>:<line>:
// <message>" from a command file.
class MonitorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A command file stopped at a line.  The message already carries the file
// name and line number.
class CommandFileError : public std::runtime_error {
public:
    CommandFileError(const std::string& file, int line, const std::string& message)
        : std::runtime_error(file + ":" + std::to_string(line) + ": " + message) {}
};

class SimulatorSession {
public:
    // A session whose startup command file has not yet been applied.
    // Services are activated by activateConfiguredServices() after the
    // complete definition is known.
    SimulatorSession();
    // A session from a legacy INI definition: listeners come up at once.
    explicit SimulatorSession(configuration::EmulatorConfig definition);
    ~SimulatorSession();
    SimulatorSession(const SimulatorSession&) = delete;
    SimulatorSession& operator=(const SimulatorSession&) = delete;

    void activateConfiguredServices();

    // The interactive read-execute loop on standard input.
    void run();

    // Executes every line of a command file.  With echo, each line is printed
    // behind the prompt first, as a scripted transcript.
    void executeFile(const std::string& path, bool echo);

    void execute(const std::string& line);
    void executeTokens(std::vector<std::string> tokens);

    bool quitRequested() const { return quitRequested_; }

    // Lexes every line of a command file and looks each verb up in the
    // registry, following `do` includes, without executing anything.  Prints
    // one line per problem and returns false when there was one.
    static bool checkFile(const std::string& path);
    const configuration::EmulatorConfig& definition() const { return definition_; }

private:
    using Args = std::vector<std::string>;

    void executeTokensCore(const Args& a);
    void constructMachine();
    void releaseMachine();
    void resetMachine(const Args& a);
    void setDefinition(const Args& a);
    void setTerminal(const Args& a);
    void setMachine(const Args& a);
    void setDevice(const Args& a);
    void setStation(const Args& a);
    void setMultiplex(const Args& a);
    void showConfigurableStations();
    void showStatus();
    void getTerminalConfiguration();
    void showTerminals();
    void reconcileListeners();
    void disposeStationBackends();
    void startMultiplexer();
    void reportMultiplexer();
    void stopMultiplexer();
    int multiplexDisplayStations() const;
    void removeStation(const Args& a);
    void media(const Args& a);
    static void reportVolume(const std::string& path);
    void saveConfig(const Args& a);
    std::string resolvePath(const std::string& path) const;
    configuration::StationConfig& findOrCreateStation(const std::string& id);
    configuration::StationConfig* findStation(const std::string& id);
    void requireConfigurable() const;
    bool machineConstructed() const { return false; }
    bool inputRedirected() const;

    configuration::EmulatorConfig definition_;
    bool quitRequested_ = false;
    std::vector<std::filesystem::path> sourceDirectories_;
    std::set<std::string> activeFiles_;
    std::map<std::string, std::unique_ptr<host::StationBackend>> stationBackends_;
    Tracer listenerTrace_;
    Tracer multiplexerTrace_;
    std::unique_ptr<host::StationMultiplexer> multiplexer_;
    uint32_t pendingTrace_ = TraceNone;
};

}  // namespace sim36::monitor
