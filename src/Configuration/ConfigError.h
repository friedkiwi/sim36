#pragma once

#include <stdexcept>
#include <string>

namespace sim36::configuration {

// A definition error.  The message carries "file:line: " when a line is
// known and "file: " otherwise, exactly as the reference reported them.
class ConfigError : public std::runtime_error {
public:
    ConfigError(const std::string& file, int line, const std::string& message)
        : std::runtime_error(line > 0 ? file + ":" + std::to_string(line) + ": " + message
                                      : file + ": " + message) {}
};

}  // namespace sim36::configuration
