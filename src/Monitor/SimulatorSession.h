// The monitor-level chassis.  The editable definition and socket listeners
// exist independently; IPL latches them into a lazily constructed machine.
#pragma once

#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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
    SimulatorSession();

    // The interactive read-execute loop on standard input.
    void run();

    // Executes every line of a command file.  With echo, each line is printed
    // behind the prompt first, as a scripted transcript.
    void executeFile(const std::string& path, bool echo);

    void execute(const std::string& line);
    void executeTokens(std::vector<std::string> tokens);

    bool quitRequested() const { return quitRequested_; }

private:
    void executeTokensCore(const std::vector<std::string>& a);
    std::string resolvePath(const std::string& path) const;

    bool quitRequested_ = false;
    std::vector<std::filesystem::path> sourceDirectories_;
    std::set<std::string> activeFiles_;
};

}  // namespace sim36::monitor
