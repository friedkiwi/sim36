#include "Storage/TapeBackend.h"

#include <algorithm>
#include <exception>

#include "Storage/TapeManifest.h"

namespace sim36::storage {

bool restoreTapePosition(ITapeBackend& tape, const TapePosition& wanted, std::string& reason)
{
    if (wanted.fileNumber < 0 || wanted.blockNumber < 0) {
        reason = "saved tape position has a negative file or block number";
        return false;
    }

    tape.rewind();
    int spaced = 0;
    if (wanted.fileNumber != 0) {
        TapeResult r = tape.spaceFiles(wanted.fileNumber, spaced);
        if (r != TapeResult::Ok || spaced != wanted.fileNumber) {
            reason = fmt::format("cannot restore tape file {}: {} after {} file(s)", wanted.fileNumber,
                                 tapeResultName(r), spaced);
            return false;
        }
    }
    if (wanted.blockNumber != 0) {
        TapeResult r = tape.spaceRecords(wanted.blockNumber, spaced);
        if (r != TapeResult::Ok || spaced != wanted.blockNumber) {
            reason = fmt::format("cannot restore tape block {} in file {}: {} after {} block(s)", wanted.blockNumber,
                                 wanted.fileNumber, tapeResultName(r), spaced);
            return false;
        }
    }

    TapePosition actual = tape.readPosition();
    if (actual.fileNumber != wanted.fileNumber || actual.blockNumber != wanted.blockNumber ||
        actual.atTapeMark != wanted.atTapeMark || actual.beginningOfTape != wanted.beginningOfTape ||
        actual.endOfData != wanted.endOfData) {
        reason = fmt::format("saved tape position {} reconstructs as {}", wanted.toString(), actual.toString());
        return false;
    }
    reason.clear();
    return true;
}

std::unique_ptr<TapeManifest> inspectTape(ITapeBackend& tape, std::string& reason)
{
    reason.clear();
    const bool wasLoaded = tape.loaded();
    const TapePosition saved = tape.readPosition();
    auto catalog = std::make_unique<TapeManifest>();
    catalog->volume.volumeId.clear();
    catalog->volume.ownerId.clear();
    catalog->volume.accessSecurity = " ";
    catalog->volume.labeled = false;

    std::vector<int> lengths;
    std::vector<std::vector<uint8_t>> captured;
    bool capturing = true;
    auto closeFile = [&] {
        TapeFileEntry file;
        file.sequence = static_cast<int>(catalog->files.size()) + 1;
        file.blob = "(container)";
        file.blockCount = static_cast<int>(lengths.size());
        bool uniform = !lengths.empty();
        for (std::size_t i = 1; i < lengths.size(); ++i) uniform = uniform && lengths[i] == lengths[0];
        if (uniform) {
            file.blockLength = lengths[0];
        } else {
            file.hasBlockLengths = true;
            file.blockLengths = lengths;
        }
        if (capturing && !captured.empty() && TapeLabel::decodeLabelGroup(captured, file.labels)) {
            file.kind = "label";
            file.recordFormat = "F";
            file.recordLength = 80;
        } else {
            file.kind = "data";
            file.recordFormat = "U";
            file.recordLength = uniform ? file.blockLength : 0;
        }
        catalog->files.push_back(std::move(file));
        lengths.clear();
        captured.clear();
        capturing = true;
    };

    bool inspected = false;
    try {
        if (!wasLoaded) tape.load();
        tape.rewind();
        for (;;) {
            std::vector<uint8_t> block;
            const TapeResult result = tape.readBlock(block);
            if (result == TapeResult::Ok || result == TapeResult::RecordError) {
                if (catalog->files.empty() && lengths.empty() && block.size() == 80 && block[0] == 0xE5 &&
                    block[1] == 0xD6 && block[2] == 0xD3 && block[3] == 0xF1) {
                    const auto vol1 = TapeLabel::decodeVol1(block, 0);
                    catalog->volume.volumeId = vol1.value("volumeId", std::string());
                    catalog->volume.ownerId = vol1.value("ownerId", std::string());
                    catalog->volume.accessSecurity = vol1.value("accessSecurity", std::string(" "));
                    catalog->volume.labeled = true;
                }
                lengths.push_back(static_cast<int>(block.size()));
                if (capturing && block.size() == 80 && captured.size() < 8)
                    captured.push_back(std::move(block));
                else
                    capturing = false;
                continue;
            }
            if (result == TapeResult::TapeMark) {
                closeFile();
                continue;
            }
            if (result == TapeResult::EndOfData) {
                if (!lengths.empty()) closeFile();
                inspected = true;
                break;
            }
            reason = std::string("cannot inspect tape: ") + tapeResultName(result);
            break;
        }
    } catch (const std::exception& e) {
        reason = e.what();
    }

    std::string restoreReason;
    bool restored = true;
    if (wasLoaded)
        restored = restoreTapePosition(tape, saved, restoreReason);
    else
        tape.unload();
    if (!restored) {
        reason = "tape was inspected but its position could not be restored: " + restoreReason;
        return nullptr;
    }
    return inspected ? std::move(catalog) : nullptr;
}

}  // namespace sim36::storage
