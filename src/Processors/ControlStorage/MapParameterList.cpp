#include "Processors/ControlStorage/MapParameterList.h"

#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/GuestHeap.h"
#include "Processors/ControlStorage/ProgramBlock.h"

namespace sim36::processors::controlstorage {

static_assert(MapTable::kUnitBytes == GuestHeap::kGranularity, "map table units are heap units");

int MapTable::base(machine::MachineState& m, int rb, int pb)
{
    return rb + RequestBlock::kOffMapTableBase + m.readByte(pb + ProgramBlock::kOffRequestBlockUnits) * kUnitBytes;
}

int MapTable::count(machine::MachineState& m, int rb)
{
    return m.readByte(rb + RequestBlock::kOffMapEntryCount);
}

void MapTable::setCount(machine::MachineState& m, int rb, int n)
{
    m.writeByte(rb + RequestBlock::kOffMapEntryCount, static_cast<uint8_t>(n));
}

int MapTable::blockEnd(machine::MachineState& m, int rb)
{
    return rb + m.readByte(rb + RequestBlock::kOffLengthUnits) * kUnitBytes;
}

void MapTable::write(machine::MachineState& m, int at, int startPage, int pages, int displacement, int block)
{
    m.writeByte(at + kOffStartPage, static_cast<uint8_t>(startPage));
    m.writeByte(at + kOffPages, static_cast<uint8_t>(pages));
    m.writeHalf(at + kOffDisplacement, static_cast<uint16_t>(displacement));
    m.writeAddr24(at + kOffBlock, block);
}

void MapTable::unmap(machine::MachineState& m, int rb, int pb, int startPage, int pages, bool whole)
{
    int table = base(m, rb, pb);
    int n = count(m, rb);
    int newEnd = (startPage + pages) & 0xFFFF;

    for (int i = 0; i < n; i++) {
        int e = table + i * kEntryBytes;
        int at = m.readByte(e + kOffStartPage);
        int len = m.readByte(e + kOffPages);
        int end = at + len;
        if (end <= 0) continue;                 // an emptied entry
        if (end <= startPage) continue;         // entirely before the new range
        if (at >= newEnd) continue;             // entirely past the new range
        if (whole) {
            m.writeByte(e + kOffPages, 0);
            continue;
        }

        if (end <= newEnd) {
            // The entry ends inside the new range: keep the head.
            int keep = static_cast<int16_t>(len - (end - startPage));
            m.writeByte(e + kOffPages, static_cast<uint8_t>(keep <= 0 ? 0 : keep));
        } else if (at >= startPage) {
            // The entry starts inside the new range: keep the tail, and move
            // its displacement along with its start page.
            int shift = newEnd - at;
            m.writeByte(e + kOffStartPage, static_cast<uint8_t>(at + shift));
            m.writeHalf(e + kOffDisplacement, static_cast<uint16_t>(m.readHalf(e + kOffDisplacement) + shift));
            m.writeByte(e + kOffPages, static_cast<uint8_t>(len - shift));
        }
        // else: the new range is strictly inside the entry.  It is left,
        // because splitting it would need two entries.
    }
}

}  // namespace sim36::processors::controlstorage
