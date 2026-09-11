// The library directory: 51-byte entries, five to a sector, starting one
// sector into the extent.  Member sector addresses are relative to the
// library extent.
#pragma once

#include <string>
#include <vector>

#include "Storage/DiskBackend.h"
#include "Storage/Vtoc.h"

namespace sim36::storage {

struct LibraryMember {
    char kind = '\0';        // O load module, P procedure, S source, R subroutine
    std::string name;
    int relativeSector = 0;  // relative to the library extent, not absolute
    int sectors = 0;
    int linkAddress = 0;
    uint8_t attributes = 0;

    int absoluteSector(const VtocEntry& lib) const { return lib.extentSector + relativeSector; }
};

class LibraryDirectory {
public:
    static constexpr int kEntryBytes = 51;       // 5 * 51 = 255, one spare byte per sector
    static constexpr int kEntriesPerSector = 5;

    static std::vector<LibraryMember> read(const DiskBackend& v, const VtocEntry& lib, int maxSectors = 4096);
};

}  // namespace sim36::storage
