#include "Storage/Library.h"

#include <algorithm>

#include "Storage/ByteOrder.h"
#include "Storage/Ebcdic.h"

namespace sim36::storage {

namespace {

// Entry layout.  The type byte is EBCDIC.
constexpr int kOffType = 0x00;
constexpr int kOffName = 0x01;      // 8 bytes
constexpr int kOffSector = 0x09;    // 3 bytes, relative to the library extent
constexpr int kOffSectors = 0x0C;   // 1 byte
constexpr int kOffLink = 0x0D;      // 2 bytes
constexpr int kOffAttr = 0x13;
static_assert(kOffAttr < LibraryDirectory::kEntryBytes, "directory entry fields fit the entry");
static_assert(LibraryDirectory::kEntriesPerSector * LibraryDirectory::kEntryBytes <= DiskBackend::kSectorBytes,
              "five entries fit a sector");

// O load, P procedure, R subroutine, S source.
char kindOf(uint8_t b)
{
    switch (b) {
        case 0xD6: return 'O';
        case 0xD7: return 'P';
        case 0xD9: return 'R';
        case 0xE2: return 'S';
        default: return '\0';
    }
}

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::vector<LibraryMember> LibraryDirectory::read(const DiskBackend& v, const VtocEntry& lib, int maxSectors)
{
    std::vector<LibraryMember> members;
    uint8_t buf[DiskBackend::kSectorBytes];
    const long long start = lib.extentSector + 1;      // one header sector, then the directory
    const int limit = std::min(maxSectors, lib.allocatedSectors);
    for (int i = 0; i < limit; ++i) {
        const long long s = start + i;
        if (s >= v.sectorCount()) break;
        if (!v.readSector(s, buf)) break;
        int found = 0;
        for (int e = 0; e < kEntriesPerSector; ++e) {
            const uint8_t* o = buf + e * kEntryBytes;
            const char kind = kindOf(o[kOffType]);
            if (kind == '\0') continue;
            std::string name = trim(Ebcdic::toAscii(o + kOffName, 8));
            if (name.empty() || static_cast<unsigned char>(name[0]) < ' ') continue;
            const int rel = static_cast<int>(be24(o + kOffSector));
            if (rel <= 0 || rel >= lib.allocatedSectors) continue;
            LibraryMember m;
            m.kind = kind;
            m.name = name;
            m.relativeSector = rel;
            m.sectors = o[kOffSectors];
            m.linkAddress = be16(o + kOffLink);
            m.attributes = o[kOffAttr];
            members.push_back(m);
            ++found;
        }
        if (found == 0 && !members.empty()) break;   // past the directory
    }
    return members;
}

}  // namespace sim36::storage
