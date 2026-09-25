#pragma once

#include <memory>
#include <string>

#include "Storage/TapeBackend.h"

namespace sim36::storage {

// Select solely by host object type: directories use the legacy folder
// container and every other path uses SIMH TAP.  A missing writable path is
// consequently created as an empty TAP file.
std::unique_ptr<ITapeBackend> openTapeBackend(const std::string& path, bool readOnly, std::string& reason);

// Create a fresh standard-labeled volume.  Directories select the legacy
// folder representation; every other path is written as SIMH TAP.
bool initializeTape(const std::string& path, const std::string& volumeId, const std::string& ownerId,
                    std::string& reason);

}  // namespace sim36::storage
