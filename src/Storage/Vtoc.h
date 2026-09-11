// The volume tables of contents: 128-byte format-1 labels.
//
// The volume has two VTOCs: a system-area VTOC holding #LIBRARY and the
// system files, and the 960-slot user VTOC two blocks after it.  Where they
// are is a property of the volume: the VOL1 label at sector 8190 carries a
// three-byte VTOC pointer at +0x0D, the "sector address of VTOC on disk", and
// the format-1 labels follow it after a run of FF-filled sectors.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Storage/DiskBackend.h"

namespace sim36::storage {

enum class VtocEntryType { Unknown, IndexedFile, SequentialFile, DirectFile, Library, Folder };

const char* vtocEntryTypeName(VtocEntryType t);

// A 128-byte format-1 label.
struct VtocEntry {
    std::string name;
    VtocEntryType type = VtocEntryType::Unknown;
    int extentSector = 0;     // +0x27, three bytes - the extent start
    int dataSector = 0;       // +0x21, three bytes - data within the extent, 0 if none
    int allocatedSectors = 0; // +0x2A
    int recordLength = 0;     // +0x3D
    int recordsUsed = 0;      // +0x37
    int capacity = 0;         // +0x4D

    int endSector() const { return extentSector + allocatedSectors; }
    std::string toString() const;
};

class Vtoc {
public:
    static constexpr int kVolumeLabelSector = 8190;
    static constexpr int kDefaultSystemVtocBlock = 841;   // the reference volume's layout
    static constexpr int kSystemVtocBlock = 841;
    static constexpr int kUserVtocBlock = 843;
    static constexpr int kUserVtocBlocks = 48;
    static constexpr int kEntryBytes = 128;

    // The system VTOC block this volume actually uses, from its own VOL1.
    static int systemBlockOf(const DiskBackend* v);

    // The first sector holding a format-1 label, found rather than computed:
    // scan forward from the VOL1 pointer for the first C6 F1 (EBCDIC "F1").
    // Falls back to the reference volume's layout when there is no usable
    // VOL1, no pointer, or no label within reach.
    static int systemSectorOf(const DiskBackend* v);

    static std::vector<VtocEntry> readSystem(const DiskBackend& v);
    static std::vector<VtocEntry> readUser(const DiskBackend& v);
    static std::vector<VtocEntry> read(const DiskBackend& v, int startBlock, int blocks);
    static std::vector<VtocEntry> readAt(const DiskBackend& v, long long first, int sectors);

private:
    static constexpr int kSearchSectors = 40;
};

}  // namespace sim36::storage
