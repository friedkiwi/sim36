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

bool initializeTape(const std::string& path, const std::string& volumeId, const std::string& ownerId,
                    std::string& reason)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return FolderTapeBackend::init(path, volumeId, ownerId, reason);

    auto tape = SimhTapeBackend::open(path, false, reason);
    if (!tape) return false;
    try {
        tape->load();
        tape->rewind();
        const std::vector<uint8_t> vol1 = TapeLabel::renderVol1(volumeId, ' ', ownerId);
        if (tape->writeBlock(vol1.data(), 0, static_cast<int>(vol1.size())) != TapeResult::Ok ||
            tape->writeTapeMark() != TapeResult::Ok || tape->writeTapeMark() != TapeResult::Ok) {
            reason = "could not write the initial SIMH tape labels";
            return false;
        }
        tape->unload();
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
}

}  // namespace sim36::storage
