// A host file that should exist does not.  what() carries the reference
// runtime's wording, `Could not find file "<path>"`, which is what a command
// file reports; the entry point reports `not found: <path>` when the file was
// the startup file or script itself.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace sim36::storage {

class FileNotFoundError : public std::runtime_error {
public:
    // `directoryMissing` selects the wording for a path whose parent
    // directory does not exist either, as the reference runtime reported it.
    explicit FileNotFoundError(const std::string& path, bool directoryMissing = false)
        : std::runtime_error(directoryMissing ? "Could not find a part of the path \"" + path + "\"."
                                              : "Could not find file \"" + path + "\""),
          path_(path) {}
    const std::string& path() const { return path_; }

    // Builds the error for `path` (made absolute), choosing the wording by
    // whether its parent directory exists.
    static FileNotFoundError forPath(const std::string& path);

private:
    std::string path_;
};

inline FileNotFoundError FileNotFoundError::forPath(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path full = std::filesystem::absolute(path, ec);
    if (ec) return FileNotFoundError(path);
    full = full.lexically_normal();
    const bool directoryMissing = !std::filesystem::is_directory(full.parent_path(), ec);
    return FileNotFoundError(full.string(), directoryMissing);
}

}  // namespace sim36::storage
