#include "Processors/ControlStorage/ProgramBlock.h"

#include <algorithm>

#include <fmt/format.h>

#include "Processors/ControlStorage/GuestLowStorage.h"

namespace sim36::processors::controlstorage {

static_assert(ProgramBlock::kOffRequestBlockUnits + 1 == ProgramBlock::kBytes, "program block is 64 bytes");
static_assert(StorageBlock::kOffDomainUseCount + 2 <= StorageBlock::kBytes, "storage block is 48 bytes");
static_assert(ControlBlock::kOffUseCountOverflow + 2 <= StorageBlock::kBytes, "use count overflow is inside both forms");
static_assert(ControlBlock::kChainLast == ProgramBlock::kOffChainLink + 2, "the hash chain link ends at +11");

void ProgramBlock::build(machine::MachineState& m, int pb, int sector, int sectors, uint8_t attribute)
{
    for (int i = 0; i < kBytes; i++) m.writeByte(pb + i, 0);
    m.writeHalf(pb + kOffEyecatcher, GuestLowStorage::kEyeProgramBlock);
    // The factory's type argument is 1 on the transfer path and is stored at
    // +4.  The hash-queue lookup later requires it to find the resident PB's
    // hash queue when the block delete unchains the block.
    m.writeByte(pb + StorageBlock::kOffType, ControlBlock::kTypeProgramBlock);
    m.writeAddr24(pb + kOffSector, sector);
    m.writeByte(pb + kOffSectors, static_cast<uint8_t>(sectors));
    m.writeByte(pb + kOffAttribute, attribute);
    m.writeHalf(pb + kOffPageCount, static_cast<uint16_t>((sectors + 7) / 8));
    // The transfer writes a SENTINEL here, not the task block.
    m.writeAddr24(pb + kOffOwningTask, 0xFFFFFF);
    m.writeByte(pb + kOffFlags, static_cast<uint8_t>(m.readByte(pb + kOffFlags) & ~kFlagReady));
}

void ProgramBlock::applyHeader(machine::MachineState& m, int pb, const LoadMemberHeader& hdr)
{
    m.writeByte(pb + kOffMode, hdr.mode);
    m.writeByte(pb + kOffFlags57, static_cast<uint8_t>(hdr.flags & 0x3F));
    m.writeByte(pb + kOffRequestBlockUnits, hdr.requestBlockUnits);
    m.writeByte(pb + kOffLoadPage, hdr.loadPage);
    for (int i = 0; i < kNameBytes; i++) m.writeByte(pb + kOffName + i, hdr.name[static_cast<size_t>(i)]);

    uint8_t flags = static_cast<uint8_t>(m.readByte(pb + kOffFlags) | kFlagReady);
    if ((hdr.flags & 0x20) != 0) flags |= 0x08;
    m.writeByte(pb + kOffFlags, flags);

    int pages = std::max(pageCount(m, pb), hdr.pages());
    m.writeHalf(pb + kOffPageCount, static_cast<uint16_t>(pages));
    m.writeHalf(pb + kOffPagesReady, static_cast<uint16_t>(pages));
}

bool LoadMemberHeader::parse(const uint8_t* image, int length, LoadMemberHeader& hdr)
{
    if (image == nullptr || length < kMinimumBytes) return false;
    LoadMemberHeader h;
    h.mode = image[kOffMode];
    h.flags = image[kOffFlags];
    h.requestBlockUnits = image[kOffRequestBlockUnits];
    h.loadPage = image[kOffLoadPage];
    h.sectors = image[kOffSectors];
    std::copy(image + kOffName, image + kOffName + ProgramBlock::kNameBytes, h.name.begin());
    hdr = h;
    return true;
}

std::string LoadMemberHeader::toString() const
{
    return fmt::format("+14={:02X} +15={:02X} +16={:02X} +17={:02X} +18={:02X}",
                       mode, flags, requestBlockUnits, loadPage, sectors);
}

}  // namespace sim36::processors::controlstorage
