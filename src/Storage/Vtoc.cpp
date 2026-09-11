#include "Storage/Vtoc.h"

#include <fmt/format.h>

#include "Storage/ByteOrder.h"
#include "Storage/Ebcdic.h"

namespace sim36::storage {

namespace {

constexpr int kOffVtocAddress = 0x0D;   // 3 bytes in VOL1
constexpr int kOffType = 0x02, kOffName = 0x04, kOffData = 0x21, kOffExtent = 0x27;
constexpr int kOffAlloc = 0x2A, kOffUsed = 0x37, kOffRecLen = 0x3D, kOffCapacity = 0x4D;

static_assert(kOffCapacity + 3 <= Vtoc::kEntryBytes, "format-1 label fields fit the entry");

// System-area labels store 0x01 where CATALOG displays '#', so the raw field
// is not directly printable.  Drop control characters and keep the rest.
std::string cleanName(const std::string& raw)
{
    std::string s;
    for (char c : raw)
        if (c >= ' ' && static_cast<unsigned char>(c) < 127) s.push_back(c);
    std::size_t b = 0, e = s.size();
    while (b < e && s[b] == ' ') ++b;
    while (e > b && s[e - 1] == ' ') --e;
    return s.substr(b, e - b);
}

VtocEntryType typeOf(uint8_t b)
{
    switch (b) {
        case 0x80: return VtocEntryType::IndexedFile;
        case 0x40: return VtocEntryType::SequentialFile;
        case 0x20: return VtocEntryType::DirectFile;
        case 0x08: return VtocEntryType::Library;
        case 0x02: return VtocEntryType::Folder;
        default: return VtocEntryType::Unknown;
    }
}

}  // namespace

const char* vtocEntryTypeName(VtocEntryType t)
{
    switch (t) {
        case VtocEntryType::IndexedFile: return "IndexedFile";
        case VtocEntryType::SequentialFile: return "SequentialFile";
        case VtocEntryType::DirectFile: return "DirectFile";
        case VtocEntryType::Library: return "Library";
        case VtocEntryType::Folder: return "Folder";
        default: return "Unknown";
    }
}

std::string VtocEntry::toString() const
{
    // "sectors", not "sec": the short form reads as seconds, and every number
    // on this line is a disk address or a count of disk sectors.
    return fmt::format("{:<9} {:<15} {:>8}..{:<8} {:>7} sectors",
                       name, vtocEntryTypeName(type), extentSector, endSector(), allocatedSectors);
}

int Vtoc::systemBlockOf(const DiskBackend* v)
{
    return systemSectorOf(v) / DiskBackend::kSectorsPerBlock;
}

int Vtoc::systemSectorOf(const DiskBackend* v)
{
    const int fallback = kDefaultSystemVtocBlock * DiskBackend::kSectorsPerBlock;
    if (v == nullptr || v->sectorCount() <= kVolumeLabelSector) return fallback;

    uint8_t buf[DiskBackend::kSectorBytes];
    if (!v->readSector(kVolumeLabelSector, buf)) return fallback;
    // EBCDIC "VOL1"; anything else and this is not a volume we can read.
    if (buf[0] != 0xE5 || buf[1] != 0xD6 || buf[2] != 0xD3 || buf[3] != 0xF1) return fallback;

    const int at = static_cast<int>(be24(buf + kOffVtocAddress));
    if (at <= 0 || at >= v->sectorCount()) return fallback;

    for (long long s = at; s < at + kSearchSectors && s < v->sectorCount(); ++s) {
        if (!v->readSector(s, buf)) return fallback;
        if (buf[0] == 0xC6 && buf[1] == 0xF1) return static_cast<int>(s);
    }
    return fallback;
}

std::vector<VtocEntry> Vtoc::readSystem(const DiskBackend& v)
{
    return readAt(v, systemSectorOf(&v), DiskBackend::kSectorsPerBlock);
}

std::vector<VtocEntry> Vtoc::readUser(const DiskBackend& v)
{
    long long at = systemSectorOf(&v) +
                   static_cast<long long>(kUserVtocBlock - kSystemVtocBlock) * DiskBackend::kSectorsPerBlock;
    return readAt(v, at, kUserVtocBlocks * DiskBackend::kSectorsPerBlock);
}

std::vector<VtocEntry> Vtoc::read(const DiskBackend& v, int startBlock, int blocks)
{
    return readAt(v, static_cast<long long>(startBlock) * DiskBackend::kSectorsPerBlock,
                  blocks * DiskBackend::kSectorsPerBlock);
}

std::vector<VtocEntry> Vtoc::readAt(const DiskBackend& v, long long first, int sectors)
{
    std::vector<VtocEntry> list;
    std::vector<uint8_t> all;
    uint8_t buf[DiskBackend::kSectorBytes];
    const long long last = first + sectors;
    for (long long s = first; s < last && s < v.sectorCount(); ++s) {
        if (!v.readSector(s, buf)) break;
        all.insert(all.end(), buf, buf + DiskBackend::kSectorBytes);
    }
    for (std::size_t o = 0; o + kEntryBytes <= all.size(); o += kEntryBytes) {
        const uint8_t* e = all.data() + o;
        if (e[0] != 0xC6 || e[1] != 0xF1) continue;   // EBCDIC "F1"
        VtocEntry entry;
        entry.name = cleanName(Ebcdic::toAscii(e + kOffName, 8));
        entry.type = typeOf(e[kOffType]);
        entry.extentSector = static_cast<int>(be24(e + kOffExtent));
        entry.dataSector = static_cast<int>(be24(e + kOffData));
        entry.allocatedSectors = static_cast<int>(be24(e + kOffAlloc));
        entry.recordsUsed = be16(e + kOffUsed);
        entry.recordLength = be16(e + kOffRecLen);
        entry.capacity = static_cast<int>(be24(e + kOffCapacity));
        list.push_back(entry);
    }
    return list;
}

}  // namespace sim36::storage
