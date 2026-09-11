// Operator line input.  Interactive terminals get line editing and history
// through replxx; redirected input is read verbatim so that a scripted session
// produces exactly the bytes a diff expects.
#pragma once

#include <optional>
#include <string>

namespace sim36::host {

class Console {
public:
    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Prints the prompt and reads one line; nullopt at end of input.
    std::optional<std::string> readLine(const std::string& prompt);

    // True when standard input is a terminal.
    static bool isInteractive();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sim36::host
