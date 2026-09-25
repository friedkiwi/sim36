// A virtual tape whose container is a folder: a JSON manifest plus numbered
// data blobs, one per tape file (the region between two tape marks).
//
// This is the original, human-inspectable representation retained for
// compatibility and authoring workflows.  New regular-file media uses
// SimhTapeBackend; backend selection is made solely from directory vs file.
// The block/tape-mark semantics are identical across both representations.
//
// On-disk layout:
//   mytape/
//     manifest.json      volume identity + ordered list of tape files
//     0001.dat           tape file 1's blocks, concatenated
//     0002.dat           tape file 2's blocks, concatenated
// A "tape file" is the SCSI sense, a run of blocks between two tape marks,
// not a dataset: a standard-labeled dataset occupies THREE tape files (its
// HDR label group as 80-byte blocks, its data, its EOF label group).  The
// label records are stored as real blocks in real blobs, because on the
// medium they ARE blocks the drive reads; the manifest's decoded labels are
// a convenience mirror.
//
// Blob granularity is one blob per tape file, not per block: a real save
// writes tens of thousands of blocks per dataset.  The manifest's per-block
// length list (or a fixed block length and count) keeps every block boundary
// recoverable.
//
// Runtime model: on load() the tape is expanded into a flat list of items,
// each a data block or a tape mark, with block bytes read lazily from the
// blobs (only written blocks are held in memory).  The head is a cursor into
// that list.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Storage/TapeBackend.h"
#include "Storage/TapeManifest.h"

namespace sim36::storage {

class FolderTapeBackend : public ITapeBackend {
public:
    static constexpr const char* kManifestName = "manifest.json";
    static constexpr const char* kFormatId = "s36-folder-tape";
    static constexpr int kFormatVersion = 1;
    // Every standard tape label record is 80 bytes.
    static constexpr int kLabelLength = 80;

    // Open a tape folder and index it.  Null with a reason (never throws for
    // a bad container), because "that is not a tape folder" is an answer the
    // operator should see, the same contract DisketteBackend::open holds.
    static std::unique_ptr<FolderTapeBackend> open(const std::string& folder, bool readOnly, std::string& reason);

    // Write a fresh, empty, standard-labeled tape folder: a VOL1 volume label
    // and nothing else, a newly initialised cartridge.  False with a reason
    // on failure.
    static bool init(const std::string& folder, const std::string& volumeId, const std::string& ownerId,
                     std::string& reason);

    const std::string& path() const override { return folder_; }
    bool readOnly() const override { return readOnly_; }
    bool loaded() const override { return loaded_; }
    std::string volumeId() const override { return manifest_.volume.volumeId; }

    void load() override;
    void unload() override;
    void rewind() override;
    TapeResult readBlock(std::vector<uint8_t>& data) override;
    TapeResult writeBlock(const uint8_t* data, int offset, int length) override;
    TapeResult writeTapeMark() override;
    TapeResult spaceRecords(int count, int& spaced) override;
    TapeResult spaceFiles(int count, int& spaced) override;
    TapePosition readPosition() const override;

    // Rewrite the folder from the current item list: one blob per tape file,
    // then the manifest.  Blocks are streamed, so the whole tape is never
    // held in memory at once; new blobs are written under temporary names
    // and swapped in, so a failure mid-write cannot leave a half-rewritten
    // blob in place of a good one.  Throws std::runtime_error on a
    // read-only tape or a host I/O failure.
    void flush();

private:
    // One element of the flat tape image: a data block or a tape mark.  A
    // block is either in memory (freshly written) or a slice of a blob file
    // (read lazily), so an unmodified tape costs almost no memory to load.
    struct TapeItem {
        bool isMark = false;
        bool inMemory = false;
        std::vector<uint8_t> mem;
        std::string blob;
        long long offset = 0;
        int length = 0;

        static TapeItem mark();
        static TapeItem fromMemory(std::vector<uint8_t> bytes);
        static TapeItem fromBlob(const std::string& blob, long long offset, int length);
        // Throws std::runtime_error when the blob is shorter than the
        // manifest declares.
        std::vector<uint8_t> read(const std::string& folder) const;
    };

    FolderTapeBackend(const std::string& folder, bool readOnly, TapeManifest manifest)
        : folder_(folder), readOnly_(readOnly), manifest_(std::move(manifest)) {}

    void truncateForward();
    bool forwardToAfterNextMark();
    bool backwardToStartOfFile();
    static TapeFileEntry buildFileEntry(int seq, const std::string& blob, const std::vector<int>& lengths,
                                        const std::vector<std::vector<uint8_t>>* captured);

    std::string folder_;
    bool readOnly_;
    TapeManifest manifest_;
    std::vector<TapeItem> items_;
    std::size_t cursor_ = 0;   // index into items_ of the next block/mark
    bool loaded_ = false;
    bool dirty_ = false;       // items changed since the last flush
};

}  // namespace sim36::storage
