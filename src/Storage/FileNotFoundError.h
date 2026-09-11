// A host file that should exist does not.  what() carries the reference
// runtime's wording, `Could not find file "<path>"`, which is what a command
// file reports; the entry point reports `not found: <path>` when the file was
// the startup file or script itself.
#pragma once

#include <stdexcept>
#include <string>

namespace sim36::storage {

class FileNotFoundError : public std::runtime_error {
public:
    explicit FileNotFoundError(const std::string& path)
        : std::runtime_error("Could not find file \"" + path + "\""), path_(path) {}
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

}  // namespace sim36::storage
