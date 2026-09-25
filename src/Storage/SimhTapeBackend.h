// A SIMH .tap container exposed as a generic tape backend.
//
// Each data record is stored as a 32-bit little-endian length, its bytes,
// one padding byte when the length is odd, and the same length again.  A
// zero length word is a tape mark.  The SIMH error bit is retained in the
// index and reported as TapeResult::RecordError when that record is read.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Storage/TapeBackend.h"

namespace sim36::storage {

class SimhTapeBackend : public ITapeBackend {
public:
    static constexpr std::uint32_t kTapeMark = 0x00000000U;
    static constexpr std::uint32_t kEndOfMedium = 0xFFFFFFFFU;
    static constexpr std::uint32_t kEraseGap = 0xFFFFFFFEU;
    static constexpr std::uint32_t kErrorFlag = 0x80000000U;
    static constexpr std::uint32_t kLengthMask = 0x00FFFFFFU;

    // A missing writable path is created as an empty tape.  A zero-length
    // regular file is already a valid empty tape.  Directories are refused;
    // backend selection belongs to openTapeBackend().
    static std::unique_ptr<SimhTapeBackend> open(const std::string& path, bool readOnly, std::string& reason);

    const std::string& path() const override { return path_; }
    bool readOnly() const override { return readOnly_; }
    bool loaded() const override { return loaded_; }
    std::string volumeId() const override { return volumeId_; }

    void load() override;
    void unload() override;
    void rewind() override;
    TapeResult readBlock(std::vector<uint8_t>& data) override;
    TapeResult writeBlock(const uint8_t* data, int offset, int length) override;
    TapeResult writeTapeMark() override;
    TapeResult spaceRecords(int count, int& spaced) override;
    TapeResult spaceFiles(int count, int& spaced) override;
    TapePosition readPosition() const override;

    // Persist pending writes as a complete validated TAP stream.  Existing
    // source records are copied lazily and the destination is replaced only
    // after the temporary stream is complete.
    void flush();

private:
    struct Item {
        bool mark = false;
        bool recordError = false;
        bool inMemory = false;
        std::uint64_t offset = 0;
        std::uint32_t length = 0;
        std::vector<uint8_t> bytes;

        static Item tapeMark();
        static Item source(std::uint64_t offset, std::uint32_t length, bool recordError);
        static Item memory(std::vector<uint8_t> bytes);
    };

    SimhTapeBackend(std::string path, bool readOnly) : path_(std::move(path)), readOnly_(readOnly) {}

    bool index(std::string& reason);
    bool readItem(const Item& item, std::vector<uint8_t>& data, std::string& reason) const;
    void truncateForward();
    bool forwardToAfterNextMark();
    bool backwardToStartOfFile();
    void discoverVolumeId();

    std::string path_;
    bool readOnly_ = false;
    bool loaded_ = false;
    bool dirty_ = false;
    std::string volumeId_;
    std::vector<Item> items_;
    std::size_t cursor_ = 0;
};

}  // namespace sim36::storage
