// The task work area and the fields the 64-byte program block and 48-byte
// storage block share.
//
// A task work area is a run of disk sectors named RELATIVELY: a one-byte
// base identifier and a two-byte sector displacement packed into 24 bits.
// SVC 33 hands those addresses out, SVC 34 gives them back, SVC 35 wraps one
// in a storage block so it can be mapped, and SVC 51 reads and writes
// through one.  The base identifier is resolved through a chain of 16-byte
// "QH" blocks in guest storage anchored at queue header 46, which the
// control processor builds at IPL.
#pragma once

#include <cstdint>
#include <vector>

namespace sim36::processors::controlstorage {

// A task work area queue header: the "QH" block, 16 bytes, in guest storage.
//
//   +0     eyecatcher "QH" (D8C8)
//   +2..4  the next header, a 24-bit guest address
//   +5     the base identifier: the high byte of a relative disk address
//   +6     flags; bit 0x80 makes the element search check the extent first
//   +7..9  head of the free element chain
//   +11..13 the extent's base disk sector
//   +14..15 the extent's length in sectors
//
// A relative address resolves as sector = header(key)[11..13] + offset.
struct TaskWorkAreaQueue {
    static constexpr int kAnchorHeader = 46;
    static constexpr int kBytes = 16;
    static constexpr uint16_t kEyecatcher = 0xD8C8;   // "QH", EBCDIC

    static constexpr int kOffEyecatcher = 0;
    static constexpr int kOffNext = 2;
    static constexpr int kOffBaseIdentifier = 5;
    static constexpr int kOffFlags = 6;
    static constexpr int kOffFreeChain = 7;
    static constexpr int kOffBaseSector = 11;
    static constexpr int kOffSectors = 14;

    static constexpr uint8_t kFlagCheckExtent = 0x80;
    // The allocator stops walking the header chain at the first base
    // identifier of 254 or more, so 0xFE and 0xFF are reserved: reachable
    // only by naming them in a relative address.
    static constexpr int kFirstReservedBase = 0xFE;
    // The IPL transient work area's base identifier and base sector, both
    // literals in the control processor's IPL code.
    static constexpr uint8_t kIplBase = 0xFE;
    // The chain-terminating header; the address resolver refuses key 255
    // outright, so it carries the disk end and is never addressed.
    static constexpr uint8_t kEndBase = 0xFF;
    static constexpr int kIplBaseSector = 7167;
    // The IPL extent's length is guest[0x0A78] - 7177, 0x0A78 being the IPL
    // region's last sector.
    static constexpr int kIplLengthBias = 7177;
};

// One free run of task work area sectors, chained off a header's +7..9.
//
//   +2..4  the next element
//   +5     base identifier
//   +6..7  sector displacement within the extent
//   +8..9  the run's length in sectors
//   +10    flags
//
// The header is its own first link: the element search starts its cursor at
// header + 5 and reads +2..4 off it, which lands on the header's +7..9.
struct TaskWorkAreaElement {
    static constexpr int kBytes = 16;
    static constexpr int kOffNext = 2;
    static constexpr int kOffBaseIdentifier = 5;
    static constexpr int kOffDisplacement = 6;
    static constexpr int kOffSectors = 8;
    static constexpr int kOffFlags = 10;
    static constexpr int kHeaderAsElement = 5;
};


// The task work area as the swap-area allocator hands it out: the unit is a
// 256-byte sector, the size is (region pages x 8) + 2, or 256 + 2 for the
// maximum, and a reference is architecturally RELATIVE (SA21-9436 3-127).
// The extent size is emulator policy and exists only so that exhaustion is
// reachable and testable.
class TaskWorkArea {
public:
    static constexpr int kSectors = 8192;

    TaskWorkArea() { free_.emplace_back(0, kSectors); }

    // First fit; the relative sector of the area, or -1.
    int allocate(int sectors)
    {
        if (sectors <= 0) return -1;
        for (std::size_t i = 0; i < free_.size(); i++) {
            if (free_[i].second < sectors) continue;
            int at = free_[i].first;
            if (free_[i].second == sectors) {
                free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                free_[i] = {at + sectors, free_[i].second - sectors};
            }
            return at;
        }
        return -1;
    }

    void free(int at, int sectors)
    {
        if (sectors <= 0 || at < 0) return;
        std::size_t i = 0;
        while (i < free_.size() && free_[i].first < at) i++;
        free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(i), {at, sectors});
        for (std::size_t n = 0; n + 1 < free_.size();) {
            if (free_[n].first + free_[n].second == free_[n + 1].first) {
                free_[n] = {free_[n].first, free_[n].second + free_[n + 1].second};
                free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(n) + 1);
            } else {
                n++;
            }
        }
    }

    void reset()
    {
        free_.clear();
        free_.emplace_back(0, kSectors);
    }

private:
    std::vector<std::pair<int, int>> free_;
};

}  // namespace sim36::processors::controlstorage
