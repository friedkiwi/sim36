#include "Storage/DisketteBackend.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fmt/format.h>

#include "Storage/Ebcdic.h"

namespace sim36::storage {

namespace {

const uint8_t kVol1[4] = {0xE5, 0xD6, 0xD3, 0xF1};   // EBCDIC "VOL1"

bool match(const std::vector<uint8_t>& b, std::size_t at, const uint8_t* want, std::size_t n)
{
    for (std::size_t i = 0; i < n; i++)
        if (b[at + i] != want[i]) return false;
    return true;
}

std::string trimEnd(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string ebcdic(const std::vector<uint8_t>& b, std::size_t at, std::size_t len)
{
    if (at + len > b.size()) return "";
    return trimEnd(Ebcdic::toAscii(b.data() + at, len));
}

// Two-digit or one-digit decimal fields of an extent; false on anything
// that is not a digit.
bool parseDigits(const std::string& s, std::size_t at, std::size_t len, int& v)
{
    v = 0;
    if (at + len > s.size()) return false;
    for (std::size_t i = 0; i < len; i++) {
        char c = s[at + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    return true;
}

// HDR1 field offsets within the 128-byte label record.
constexpr int kHdr1First = 8;   // first label-track record that can be a HDR1
constexpr std::size_t kHdr1NameAt = 5, kHdr1NameLen = 17;
constexpr std::size_t kHdr1BoeAt = 28, kHdr1PrlAt = 33, kHdr1EoeAt = 34, kHdr1ExtentLen = 5;

bool parseExtent(const std::string& s, std::size_t at, int& c, int& h, int& r)
{
    c = h = r = 0;
    if (at + kHdr1ExtentLen > s.size()) return false;
    return parseDigits(s, at, 2, c) && parseDigits(s, at + 2, 1, h) && parseDigits(s, at + 3, 2, r);
}

std::string trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

}  // namespace

long long DisketteGeometry::trackOffset(int cylinder, int head) const
{
    long long at = 0;
    if (cylinder > 0) {
        at = cylinderZeroBytes();
        at += static_cast<long long>(cylinder - 1) * heads_ * dataTrackBytes();
    }
    for (int h = 0; h < head; h++) at += trackBytes(cylinder, h);
    return at;
}

bool DisketteGeometry::sequentialToRecord(int ss, int& cylinder, int& head, int& record) const
{
    cylinder = head = record = 0;
    if (ss < 1) return false;
    int perTrack = dataSectorsPerTrack_;
    int perCylinder = perTrack * heads_;
    int zero = ss - 1;
    cylinder = 1 + zero / perCylinder;
    int within = zero % perCylinder;
    head = within / perTrack;
    record = within % perTrack + 1;
    return cylinder <= lastSequentialCylinder();
}

bool DisketteGeometry::tryProbe(const std::vector<uint8_t>& labelTrack, long long length, DisketteGeometry& g,
                                std::string& reason)
{
    int labelSectorBytes = 0;
    std::size_t at = 0;
    for (int candidate : {128, 256}) {
        std::size_t probe = static_cast<std::size_t>((kVolumeLabelSector - 1) * candidate);
        if (probe + 128 > labelTrack.size()) continue;
        if (!match(labelTrack, probe, kVol1, 4)) continue;
        labelSectorBytes = candidate;
        at = probe;
        break;
    }
    if (labelSectorBytes == 0) {
        reason = fmt::format("no EBCDIC VOL1 (E5 D6 D3 F1) at label sector {} - neither at 6 x 128 ({:06X}) nor at 6 x 256 "
                             "({:06X}). This is not System/36 diskette media; the PC-DOS diskettes in the media corpus "
                             "identify themselves exactly this way. docs/file-formats/s36-diskette.md",
                             kVolumeLabelSector, 6 * 128, 6 * 256);
        return false;
    }

    uint8_t prl = labelTrack[at + kVolumeLabelPrlOffset];
    int dataSectorBytes;
    switch (prl) {
        case 0xF3: dataSectorBytes = 1024; break;
        case 0xF2: dataSectorBytes = 512; break;
        case 0xF1: dataSectorBytes = 256; break;
        case 0x40: dataSectorBytes = 128; break;   // EBCDIC blank: the 3740 default
        default:
            reason = fmt::format("VOL1+{} is {:02X}, which is not a physical record length code. The code is EBCDIC: F3 = "
                                 "1024, F2 = 512, F1 = 256, and blank (40) = 128",
                                 kVolumeLabelPrlOffset, prl);
            return false;
    }

    int spt;
    switch (dataSectorBytes) {
        case 1024: spt = 8; break;
        case 512: spt = 15; break;
        default: spt = 26; break;   // 256 and 128 alike
    }

    // Sidedness is NOT arithmetic.  A 256 256-byte Diskette 1 divides evenly
    // under both the one-sided and the two-sided parse (76 and 37 cylinders
    // respectively), so the length cannot break the tie.  What breaks it is
    // the format: a volume declaring 128-byte data sectors IS the 3740
    // "Diskette 1", which is single-sided FM by definition.
    int heads = dataSectorBytes == 128 ? 1 : 2;

    DisketteGeometry probeGeometry;
    probeGeometry.labelSectorBytes_ = labelSectorBytes;
    probeGeometry.dataSectorBytes_ = dataSectorBytes;
    probeGeometry.dataSectorsPerTrack_ = spt;
    probeGeometry.heads_ = heads;
    probeGeometry.cylinders_ = 1;
    probeGeometry.volumeId_ = ebcdic(labelTrack, at + 4, 6);
    probeGeometry.ownerId_ = ebcdic(labelTrack, at + 37, 14);

    long long rest = length - probeGeometry.cylinderZeroBytes();
    long long perCylinder = static_cast<long long>(heads) * probeGeometry.dataTrackBytes();
    if (rest < 0 || rest % perCylinder != 0) {
        reason = fmt::format("the image is {} bytes: {} for cylinder 0 ({} x {} B label track{}) leaves {}, which is not a "
                             "whole number of {}-byte cylinders ({} head(s) x {} x {} B). Either the image is truncated or "
                             "it is not a physical sector image in C/H/S order - see `tools/s36dskt.py --export`",
                             length, probeGeometry.cylinderZeroBytes(), kLabelSectorsPerTrack, labelSectorBytes,
                             heads > 1 ? " plus a 26 x 256 B head 1" : ", one-sided", rest, perCylinder, heads, spt,
                             dataSectorBytes);
        return false;
    }

    probeGeometry.cylinders_ = 1 + static_cast<int>(rest / perCylinder);
    g = probeGeometry;
    reason.clear();
    return true;
}

std::string DisketteGeometry::toString() const
{
    return fmt::format("{} cylinders x {} head(s); label track {} x {} B, data {} x {} B; {}-inch; {} bytes", cylinders_,
                       heads_, kLabelSectorsPerTrack, labelSectorBytes_, dataSectorsPerTrack_, dataSectorBytes_,
                       isEightInch() ? "8" : "5 1/4", totalBytes());
}

DisketteBackend::~DisketteBackend()
{
    if (file_ != nullptr) std::fclose(file_);
}

std::unique_ptr<DisketteBackend> DisketteBackend::open(const std::string& path, bool readOnly, std::string& reason)
{
    std::FILE* f = std::fopen(path.c_str(), readOnly ? "rb" : "r+b");
    if (f == nullptr) {
        reason = std::strerror(errno);
        return nullptr;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        reason = std::strerror(errno);
        std::fclose(f);
        return nullptr;
    }
    long long length = std::ftell(f);
    std::rewind(f);

    // Enough to hold label sector 7 under either candidate sector size, and
    // nothing more: the probe must not depend on anything past the label
    // track.
    long long want = std::min<long long>(length, DisketteGeometry::kLabelSectorsPerTrack * 256);
    std::vector<uint8_t> head(static_cast<std::size_t>(want));
    std::size_t got = 0;
    while (got < head.size()) {
        std::size_t n = std::fread(head.data() + got, 1, head.size() - got, f);
        if (n == 0) break;
        got += n;
    }
    head.resize(got);

    DisketteGeometry g;
    if (!DisketteGeometry::tryProbe(head, length, g, reason)) {
        std::fclose(f);
        return nullptr;
    }
    reason.clear();
    return std::unique_ptr<DisketteBackend>(new DisketteBackend(path, readOnly, f, g));
}

bool DisketteBackend::findDataSet(const std::string& name, DataSet& out)
{
    std::string want = trim(name);
    for (int r = kHdr1First; r <= DisketteGeometry::kLabelSectorsPerTrack; r++) {
        std::vector<uint8_t> rec;
        if (!readRecord(0, 0, r, rec)) break;
        std::string label = Ebcdic::toAscii(rec.data(), std::min<std::size_t>(128, rec.size()));
        if (label.compare(0, 4, "HDR1") != 0) continue;
        if (label.size() < kHdr1NameAt + kHdr1NameLen) continue;
        if (trim(label.substr(kHdr1NameAt, kHdr1NameLen)) != want) continue;

        DataSet d;
        d.name = want;
        if (!parseExtent(label, kHdr1BoeAt, d.cylinder, d.head, d.record)) return false;
        if (!parseExtent(label, kHdr1EoeAt, d.endCylinder, d.endHead, d.endRecord)) return false;
        if (label.size() <= kHdr1PrlAt) return false;
        switch (label[kHdr1PrlAt]) {
            case '0': d.recordBytes = 128; break;
            case '1': d.recordBytes = 256; break;
            case '2': d.recordBytes = 512; break;
            case '3': d.recordBytes = 1024; break;
            default: return false;
        }
        out = d;
        return true;
    }
    return false;
}

bool DisketteBackend::readRecord(int cylinder, int head, int record, std::vector<uint8_t>& out)
{
    int size = geometry_.trackSectorBytes(cylinder, head);
    out.assign(static_cast<std::size_t>(size), 0);
    if (std::fseek(file_, static_cast<long>(geometry_.recordOffset(cylinder, head, record)), SEEK_SET) != 0) return false;
    std::size_t got = 0;
    while (got < out.size()) {
        std::size_t n = std::fread(out.data() + got, 1, out.size() - got, file_);
        if (n == 0) return false;
        got += n;
    }
    return true;
}

bool DisketteBackend::writeRecord(int cylinder, int head, int record, const uint8_t* src, int count, std::string& why)
{
    if (readOnly_) {
        why = "the diskette is read-only";
        return false;
    }
    int size = geometry_.trackSectorBytes(cylinder, head);
    if (count > size) {
        why = "a record cannot be longer than the sector that holds it\nParameter name: count";
        return false;
    }
    std::vector<uint8_t> sector;
    const uint8_t* bytes = src;
    if (count != size) {
        if (!readRecord(cylinder, head, record, sector)) {
            why = "the image is shorter than its geometry declares";
            return false;
        }
        std::memcpy(sector.data(), src, static_cast<std::size_t>(count));
        bytes = sector.data();
    }
    if (std::fseek(file_, static_cast<long>(geometry_.recordOffset(cylinder, head, record)), SEEK_SET) != 0 ||
        std::fwrite(bytes, 1, static_cast<std::size_t>(size), file_) != static_cast<std::size_t>(size)) {
        why = std::strerror(errno);
        return false;
    }
    std::fflush(file_);
    why.clear();
    return true;
}

bool DisketteBackend::next(int& cylinder, int& head, int& record) const
{
    record++;
    if (record <= geometry_.trackSectors(cylinder, head)) return true;
    record = 1;
    head++;
    if (head < geometry_.heads()) return true;
    head = 0;
    cylinder++;
    return cylinder < geometry_.cylinders();
}

}  // namespace sim36::storage
