// The host end of one tape drive: a generic, SCSI-like tape target.
//
// Primitive, not semantic, the same rule the display and printer seams
// hold: nothing here names a System/36 IOB, an SVC, a command code or a
// completion byte.  It knows about blocks, tape marks and position, a tape
// drive's own vocabulary, and nothing above it.  The device model maps the
// guest's tape IOB onto these operations and translates a TapeResult back
// into a completion byte; this interface is the seam it calls across.
//
// The operation set is the one a tape driver uses over its adapter: LOAD /
// UNLOAD, REWIND, READ / WRITE a block, WRITE FILEMARKS, SPACE (by record and
// by file) and READ POSITION.  A concrete backend supplies the storage; the
// folder format is FolderTapeBackend, and other containers can implement the
// same interface later.
//
// Positioning model: the tape is a linear sequence of blocks with tape marks
// between files, exactly as hardware sees it.  Read and forward-space stop ON
// a tape mark and report it (the mark is then crossed by the next
// operation); the "two marks in a row" logical end of tape surfaces as
// EndOfData.  Writing follows tape semantics: a WRITE or WRITE FILEMARK that
// is not at end of data logically erases everything ahead of it
// (append-forward), which is how a save writes a volume.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace sim36::storage {

class TapeManifest;

// The result of one tape operation: the drive's answer, at the level a
// SCSI tape target answers it.  Intentionally not the System/36 IOB
// completion byte.
enum class TapeResult {
    Ok,                // the block or space completed normally
    TapeMark,          // a tape mark was encountered (a read: the file ended; a space: stopped early)
    EndOfData,         // no more recorded data ahead
    BeginningOfTape,   // a backward operation reached load point
    WriteProtected,    // a write was refused because the volume is write-protected
    NotReady,          // no volume is loaded, or it is not spun up
    RecordError,       // a SIMH record carries the recorded-data error flag
    InvalidArgument,   // the requested block cannot be represented by this drive
};

inline const char* tapeResultName(TapeResult r)
{
    switch (r) {
        case TapeResult::Ok: return "Ok";
        case TapeResult::TapeMark: return "TapeMark";
        case TapeResult::EndOfData: return "EndOfData";
        case TapeResult::BeginningOfTape: return "BeginningOfTape";
        case TapeResult::WriteProtected: return "WriteProtected";
        case TapeResult::NotReady: return "NotReady";
        case TapeResult::RecordError: return "RecordError";
        case TapeResult::InvalidArgument: return "InvalidArgument";
    }
    return "?";
}

// Where the head is, reported the way READ POSITION reports it: the tape
// file number and the block within it.  "File" here is the SCSI sense, a
// region between two tape marks, not a dataset, which spans three such
// regions (its HDR labels, its data, its EOF labels).
struct TapePosition {
    int fileNumber = 0;          // 0-based index of the tape file the head is in
    int blockNumber = 0;         // 0-based index of the next block within the current file
    bool atTapeMark = false;     // the head sits on the mark that closes the current file
    bool beginningOfTape = false;
    bool endOfData = false;      // past the last recorded block and mark

    std::string toString() const
    {
        return fmt::format("file {} block {}{}{}{}", fileNumber, blockNumber, atTapeMark ? " @mark" : "",
                           beginningOfTape ? " BOT" : "", endOfData ? " EOD" : "");
    }
};

class ITapeBackend {
public:
    virtual ~ITapeBackend() = default;

    // The folder (or container) backing this drive, for traces.
    virtual const std::string& path() const = 0;
    // The volume cannot be written.
    virtual bool readOnly() const = 0;
    // A cartridge is present AND spun up.  LOAD makes a present cartridge
    // ready; UNLOAD takes it offline.  Operations other than load() answer
    // NotReady when this is false.
    virtual bool loaded() const = 0;
    // The VOL1 volume serial, or empty when the cartridge carries no
    // standard volume label.
    virtual std::string volumeId() const = 0;

    // SCSI LOAD: spin up the present cartridge and position it at load point.
    virtual void load() = 0;
    // SCSI UNLOAD: rewind and take the cartridge offline.  It stays
    // openable; load() brings it back.
    virtual void unload() = 0;
    // SCSI REWIND: position the head at load point (file 0, block 0).
    virtual void rewind() = 0;

    // Read the block at the head and advance past it.  On success `data` is
    // the block's bytes and the result Ok.  When the head is on a tape mark
    // the mark is crossed, `data` is empty and the result TapeMark.  Past the
    // last data the result is EndOfData.
    virtual TapeResult readBlock(std::vector<uint8_t>& data) = 0;
    // Write one block at the head.  Append-forward: if the head is not at
    // end of data, everything ahead of it is discarded first.  Refused with
    // WriteProtected on a read-only volume.
    virtual TapeResult writeBlock(const uint8_t* data, int offset, int length) = 0;
    // SCSI WRITE FILEMARK: write one tape mark at the head, closing the
    // current tape file.  Append-forward, like writeBlock().
    virtual TapeResult writeTapeMark() = 0;
    // SPACE by record: move `count` blocks forward (positive) or backward
    // (negative).  Stops early on a tape mark, at load point, or at end of
    // data; `spaced` returns how many blocks were actually skipped and the
    // result says why it stopped (Ok when the full count was consumed).
    virtual TapeResult spaceRecords(int count, int& spaced) = 0;
    // SPACE by file: move across `count` tape marks forward (positive) or
    // backward (negative), leaving the head just past the mark (forward) or
    // just before it (backward).  `spaced` returns how many marks were
    // crossed; the result is EndOfData or BeginningOfTape when it ran out
    // of tape first.
    virtual TapeResult spaceFiles(int count, int& spaced) = 0;
    // SCSI READ POSITION: the current head position.
    virtual TapePosition readPosition() const = 0;
};

// Reconstruct an exact saved position using only portable tape primitives.
// A different or truncated cartridge must not be accepted as restored.
bool restoreTapePosition(ITapeBackend& tape, const TapePosition& wanted, std::string& reason);

// Inspect the linear medium without changing its ready state or head
// position.  The returned catalog uses TapeManifest's existing typed volume
// and file descriptions but is derived from actual records, not a folder's
// manifest.json.
std::unique_ptr<TapeManifest> inspectTape(ITapeBackend& tape, std::string& reason);

}  // namespace sim36::storage
