#pragma once

#include <memory>
#include <string>

#include "Storage/TapeBackend.h"

namespace sim36::storage {

// Select solely by host object type: directories use the legacy folder
// container and every other path uses SIMH TAP.  A missing writable path is
// consequently created as an empty TAP file.
std::unique_ptr<ITapeBackend> openTapeBackend(const std::string& path, bool readOnly, std::string& reason);

}  // namespace sim36::storage
