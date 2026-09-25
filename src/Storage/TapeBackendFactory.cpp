#include "Storage/TapeBackendFactory.h"

#include <filesystem>

#include "Storage/FolderTapeBackend.h"
#include "Storage/SimhTapeBackend.h"

namespace sim36::storage {

std::unique_ptr<ITapeBackend> openTapeBackend(const std::string& path, bool readOnly, std::string& reason)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return FolderTapeBackend::open(path, readOnly, reason);
    return SimhTapeBackend::open(path, readOnly, reason);
}

}  // namespace sim36::storage
