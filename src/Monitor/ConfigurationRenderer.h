// The two deliberate representations of an emulator definition.
//
// Human output describes the effective values without pretending to be a
// command file.  Replay output contains only monitor commands and can be
// used as a startup file for a fresh session.
#pragma once

#include <string>

#include "Configuration/EmulatorConfig.h"

namespace sim36::monitor {

class ConfigurationRenderer {
public:
    static std::string renderHuman(const configuration::EmulatorConfig& config, bool latched);
    static std::string renderReplay(const configuration::EmulatorConfig& config);

    // Quote one argument for CommandLine::tokenize, including the empty
    // string, apostrophe and backslash cases.
    static std::string quoteArgument(const std::string& value);
};

}  // namespace sim36::monitor
