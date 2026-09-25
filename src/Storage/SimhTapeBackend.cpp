#include "Storage/SimhTapeBackend.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <fmt/format.h>

#include "Storage/TapeManifest.h"

namespace sim36::storage {

namespace fs = std::filesystem;

namespace {

bool readWord(std::istream& in, std::uint32_t& value)
{
    unsigned char b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    if (in.gcount() != 4) return false;
    value = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
            (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

void writeWord(std::ostream& out, std::uint32_t value)
{
    const unsigned char b[4] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
                                static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    out.write(reinterpret_cast<const char*>(b), 4);
}

void replaceFile(const fs::path& source, const fs::path& destination)
{
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return;
    throw std::runtime_error(
        std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
#else
    if (std::rename(source.c_str(), destination.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
#endif
}

}  // namespace

SimhTapeBackend::Item SimhTapeBackend::Item::tapeMark()
{
    Item item;
    item.mark = true;
    return item;
}

SimhTapeBackend::Item SimhTapeBackend::Item::source(std::uint64_t offset, std::uint32_t length, bool recordError)
{
    Item item;
    item.offset = offset;
    item.length = length;
    item.recordError = recordError;
    return item;
}

SimhTapeBackend::Item SimhTapeBackend::Item::memory(std::vector<uint8_t> bytes)
{
    Item item;
    item.inMemory = true;
    item.length = static_cast<std::uint32_t>(bytes.size());
    item.bytes = std::move(bytes);
    return item;
}

std::unique_ptr<SimhTapeBackend> SimhTapeBackend::open(const std::string& path, bool readOnly, std::string& reason)
{
    reason.clear();
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        reason = path + " is a directory, not a SIMH tape file";
        return nullptr;
    }
    if (!fs::exists(path, ec)) {
        if (readOnly) {
            reason = path + " does not exist";
            return nullptr;
        }
        std::ofstream create(path, std::ios::binary | std::ios::app);
        if (!create) {
            reason = std::strerror(errno);
            return nullptr;
        }
    }
    if (!fs::is_regular_file(path, ec)) {
        reason = path + " is not a regular file";
        return nullptr;
    }

    auto tape = std::unique_ptr<SimhTapeBackend>(new SimhTapeBackend(path, readOnly));
    if (!tape->index(reason)) return nullptr;
    tape->discoverVolumeId();
    return tape;
}

bool SimhTapeBackend::index(std::string& reason)
{
    items_.clear();
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        reason = std::strerror(errno);
        return false;
    }
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path_, ec);
    if (ec) {
        reason = ec.message();
        return false;
    }

    std::uint64_t pos = 0;
    while (pos < size) {
        if (size - pos < 4) {
            reason = fmt::format("truncated SIMH record marker at byte {}", pos);
            return false;
        }
        std::uint32_t marker = 0;
        if (!readWord(in, marker)) {
            reason = fmt::format("cannot read SIMH record marker at byte {}", pos);
            return false;
        }
        pos += 4;
        if (marker == kTapeMark) {
            items_.push_back(Item::tapeMark());
            continue;
        }
        if (marker == kEndOfMedium) {
            if (pos != size) {
                reason = fmt::format("data follows the SIMH end-of-medium marker at byte {}", pos - 4);
                return false;
            }
            break;
        }
        if (marker == kEraseGap) continue;

        const std::uint32_t flags = marker & ~kLengthMask;
        if (flags != 0 && flags != kErrorFlag) {
            reason = fmt::format("unsupported SIMH record flags 0x{:08X} at byte {}", flags, pos - 4);
            return false;
        }
        const std::uint32_t length = marker & kLengthMask;
        const std::uint64_t padded = static_cast<std::uint64_t>(length) + (length & 1U);
        if (length == 0 || padded + 4 > size - pos) {
            reason = fmt::format("truncated SIMH record of {} byte(s) at byte {}", length, pos - 4);
            return false;
        }
        const std::uint64_t dataOffset = pos;
        in.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
        pos += padded;
        std::uint32_t trailer = 0;
        if (!readWord(in, trailer)) {
            reason = fmt::format("missing SIMH record trailer at byte {}", pos);
            return false;
        }
        pos += 4;
        if (trailer != marker) {
            reason = fmt::format("SIMH record marker mismatch at byte {}: 0x{:08X} != 0x{:08X}", pos - 4,
                                 marker, trailer);
            return false;
        }
        items_.push_back(Item::source(dataOffset, length, flags == kErrorFlag));
    }
    dirty_ = false;
    cursor_ = 0;
    return true;
}

bool SimhTapeBackend::readItem(const Item& item, std::vector<uint8_t>& data, std::string& reason) const
{
    if (item.inMemory) {
        data = item.bytes;
        return true;
    }
    data.resize(item.length);
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        reason = std::strerror(errno);
        return false;
    }
    in.seekg(static_cast<std::streamoff>(item.offset));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::size_t>(in.gcount()) != data.size()) {
        reason = "the SIMH tape changed or was truncated while mounted";
        return false;
    }
    return true;
}

void SimhTapeBackend::discoverVolumeId()
{
    volumeId_.clear();
    if (items_.empty() || items_.front().mark || items_.front().length != 80) return;
    std::vector<uint8_t> record;
    std::string reason;
    if (!readItem(items_.front(), record, reason) || record.size() != 80 || record[0] != 0xE5 ||
        record[1] != 0xD6 || record[2] != 0xD3 || record[3] != 0xF1)
        return;
    volumeId_ = TapeLabel::decodeVol1(record, 0).value("volumeId", std::string());
}

void SimhTapeBackend::load()
{
    cursor_ = 0;
    loaded_ = true;
}

void SimhTapeBackend::unload()
{
    if (loaded_ && dirty_) flush();
    loaded_ = false;
}

void SimhTapeBackend::rewind() { cursor_ = 0; }

TapeResult SimhTapeBackend::readBlock(std::vector<uint8_t>& data)
{
    data.clear();
    if (!loaded_) return TapeResult::NotReady;
    if (cursor_ >= items_.size()) return TapeResult::EndOfData;
    const Item& item = items_[cursor_++];
    if (item.mark) return TapeResult::TapeMark;
    std::string reason;
    if (!readItem(item, data, reason)) throw std::runtime_error(reason);
    return item.recordError ? TapeResult::RecordError : TapeResult::Ok;
}

TapeResult SimhTapeBackend::writeBlock(const uint8_t* data, int offset, int length)
{
    if (!loaded_) return TapeResult::NotReady;
    if (readOnly_) return TapeResult::WriteProtected;
    if (data == nullptr || offset < 0 || length < 1 || static_cast<std::uint32_t>(length) > kLengthMask)
        return TapeResult::InvalidArgument;
    truncateForward();
    items_.push_back(Item::memory(std::vector<uint8_t>(data + offset, data + offset + length)));
    cursor_ = items_.size();
    dirty_ = true;
    return TapeResult::Ok;
}

TapeResult SimhTapeBackend::writeTapeMark()
{
    if (!loaded_) return TapeResult::NotReady;
    if (readOnly_) return TapeResult::WriteProtected;
    truncateForward();
    items_.push_back(Item::tapeMark());
    cursor_ = items_.size();
    dirty_ = true;
    return TapeResult::Ok;
}

void SimhTapeBackend::truncateForward()
{
    if (cursor_ < items_.size()) items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(cursor_), items_.end());
}

TapeResult SimhTapeBackend::spaceRecords(int count, int& spaced)
{
    spaced = 0;
    if (!loaded_) return TapeResult::NotReady;
    if (count >= 0) {
        while (spaced < count) {
            if (cursor_ >= items_.size()) return TapeResult::EndOfData;
            if (items_[cursor_].mark) return TapeResult::TapeMark;
            ++cursor_;
            ++spaced;
        }
        return TapeResult::Ok;
    }
    while (spaced < -count) {
        if (cursor_ == 0) return TapeResult::BeginningOfTape;
        if (items_[cursor_ - 1].mark) return TapeResult::TapeMark;
        --cursor_;
        ++spaced;
    }
    return TapeResult::Ok;
}

TapeResult SimhTapeBackend::spaceFiles(int count, int& spaced)
{
    spaced = 0;
    if (!loaded_) return TapeResult::NotReady;
    if (count >= 0) {
        while (spaced < count) {
            if (!forwardToAfterNextMark()) return TapeResult::EndOfData;
            ++spaced;
        }
        return TapeResult::Ok;
    }
    while (spaced < -count) {
        if (!backwardToStartOfFile()) return TapeResult::BeginningOfTape;
        ++spaced;
    }
    return TapeResult::Ok;
}

bool SimhTapeBackend::forwardToAfterNextMark()
{
    while (cursor_ < items_.size()) {
        const bool mark = items_[cursor_++].mark;
        if (mark) return true;
    }
    return false;
}

bool SimhTapeBackend::backwardToStartOfFile()
{
    if (cursor_ == 0) return false;
    if (items_[cursor_ - 1].mark) --cursor_;
    while (cursor_ > 0 && !items_[cursor_ - 1].mark) --cursor_;
    return true;
}

TapePosition SimhTapeBackend::readPosition() const
{
    TapePosition result;
    for (std::size_t i = 0; i < cursor_ && i < items_.size(); ++i) {
        if (items_[i].mark) {
            ++result.fileNumber;
            result.blockNumber = 0;
        } else {
            ++result.blockNumber;
        }
    }
    result.beginningOfTape = cursor_ == 0;
    result.endOfData = cursor_ >= items_.size();
    result.atTapeMark = cursor_ < items_.size() && items_[cursor_].mark;
    return result;
}

void SimhTapeBackend::flush()
{
    if (readOnly_) throw std::runtime_error("the tape is read-only");
    fs::path temp = fs::path(path_).string() + ".sim36-tmp";
    for (unsigned suffix = 0; fs::exists(temp); ++suffix)
        temp = fs::path(path_).string() + fmt::format(".sim36-tmp-{}", suffix);

    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error(std::strerror(errno));
        for (const Item& item : items_) {
            if (item.mark) {
                writeWord(out, kTapeMark);
                continue;
            }
            std::vector<uint8_t> data;
            std::string reason;
            if (!readItem(item, data, reason)) throw std::runtime_error(reason);
            const std::uint32_t marker = item.length | (item.recordError ? kErrorFlag : 0U);
            writeWord(out, marker);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (data.size() & 1U) out.put('\0');
            writeWord(out, marker);
        }
        out.flush();
        if (!out) throw std::runtime_error(std::strerror(errno));
        out.close();
        replaceFile(temp, path_);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }

    const std::size_t savedCursor = cursor_;
    std::string reason;
    if (!index(reason)) throw std::runtime_error("written SIMH tape did not validate: " + reason);
    cursor_ = std::min(savedCursor, items_.size());
    discoverVolumeId();
}

}  // namespace sim36::storage
