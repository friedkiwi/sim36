#include "Host/Console.h"

#include <cstdio>
#include <iostream>

#include <replxx.hxx>

#ifdef _WIN32
#include <io.h>
#define SIM36_ISATTY _isatty
#define SIM36_FILENO _fileno
#else
#include <unistd.h>
#define SIM36_ISATTY isatty
#define SIM36_FILENO fileno
#endif

namespace sim36::host {

struct Console::Impl {
    replxx::Replxx rx;
};

Console::Console() : impl_(isInteractive() ? new Impl() : nullptr) {}

Console::~Console() { delete impl_; }

bool Console::isInteractive()
{
    return SIM36_ISATTY(SIM36_FILENO(stdin)) != 0;
}

std::optional<std::string> Console::readLine(const std::string& prompt)
{
    if (impl_ != nullptr) {
        const char* line = impl_->rx.input(prompt);
        if (line == nullptr) return std::nullopt;
        std::string result(line);
        if (!result.empty()) impl_->rx.history_add(result);
        return result;
    }
    std::fputs(prompt.c_str(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace sim36::host
