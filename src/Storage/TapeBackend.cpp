#include "Storage/TapeBackend.h"

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

}  // namespace sim36::storage
