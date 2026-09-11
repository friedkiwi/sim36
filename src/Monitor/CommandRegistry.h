// The public monitor command surface.  Parsing, aliases, lifecycle errors and
// generated help all use this table; command handlers contain no additional
// public spellings.
#pragma once

#include <string>
#include <vector>

namespace sim36::monitor {

enum class Lifecycle { Any, Machine, ConstructsMachine };

struct Command {
    const char* name;
    Lifecycle lifecycle;
    const char* group;
    const char* usage;
    const char* summary;
    std::vector<const char*> aliases;
};

class CommandRegistry {
public:
    // Case-insensitive lookup by name or alias; nullptr when unknown.
    static const Command* find(const std::string& name);

    // Replaces an alias in tokens[0] by the canonical command name.
    static std::vector<std::string> canonicalize(std::vector<std::string> tokens);

    static const std::vector<Command>& all();

    static bool isMachineShowTarget(const std::string& target);
    static bool isShowTarget(const std::string& target);

    // Renders the grouped help listing to stdout.
    static void printHelp();

    // Commands which must run on the shell thread because they control or
    // join execution, recurse through command files, or may prompt.
    static bool isHostControl(const std::vector<std::string>& a);
};

// Case-insensitive ASCII string equality, the monitor's `Eq`.
bool equalsIgnoreCase(const std::string& a, const std::string& b);
std::string toLower(std::string s);

}  // namespace sim36::monitor
