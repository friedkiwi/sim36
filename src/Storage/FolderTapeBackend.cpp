#include "Storage/FolderTapeBackend.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include <fmt/format.h>

namespace sim36::storage {

namespace fs = std::filesystem;

namespace {

bool readWholeFile(const fs::path& p, std::string& out, std::string& reason)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        reason = std::strerror(errno);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeWholeFile(const fs::path& p, const void* bytes, std::size_t length, std::string& reason)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        reason = std::strerror(errno);
        return false;
    }
    out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(length));
    if (!out) {
        reason = std::strerror(errno);
        return false;
    }
    return true;
}

bool safeBlobPath(const std::string& blob)
{
    if (blob.empty() || blob.find('\\') != std::string::npos) return false;
    fs::path p(blob);
    if (p.is_absolute()) return false;
    for (const auto& part : p)
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}

bool validateTapeFolder(const fs::path& folder, const TapeManifest& manifest, std::string& reason)
{
    constexpr int kMaxBlockLength = 0x7FFF;
    std::set<std::string> blobs;
    for (std::size_t index = 0; index < manifest.files.size(); ++index) {
        const TapeFileEntry& f = manifest.files[index];
        const int expectedSequence = static_cast<int>(index) + 1;
        if (f.sequence != expectedSequence) {
            reason = fmt::format("tape file {} has sequence {}, expected {}", index + 1, f.sequence, expectedSequence);
            return false;
        }
        if (!safeBlobPath(f.blob)) {
            reason = fmt::format("tape file {} has unsafe blob path '{}'", f.sequence, f.blob);
            return false;
        }
        if (!blobs.insert(f.blob).second) {
            reason = fmt::format("tape file {} reuses blob '{}'", f.sequence, f.blob);
            return false;
        }
        std::vector<int> lengths = f.resolveBlockLengths();
        if (f.blockCount < 0 || static_cast<std::size_t>(f.blockCount) != lengths.size()) {
            reason = fmt::format("tape file {} has inconsistent blockCount", f.sequence);
            return false;
        }
        long long declaredBytes = 0;
        for (int length : lengths) {
            if (length < 1 || length > kMaxBlockLength) {
                reason = fmt::format("tape file {} has block length {} outside 1..{}", f.sequence, length, kMaxBlockLength);
                return false;
            }
            declaredBytes += length;
        }
        fs::path path = folder / fs::path(f.blob);
        std::error_code ec;
        if (fs::is_symlink(path, ec) || !fs::is_regular_file(path, ec)) {
            reason = fmt::format("tape file {} blob '{}' is missing or is not a regular file", f.sequence, f.blob);
            return false;
        }
        std::uintmax_t actualBytes = fs::file_size(path, ec);
        if (ec || actualBytes != static_cast<std::uintmax_t>(declaredBytes)) {
            reason = fmt::format("tape file {} blob '{}' has {} byte(s), manifest declares {}", f.sequence, f.blob,
                                 ec ? 0 : actualBytes, declaredBytes);
            return false;
        }
        if (!f.labels.empty()) {
            std::vector<std::vector<uint8_t>> records;
            std::ifstream in(path, std::ios::binary);
            for (int length : lengths) {
                std::vector<uint8_t> record(static_cast<std::size_t>(length));
                in.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()));
                records.push_back(std::move(record));
            }
            nlohmann::ordered_json decoded = nlohmann::ordered_json::object();
            if (!TapeLabel::decodeLabelGroup(records, decoded) || decoded != f.labels) {
                reason = fmt::format("tape file {} decoded labels do not match its blob bytes", f.sequence);
                return false;
            }
        }
    }

    if (manifest.volume.labeled) {
        if (manifest.files.empty()) {
            reason = "labeled tape has no VOL1 file";
            return false;
        }
        const TapeFileEntry& first = manifest.files.front();
        std::vector<int> lengths = first.resolveBlockLengths();
        if (lengths.empty() || lengths.front() != FolderTapeBackend::kLabelLength) {
            reason = "labeled tape does not begin with an 80-byte VOL1 block";
            return false;
        }
        std::ifstream in(folder / first.blob, std::ios::binary);
        std::vector<uint8_t> record(FolderTapeBackend::kLabelLength);
        in.read(reinterpret_cast<char*>(record.data()), FolderTapeBackend::kLabelLength);
        if (record[0] != 0xE5 || record[1] != 0xD6 || record[2] != 0xD3 || record[3] != 0xF1) {
            reason = "labeled tape does not begin with VOL1 bytes";
            return false;
        }
        nlohmann::ordered_json vol1 = TapeLabel::decodeVol1(record, 0);
        std::string expectedAccess = manifest.volume.accessSecurity;
        if (expectedAccess == " ") expectedAccess.clear();
        if (vol1.value("volumeId", std::string()) != manifest.volume.volumeId ||
            vol1.value("ownerId", std::string()) != manifest.volume.ownerId ||
            vol1.value("accessSecurity", std::string()) != expectedAccess) {
            reason = "manifest volume metadata does not match the VOL1 bytes";
            return false;
        }
    }
    return true;
}

}  // namespace

// ---- TapeItem ---------------------------------------------------------------------

FolderTapeBackend::TapeItem FolderTapeBackend::TapeItem::mark()
{
    TapeItem t;
    t.isMark = true;
    return t;
}

FolderTapeBackend::TapeItem FolderTapeBackend::TapeItem::fromMemory(std::vector<uint8_t> bytes)
{
    TapeItem t;
    t.inMemory = true;
    t.length = static_cast<int>(bytes.size());
    t.mem = std::move(bytes);
    return t;
}

FolderTapeBackend::TapeItem FolderTapeBackend::TapeItem::fromBlob(const std::string& blob, long long offset, int length)
{
    TapeItem t;
    t.blob = blob;
    t.offset = offset;
    t.length = length;
    return t;
}

std::vector<uint8_t> FolderTapeBackend::TapeItem::read(const std::string& folder) const
{
    if (isMark) return {};
    if (inMemory) return mem;

    std::vector<uint8_t> b(static_cast<std::size_t>(length));
    std::ifstream in(fs::path(folder) / blob, std::ios::binary);
    if (!in) throw std::runtime_error("blob " + blob + " is shorter than the manifest declares");
    in.seekg(offset, std::ios::beg);
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (static_cast<std::size_t>(in.gcount()) != b.size())
        throw std::runtime_error("blob " + blob + " is shorter than the manifest declares");
    return b;
}

// ---- opening and creating ---------------------------------------------------------

std::unique_ptr<FolderTapeBackend> FolderTapeBackend::open(const std::string& folder, bool readOnly, std::string& reason)
{
    reason.clear();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        reason = folder + " is not a directory";
        return nullptr;
    }
    fs::path manifestPath = fs::path(folder) / kManifestName;
    if (!fs::exists(manifestPath, ec)) {
        reason = std::string("no ") + kManifestName + " in " + folder + " - not a tape folder";
        return nullptr;
    }
    std::string text;
    if (!readWholeFile(manifestPath, text, reason)) return nullptr;
    std::unique_ptr<TapeManifest> m = TapeManifest::read(text, reason);
    if (!m) return nullptr;
    if (!validateTapeFolder(folder, *m, reason)) return nullptr;
    return std::unique_ptr<FolderTapeBackend>(new FolderTapeBackend(folder, readOnly, std::move(*m)));
}

bool FolderTapeBackend::init(const std::string& folder, const std::string& volumeId, const std::string& ownerId,
                             std::string& reason)
{
    reason.clear();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        reason = ec.message();
        return false;
    }
    // A fresh standard-labeled volume: VOL1, then two tape marks.  The empty
    // second tape file represents the second consecutive mark; one mark alone
    // is only a file boundary, not the standard empty-volume terminator.
    std::vector<uint8_t> vol1 = TapeLabel::renderVol1(volumeId, ' ', ownerId);

    TapeManifest manifest;
    manifest.volume.volumeId = TapeLabel::normaliseVolumeId(volumeId);
    manifest.volume.ownerId = ownerId.substr(0, 14);
    manifest.volume.labeled = true;

    TapeFileEntry f;
    f.sequence = 1;
    f.kind = "label";
    f.blob = "0001.dat";
    f.blockLength = kLabelLength;
    f.blockCount = 1;
    f.recordFormat = "F";
    f.recordLength = kLabelLength;
    f.labels["vol1"] = TapeLabel::decodeVol1(vol1, 0);
    manifest.files.push_back(f);

    TapeFileEntry end;
    end.sequence = 2;
    end.kind = "data";
    end.blob = "0002.dat";
    end.blockCount = 0;
    end.hasBlockLengths = true;
    end.recordFormat = "U";
    manifest.files.push_back(end);

    if (!writeWholeFile(fs::path(folder) / "0001.dat", vol1.data(), vol1.size(), reason)) return false;
    const char empty = 0;
    if (!writeWholeFile(fs::path(folder) / "0002.dat", &empty, 0, reason)) return false;
    std::string text = manifest.write();
    return writeWholeFile(fs::path(folder) / kManifestName, text.data(), text.size(), reason);
}

// ---- load / unload / rewind --------------------------------------------------------

void FolderTapeBackend::load()
{
    items_.clear();
    for (const TapeFileEntry& f : manifest_.files) {
        long long offset = 0;
        for (int len : f.resolveBlockLengths()) {
            items_.push_back(TapeItem::fromBlob(f.blob, offset, len));
            offset += len;
        }
        // Exactly one tape mark closes every tape file.
        items_.push_back(TapeItem::mark());
    }
    cursor_ = 0;
    loaded_ = true;
    dirty_ = false;
}

void FolderTapeBackend::unload()
{
    if (loaded_ && dirty_) flush();
    loaded_ = false;
}

void FolderTapeBackend::rewind() { cursor_ = 0; }

// ---- read / write -------------------------------------------------------------------

TapeResult FolderTapeBackend::readBlock(std::vector<uint8_t>& data)
{
    data.clear();
    if (!loaded_) return TapeResult::NotReady;
    if (cursor_ >= items_.size()) return TapeResult::EndOfData;

    const TapeItem& it = items_[cursor_];
    if (it.isMark) {
        cursor_++;   // cross the mark
        return TapeResult::TapeMark;
    }
    data = it.read(folder_);
    cursor_++;
    return TapeResult::Ok;
}

TapeResult FolderTapeBackend::writeBlock(const uint8_t* src, int offset, int length)
{
    if (!loaded_) return TapeResult::NotReady;
    if (readOnly_) return TapeResult::WriteProtected;

    truncateForward();
    items_.push_back(TapeItem::fromMemory(std::vector<uint8_t>(src + offset, src + offset + length)));
    cursor_ = items_.size();
    dirty_ = true;
    return TapeResult::Ok;
}

TapeResult FolderTapeBackend::writeTapeMark()
{
    if (!loaded_) return TapeResult::NotReady;
    if (readOnly_) return TapeResult::WriteProtected;

    truncateForward();
    items_.push_back(TapeItem::mark());
    cursor_ = items_.size();
    dirty_ = true;
    return TapeResult::Ok;
}

// A write that is not at end of data logically erases everything ahead of
// it: the append-forward rule of a tape.
void FolderTapeBackend::truncateForward()
{
    if (cursor_ < items_.size()) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(cursor_), items_.end());
        dirty_ = true;
    }
}

// ---- space (record and file) -----------------------------------------------------------

TapeResult FolderTapeBackend::spaceRecords(int count, int& spaced)
{
    spaced = 0;
    if (!loaded_) return TapeResult::NotReady;

    if (count >= 0) {
        for (int i = 0; i < count; i++) {
            if (cursor_ >= items_.size()) return TapeResult::EndOfData;
            // Record spacing does not cross tape marks; spaceFiles() does.
            // Stopping ON the mark keeps the two operations unambiguous.
            if (items_[cursor_].isMark) return TapeResult::TapeMark;
            cursor_++;
            spaced++;
        }
        return TapeResult::Ok;
    }

    for (int i = 0; i < -count; i++) {
        if (cursor_ == 0) return TapeResult::BeginningOfTape;
        if (items_[cursor_ - 1].isMark) return TapeResult::TapeMark;
        cursor_--;
        spaced++;
    }
    return TapeResult::Ok;
}

TapeResult FolderTapeBackend::spaceFiles(int count, int& spaced)
{
    spaced = 0;
    if (!loaded_) return TapeResult::NotReady;

    if (count >= 0) {
        for (int i = 0; i < count; i++) {
            if (!forwardToAfterNextMark()) return TapeResult::EndOfData;
            spaced++;
        }
        return TapeResult::Ok;
    }

    for (int i = 0; i < -count; i++) {
        if (!backwardToStartOfFile()) return TapeResult::BeginningOfTape;
        spaced++;
    }
    return TapeResult::Ok;
}

// Advance the cursor to just past the next tape mark.  False if there is no
// further mark (already at or past end of data).
bool FolderTapeBackend::forwardToAfterNextMark()
{
    while (cursor_ < items_.size()) {
        bool wasMark = items_[cursor_].isMark;
        cursor_++;
        if (wasMark) return true;
    }
    return false;
}

// Move the cursor to the start of the current tape file, or, if already
// there, to the start of the previous one.  False at load point.
bool FolderTapeBackend::backwardToStartOfFile()
{
    if (cursor_ == 0) return false;
    // Mid-file, stepping to the start of THIS file crosses the preceding
    // mark once.  Already at a file start (the item behind is a mark), step
    // over that mark into the previous file.
    if (items_[cursor_ - 1].isMark) cursor_--;
    while (cursor_ > 0 && !items_[cursor_ - 1].isMark) cursor_--;
    return true;
}

TapePosition FolderTapeBackend::readPosition() const
{
    TapePosition p;
    int marks = 0, blocksSinceMark = 0;
    for (std::size_t i = 0; i < cursor_ && i < items_.size(); i++) {
        if (items_[i].isMark) {
            marks++;
            blocksSinceMark = 0;
        } else {
            blocksSinceMark++;
        }
    }
    p.fileNumber = marks;
    p.blockNumber = blocksSinceMark;
    p.beginningOfTape = cursor_ == 0;
    p.endOfData = cursor_ >= items_.size();
    p.atTapeMark = cursor_ < items_.size() && items_[cursor_].isMark;
    return p;
}

// ---- persistence ------------------------------------------------------------------------

void FolderTapeBackend::flush()
{
    if (readOnly_) throw std::runtime_error("the tape is read-only");

    // Group the flat item list back into tape files (runs of blocks between
    // marks).  A run with no following mark still becomes a file.
    std::vector<std::vector<const TapeItem*>> files;
    std::vector<const TapeItem*> current;
    bool open = false;
    for (const TapeItem& it : items_) {
        if (it.isMark) {
            files.push_back(current);
            current.clear();
            open = false;
        } else {
            current.push_back(&it);
            open = true;
        }
    }
    if (open) files.push_back(current);

    TapeManifest manifest;
    manifest.volume = manifest_.volume;

    std::vector<fs::path> temps;
    int seq = 0;
    for (const auto& blocks : files) {
        seq++;
        std::string blob = fmt::format("{:04}.dat", seq);
        // The temp suffix must NOT be ".dat" or the "*.dat" cleanup below
        // would delete the very files being written.
        fs::path temp = fs::path(folder_) / (blob + ".tmp");
        temps.push_back(temp);

        std::vector<int> lengths;
        // Capture the block bytes only while they still look like a label
        // group (a few 80-byte records): enough to re-decode the labels
        // without ever holding a data file's blocks in memory.
        std::vector<std::vector<uint8_t>> captured;
        bool capturing = true;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error(std::strerror(errno));
            for (const TapeItem* it : blocks) {
                std::vector<uint8_t> bytes = it->read(folder_);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                lengths.push_back(static_cast<int>(bytes.size()));
                if (capturing && bytes.size() == static_cast<std::size_t>(kLabelLength) && captured.size() < 8)
                    captured.push_back(bytes);
                else
                    capturing = false;
            }
            if (!out) throw std::runtime_error(std::strerror(errno));
        }
        manifest.files.push_back(buildFileEntry(seq, blob, lengths, capturing ? &captured : nullptr));
    }

    // Swap the new blobs in and drop every stale one.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder_, ec))
        if (entry.path().extension() == ".dat") fs::remove(entry.path(), ec);
    for (const fs::path& temp : temps) {
        std::string s = temp.string();
        fs::path final = s.substr(0, s.size() - 4);
        fs::rename(temp, final, ec);
        if (ec) throw std::runtime_error(ec.message());
    }

    std::string text = manifest.write();
    std::string reason;
    if (!writeWholeFile(fs::path(folder_) / kManifestName, text.data(), text.size(), reason))
        throw std::runtime_error(reason);

    // Re-point every item at the freshly written blobs and reload the index,
    // so the in-memory model matches disk again.
    manifest_.files = manifest.files;
    std::size_t savedCursor = cursor_;
    load();
    cursor_ = std::min(savedCursor, items_.size());
}

// Describe one written tape file.  When every block is an 80-byte standard
// label and the first is a known label id (`captured` non-null), the file is
// tagged as a label group and its records are decoded into the manifest for
// legibility, so a rewritten tape stays as self-describing as an authored
// one.
TapeFileEntry FolderTapeBackend::buildFileEntry(int seq, const std::string& blob, const std::vector<int>& lengths,
                                                const std::vector<std::vector<uint8_t>>* captured)
{
    TapeFileEntry f;
    f.sequence = seq;
    f.blob = blob;
    f.blockCount = static_cast<int>(lengths.size());

    bool uniform = true;
    for (std::size_t i = 1; i < lengths.size(); i++)
        if (lengths[i] != lengths[0]) {
            uniform = false;
            break;
        }
    if (uniform && !lengths.empty()) {
        f.blockLength = lengths[0];
    } else {
        f.hasBlockLengths = true;
        f.blockLengths = lengths;
    }

    if (captured != nullptr && !captured->empty() && TapeLabel::decodeLabelGroup(*captured, f.labels)) {
        f.kind = "label";
        f.recordFormat = "F";
        f.recordLength = kLabelLength;
    } else {
        f.kind = "data";
        f.recordFormat = "U";
        f.recordLength = f.blockLength;
    }
    return f;
}

}  // namespace sim36::storage
