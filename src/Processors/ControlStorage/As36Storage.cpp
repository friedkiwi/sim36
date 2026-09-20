// The Advanced/36 control storage processor: the storage family.
//
// SVC 2C/2D (translated assign and free out of a work space), SVC 2F (MAP,
// which builds map table entries and rebuilds the task's translation
// registers), the translation register builder itself, the control-block
// use counts and deletion, the task work area and SVC 51.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>
#include <set>

#include <fmt/format.h>

#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/MapParameterList.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"

namespace sim36::processors::controlstorage {

// ---- SVC 2C and 2D -----------------------------------------------------------------

// Translated assign / translated free.  XR1 (2C) or XR2 (2D) names the
// storage block, WR6 the length, and with Q bit 5 the mapped address is in
// WR7.  Only an "SB" or "PB" eyecatcher is accepted; +16 is a page count in
// both blocks, so the arena bound reads the same way for either.  Success
// is PSR Equal, failure Low with the block's +8 bit 0 set.
bool As36ControlStorageProcessor::translatedAssignOrFree(SvcRequest& req)
{
    constexpr uint8_t kMappedAddressInWr7 = 0x04;   // Q bit 5, both calls
    constexpr uint8_t kFewestPages = 0x20;          // Q bit 2, assign only
    constexpr uint8_t kWaitForSpace = 0x01;         // Q bit 7, assign only

    int rb = req.requestBlock;
    bool assignCall = req.r == 0x2C;
    int block = assignCall ? RequestBlock::readXr1Field(m_, rb) : RequestBlock::readXr2Field(m_, rb);
    int length = RequestBlock::readWr(m_, rb, 6);
    int mapped = RequestBlock::readWr(m_, rb, 7);
    bool translated = (req.q & kMappedAddressInWr7) != 0;

    uint16_t eye = block == 0 ? static_cast<uint16_t>(0) : m_.readHalf(block + StorageBlock::kOffEyecatcher);
    if (eye != GuestLowStorage::kEyeSystemBlock && eye != GuestLowStorage::kEyeProgramBlock) {
        trace_.csp("SVC {:02X}: {} = {:06X} is neither a storage block nor a program block - its eyecatcher is {:04X}, "
                   "and NuXlateHeap::init accepts only SB and PB (c18cb970)",
                   req.r, assignCall ? "XR1" : "XR2", block, eye);
        return false;
    }

    WorkSpaceHeap* heap = workSpaceFor(block);
    if (heap == nullptr) {
        trace_.csp("SVC {:02X}: storage block {:06X} says its work space is {} page(s) long, so there is nothing to "
                   "assign from (sb+16, the bound getHeap applies at c18cb08c)",
                   req.r, block, m_.readHalf(block + StorageBlock::kOffSizePages));
        return false;
    }

    if (!assignCall) {
        // XR1 is the area, WR6 its length, and with Q bit 5 the mapped
        // address in WR7 has to come back off it first.
        int area = RequestBlock::readXr1RealAddress(m_, rb);
        int displacement = translated ? area - mapped : area;
        if (displacement < 0 || length <= 0) {
            trace_.csp("SVC 2D: XR1 = {:06X} with mapped address {:04X} gives displacement {}, which is not inside the "
                       "work space",
                       RequestBlock::readXr1Field(m_, rb), mapped, displacement);
            return false;
        }
        heap->free(displacement, length);
        trace_.csp("SVC 2D: freed {} bytes at displacement {:04X} of the work space at {:06X}; {} bytes free",
                   WorkSpaceHeap::round(length), displacement, block, heap->available());
        return true;
    }

    uint8_t psr = m_.readByte(rb + RequestBlock::kOffPsr);
    if (length <= 0 || length > WorkSpaceHeap::kMaximumLength || length > StorageBlock::sizeBytes(m_, block)) {
        // The failure is recorded in the block itself before zero is
        // returned.
        m_.writeByte(block + StorageBlock::kOffFlags,
                     static_cast<uint8_t>(m_.readByte(block + StorageBlock::kOffFlags) | StorageBlock::kFlagAssignFailed));
        m_.writeByte(rb + RequestBlock::kOffPsr, static_cast<uint8_t>((psr & ~kPsrClearForLow) | kPsrLow));
        trace_.csp("SVC 2C: {} bytes is outside 1..{} or longer than the work space's {} bytes - Low, and sb+8 bit 0 set",
                   length, WorkSpaceHeap::kMaximumLength, StorageBlock::sizeBytes(m_, block));
        return true;
    }

    bool fewestPages = (req.q & kFewestPages) != 0;
    int at = heap->allocate(length, fewestPages);
    if (at < 0) {
        m_.writeByte(block + StorageBlock::kOffFlags,
                     static_cast<uint8_t>(m_.readByte(block + StorageBlock::kOffFlags) | StorageBlock::kFlagAssignFailed));
        m_.writeByte(rb + RequestBlock::kOffPsr, static_cast<uint8_t>((psr & ~kPsrClearForLow) | kPsrLow));
        trace_.csp("SVC 2C: no run of {} bytes in the work space at {:06X} ({} free){} - Low", WorkSpaceHeap::round(length),
                   block, heap->available(),
                   (req.q & kWaitForSpace) != 0 ? ", and Q bit 7 asks to wait, which needs a dispatcher" : "");
        return true;
    }

    // A program block's work space is its region.  When the mapped area a
    // caller hands in (WR7) plus the assigned displacement ends past the
    // block's published pages, getHeap does not answer Low: it raises the
    // JCB ceiling through mspag000 function 04 and retries, and the larger
    // region is then published exactly as SVC 12 publishes a grown one, so
    // nucratr maps the new task-work-area-backed page before the caller
    // touches it.  The heap's own capacity (+16) is unchanged.
    if (translated && m_.readHalf(block + StorageBlock::kOffEyecatcher) == GuestLowStorage::kEyeProgramBlock) {
        const int loadPage = ProgramBlock::loadPage(m_, block);
        const int endLogical = mapped + at + WorkSpaceHeap::round(length);
        const int neededPages =
            ((endLogical + machine::MachineState::kPageBytes - 1) >> machine::MachineState::kPageShift) - loadPage;
        const int published = ProgramBlock::pageCount(m_, block);
        if (neededPages > published && neededPages + loadPage <= kAtrCount) {
            if (!ensureModuleStoragePages(block, neededPages, "SVC 2C region growth")) {
                trace_.csp("SVC 2C: cannot grow program block {:06X} backing to {} page(s) for the area ending at {:04X}",
                           block, neededPages, endLogical);
                return false;
            }
            m_.writeHalf(block + ProgramBlock::kOffPageCount, static_cast<uint16_t>(neededPages));
            m_.writeHalf(block + ProgramBlock::kOffPagesReady, static_cast<uint16_t>(neededPages));
            // getHeap's JCB traffic (c18cb104..c18cb178): the ceiling at +137
            // is what mspag000 function 04 rewrites, +89 latches the old
            // ceiling while the growth counter at +90..91 is still zero, and
            // the counter is then incremented.  +92 is NOT touched: SSP
            // keeps the region's used size there (in 256-byte units) and
            // both the assign and the later free derive their mapped base
            // from it, so the two must agree.
            int jcb = m_.readAddr24(req.taskBlock + JobControlBlock::kTaskBlockPointer);
            if (jcb != 0) {
                if (m_.readHalf(jcb + JobControlBlock::kOffGrowthCounter) == 0)
                    m_.writeByte(jcb + JobControlBlock::kOffRegionCeiling, m_.readByte(jcb + JobControlBlock::kOffRegionPages));
                if (JobControlBlock::regionPages(m_, jcb) < neededPages)
                    m_.writeByte(jcb + JobControlBlock::kOffRegionPages, static_cast<uint8_t>(neededPages));
                m_.writeHalf(jcb + JobControlBlock::kOffGrowthCounter,
                             static_cast<uint16_t>(m_.readHalf(jcb + JobControlBlock::kOffGrowthCounter) + 1));
            }
            // The current frame's file is rebuilt now.  Every frame beneath
            // it on this task keeps its own ATR file, repointed rather than
            // rebuilt when the callee exits, so the new page would stay
            // protected there: rb+44 bit 0x40 is exactly the "rebuild on
            // resume" request nupexit honours (c18a4184).
            int outer = m_.readAddr24(rb + RequestBlock::kOffPrevious);
            int marked = 0;
            for (int guard = 0; outer != 0 && guard < 64; ++guard) {
                m_.writeByte(outer + RequestBlock::kOffAtrStale,
                             static_cast<uint8_t>(m_.readByte(outer + RequestBlock::kOffAtrStale) | kAtrStaleFlag));
                ++marked;
                outer = m_.readAddr24(outer + RequestBlock::kOffPrevious);
            }
            buildTranslationRegisters(req.taskBlock);
            trace_.csp("SVC 2C: {} outer request block(s) marked for an ATR rebuild on resume (rb+44 bit 40)", marked);
            trace_.csp("SVC 2C: the area ending at logical {:04X} lies past program block {:06X}'s {} published page(s); "
                       "getHeap grows the region to {} page(s) (mspag000 function 04 and retry, c18cb124..c18cb178) and "
                       "publishes it as SVC 12 would; JCB {:06X} ceiling raised and growth counted",
                       endLogical, block, published, neededPages, jcb);
        }
    }

    // The page high-water mark is published in SB+18 and its copy at +42;
    // the translation register builder bounds its mappings by this value,
    // and SB+16 remains capacity.  When the mark grows the task's registers
    // are rebuilt at once, so storage assigned above registers SVC 2F
    // already built as protected becomes addressable before the call
    // returns.
    int highWaterPages = (at + WorkSpaceHeap::round(length) + machine::MachineState::kPageBytes - 1) >>
                         machine::MachineState::kPageShift;
    int oldHighWater = m_.readHalf(block + StorageBlock::kOffPagesMapped);
    if (highWaterPages > oldHighWater) {
        m_.writeHalf(block + StorageBlock::kOffPagesMapped, static_cast<uint16_t>(highWaterPages));
        m_.writeHalf(block + ControlBlock::kOffPagesMappedCopy, static_cast<uint16_t>(highWaterPages));
        trace_.csp("SVC 2C: storage block {:06X} mapped/high-water pages raised {} -> {} (NuXlateHeap::getHeap "
                   "c18cb200/c18cb208)",
                   block, oldHighWater, highWaterPages);
        buildTranslationRegisters(req.taskBlock);
        trace_.csp("SVC 2C: rebuilt task {:06X} ATRs after high-water growth (NuXlateHeap::getHeap -> nucratr "
                   "c18cb218..c18cb23c)",
                   req.taskBlock);
    }

    int answer = kTranslatedBit | ((translated ? mapped + at : at) & 0xFFFF);
    m_.writeByte(rb + RequestBlock::kOffXr1High, static_cast<uint8_t>(answer >> 16));
    m_.writeHalf(rb + RequestBlock::kOffXr1Low, static_cast<uint16_t>(answer));
    m_.writeByte(rb + RequestBlock::kOffPsr, static_cast<uint8_t>((psr & ~kPsrClearForEqual) | kPsrEqual));

    trace_.csp("SVC 2C: assigned {} bytes at displacement {:04X} of the work space at {:06X} -> XR1 = {:06X} ({}, "
               "{}); {} bytes free",
               WorkSpaceHeap::round(length), at, block, answer,
               translated ? fmt::format("translated, mapped at {:04X}", mapped) : std::string("virtual"),
               fewestPages ? "fewest-page placement" : "first-fit placement",
               heap->available());
    return true;
}

// The free space of one work space, created the first time a storage block
// is assigned from and keyed by the block's guest address.  A block that is
// freed and rebuilt at the same address inherits the previous space's
// state, which is the visible edge of this being emulator policy rather
// than the machine's in-block bitmap.
WorkSpaceHeap* As36ControlStorageProcessor::workSpaceFor(int block)
{
    auto it = workSpaces_.find(block);
    int bytes = StorageBlock::sizeBytes(m_, block);
    if (it != workSpaces_.end() && it->second->capacity() == bytes) return it->second.get();
    if (it != workSpaces_.end()) {
        trace_.csp("work space at storage block {:06X} changed size from {} to {} bytes; discarded stale allocator "
                   "state before SVC 2C/2D",
                   block, it->second->capacity(), bytes);
        workSpaces_.erase(it);
    }
    if (bytes <= 0) return nullptr;
    auto heap = std::make_unique<WorkSpaceHeap>(bytes);
    WorkSpaceHeap* raw = heap.get();
    workSpaces_[block] = std::move(heap);
    trace_.csp("SVC 2C: work space for storage block {:06X} is {} page(s), {} bytes", block,
               m_.readHalf(block + StorageBlock::kOffSizePages), bytes);
    return raw;
}

// ---- SVC 2F, MAP ---------------------------------------------------------------------

// "Establishes translated addressability to system work spaces, task work
// spaces, and other programs."  The call reads a parameter list through
// XR2, rewrites the register each entry names to a translated address,
// builds map table entries, and rebuilds the translation registers so the
// new addressability is live on return.
bool As36ControlStorageProcessor::map(SvcRequest& req)
{
    int rb = req.requestBlock;
    int pb = m_.readAddr24(rb + RequestBlock::kOffProgramBlock);
    if (pb == 0) {
        trace_.csp("SVC 2F: request block {:04X} has no program block at +41, and the map table is at rb + 64 + pb[63] * "
                   "16 (nucfmapm)",
                   rb);
        return false;
    }

    // XR2 is compared against 0x800000 and the list is resolved through the
    // task's translation registers when the bit is on.
    int xr2 = RequestBlock::readXr2Field(m_, rb);
    int list;
    if (!resolveTranslated(xr2, list)) {
        trace_.csp("SVC 2F: the parameter list is at translated {:06X} and that page is not mapped", xr2);
        return false;
    }

    return mapParameterListCore(req, rb, pb, list, "SVC 2F");
}

// The shared body: SVC 2F supplies a guest-addressed list, and a transfer
// supplies a list embedded in the callee's load image.
bool As36ControlStorageProcessor::mapParameterListCore(SvcRequest& req, int rb, int pb, int list,
                                                        const std::string& call)
{
    int entries = 0, lastEntry = 0;
    for (;;) {
        uint8_t parm0 = m_.readByte(list + MapParameterList::kOffTargetPage);
        uint8_t parm1 = m_.readByte(list + MapParameterList::kOffAction);
        uint8_t parm2 = m_.readByte(list + MapParameterList::kOffRegister);
        int action = parm1 >> 4;
        int entryLength = parm1 & MapParameterList::kEntryLengthMask;
        bool last = (parm0 & MapParameterList::kFlagLastEntry) != 0;

        // Bit 0x40 continues where the previous entry stopped; it is ignored
        // on the first entry, so a template may carry it in every entry.
        int target = (parm2 & MapParameterList::kFlagFollowPrevious) != 0 && lastEntry != 0
                         ? (m_.readByte(lastEntry + MapTable::kOffStartPage) + m_.readByte(lastEntry + MapTable::kOffPages))
                               << machine::MachineState::kPageShift
                         : MapParameterList::targetAddress(parm0);

        int source, logical;
        bool mapped = mapRegister(rb, parm2, action, target, list, source, logical);

        // The length is a halfword in the list, or, with bit 6 of parm[0],
        // the register the list names by save-area index.
        int length = m_.readHalf(list + MapParameterList::kOffLength);
        if ((parm0 & MapParameterList::kFlagLengthInRegister) != 0) {
            int index = length;
            length = m_.readHalf(rb + RequestBlock::kOffIar + index * 2);
            if (length == 0) mapped = false;
        }

        if (mapped) {
            if (length == 0) {
                // A zero length runs to the end of the 64 KB region on the
                // last entry, and up to the next entry's target page on any
                // other.
                length = 0x10000 - logical;
                if (!last) length = MapParameterList::targetAddress(m_.readByte(list + entryLength)) - target;
            }

            int startPage = target >> machine::MachineState::kPageShift;
            int pages = ((logical + length - 1) >> machine::MachineState::kPageShift) -
                        (logical >> machine::MachineState::kPageShift) + 1;
            // The 32-entry translation window, clamped rather than refused.
            if (startPage + pages > kAtrCount) pages -= startPage + pages - kAtrCount;

            if (pages > 0 && !mapAction(req, action, parm2, list, pb, source, startPage, pages, entries, lastEntry))
                return false;
        }

        if (last) break;
        if (entryLength == 0) {
            trace_.csp("{}: entry at {:06X} is not the last and its length nibble is zero, so the list does not advance",
                       call, list);
            return false;
        }
        list += entryLength;
    }

    // Every completed list is followed by compaction.  This is observable:
    // action 1 leaves zero-length entries in place while the unmap walks,
    // then compaction releases their object references, closes the holes,
    // and decrements rb+40.
    compactMapTable(rb, pb, call);

    // The translation registers are rebuilt with the task block, so the
    // caller resumes with the new mapping already live.
    buildTranslationRegisters(req.taskBlock);
    trace_.csp("{}: {} map table entry(s) built; the block now has {}", call, entries, MapTable::count(m_, rb));
    return true;
}

// Remove every map entry whose page count was reduced to zero by an unmap.
// For each hole the block's reference is released, the remaining bytes move
// down eight, and rb+40 is decremented; nonempty entries are left intact.
void As36ControlStorageProcessor::compactMapTable(int rb, int pb, const std::string& call)
{
    int count = MapTable::count(m_, rb);
    int table = MapTable::base(m_, rb, pb);
    int removed = 0;

    for (int i = 0; i < count;) {
        int entry = table + i * MapTable::kEntryBytes;
        if (m_.readByte(entry + MapTable::kOffPages) != 0) {
            i++;
            continue;
        }

        int block = m_.readAddr24(entry + MapTable::kOffBlock);
        if (block != 0xFFFFFF && heap_.contains(block)) {
            uint16_t eye = m_.readHalf(block);
            if (eye == GuestLowStorage::kEyeProgramBlock || eye == GuestLowStorage::kEyeSystemBlock)
                deactivateControlBlock(block, call + " nucmcomp");
            else
                trace_.csp("{}: nucmcomp removed zero-length entry {}, but block {:06X} has eyecatcher {:04X}; no reference "
                           "released",
                           call, i, block, eye);
        } else if (block != 0xFFFFFF) {
            trace_.csp("{}: nucmcomp removed zero-length entry {}, but block {:06X} is outside the live system queue space",
                       call, i, block);
        }

        for (int source = entry + MapTable::kEntryBytes; source < table + count * MapTable::kEntryBytes; source++)
            m_.writeByte(source - MapTable::kEntryBytes, m_.readByte(source));

        count--;
        MapTable::setCount(m_, rb, count);
        removed++;
    }

    if (removed != 0)
        trace_.csp("{}: nucmcomp removed {} zero-length map entr{}; rb {:06X} now has {} (c1891d20)", call, removed,
                   removed == 1 ? "y" : "ies", rb, count);
}

// One entry's register rewrite.  Each case reads the register, keeps the low
// 11 bits (the offset within a 2 KB page) and the prefix except its
// translated bit, adds the target page address, stores the result back and
// sets the addressing path's PACT prefix to 0x80: MAP turns translation on
// for the path it retargets.  A path that is not already translated is only
// retargeted for actions 4 and 6; otherwise the entry is skipped.  Work
// registers have no prefix byte and no such gate.
bool As36ControlStorageProcessor::mapRegister(int rb, uint8_t parm2, int action, int target, int list, int& source,
                                              int& logical)
{
    int selector = parm2 & MapParameterList::kRegisterMask;
    source = 0;
    logical = 0;

    int prefixAt = -1, valueAt = -1;
    switch (selector) {
        case MapParameterList::kRegisterXr1:
            prefixAt = rb + RequestBlock::kOffXr1High;
            valueAt = rb + RequestBlock::kOffXr1Low;
            break;
        case MapParameterList::kRegisterXr2:
            prefixAt = rb + RequestBlock::kOffXr2High;
            valueAt = rb + RequestBlock::kOffXr2Low;
            break;
        case MapParameterList::kRegisterArr:
            prefixAt = rb + RequestBlock::kOffPdir;
            valueAt = rb + RequestBlock::kOffArr;
            break;
        case 4: case 5: case 6: case 7:
            valueAt = rb + RequestBlock::kOffWr4 + (selector - MapParameterList::kRegisterWr4) * 2;
            break;
        case MapParameterList::kRegisterNone:
            // No register at all: the source is virtual zero with the
            // translated bit on and no address is returned.
            source = kTranslatedBit;
            return true;
        case MapParameterList::kRegisterListUpdated:
        case MapParameterList::kRegisterListReadOnly:
            break;
        default:
            trace_.csp("SVC 2F: register selector {} is outside 1-10, which is nuersvc code 83 (c1891428)", selector);
            return false;
    }

    int value;
    if (selector == MapParameterList::kRegisterListUpdated || selector == MapParameterList::kRegisterListReadOnly) {
        value = m_.readAddr24(list + MapParameterList::kOffAddress);
        if ((value >> 16) < kTranslatedPrefix && action != MapParameterList::kActionByTypeAndId &&
            action != MapParameterList::kActionByBlockAddress)
            return false;
    } else if (prefixAt >= 0) {
        value = (m_.readByte(prefixAt) << 16) | m_.readHalf(valueAt);
        if (m_.readByte(prefixAt) < kTranslatedPrefix && action != MapParameterList::kActionByTypeAndId &&
            action != MapParameterList::kActionByBlockAddress)
            return false;
    } else {
        value = m_.readHalf(valueAt);
    }

    source = value;
    logical = target + (value & MapParameterList::kSourceOffsetMask);

    if (selector == MapParameterList::kRegisterListUpdated) {
        m_.writeAddr24(list + MapParameterList::kOffAddress, kTranslatedBit | (logical & 0xFFFFFF));
    } else if (selector != MapParameterList::kRegisterListReadOnly) {
        m_.writeHalf(valueAt, static_cast<uint16_t>(logical));
        if (prefixAt >= 0) m_.writeByte(prefixAt, kTranslatedPrefix);
    }
    return true;
}

// The action half of one map entry: which object the caller is asking for
// addressability to, and the entry (or entries) that names it.  Actions 2
// and 3 build nothing; action 1 unmaps the range; actions 6 and 9 name one
// block directly; actions 5 and 7 copy another request block's
// addressability; action 4 finds a work space by type and id.
bool As36ControlStorageProcessor::mapAction(SvcRequest& req, int action, uint8_t parm2, int list, int pb, int source,
                                            int startPage, int pages, int& entries, int& lastEntry)
{
    int rb = req.requestBlock;
    bool whole = (parm2 & MapParameterList::kFlagUnmapWhole) != 0;
    int sourcePage = (source & ~kTranslatedBit) >> machine::MachineState::kPageShift;

    switch (action) {
        case MapParameterList::kActionAddressOnly:
        case MapParameterList::kActionAddressOnlyAlt:
            return true;

        case MapParameterList::kActionUnmap:
            MapTable::unmap(m_, rb, pb, startPage, pages, whole);
            trace_.csp("SVC 2F: action 1 unmapped pages {}..{}", startPage, startPage + pages - 1);
            return true;

        case MapParameterList::kActionOwnProgram:
        case MapParameterList::kActionByBlockAddress: {
            int block = action == MapParameterList::kActionOwnProgram ? pb
                                                                       : m_.readAddr24(list + MapParameterList::kOffAddress);
            uint16_t eye = m_.readHalf(block);
            if (eye != GuestLowStorage::kEyeProgramBlock && eye != GuestLowStorage::kEyeSystemBlock) {
                trace_.csp("SVC 2F: action {} names {:06X}, whose eyecatcher {:04X} is neither PB nor SB", action, block, eye);
                return false;
            }
            // The domain count and the use count are taken on the found
            // block, before the displacement compute and its negative-
            // displacement fallback.
            includeInDomain(block, fmt::format("SVC 2F action {}", action));
            activateControlBlock(block, fmt::format("SVC 2F action {}", action));
            int displacement = sourcePage - m_.readByte(block + ProgramBlock::kOffLoadPage);
            if ((displacement & 0xFF00) == 0xFF00) {
                // A negative displacement is not an error: the entry falls
                // back to the caller's own program at zero.
                displacement = 0;
                block = pb;
            }
            return appendMapEntry(rb, pb, startPage, pages, displacement, block, whole, entries, lastEntry);
        }

        case MapParameterList::kActionCallersProgram:
        case MapParameterList::kActionByRequestBlock: {
            int other = action == MapParameterList::kActionCallersProgram
                            ? m_.readAddr24(rb + RequestBlock::kOffPrevious)
                            : m_.readAddr24(list + MapParameterList::kOffAddress);
            if (other == 0) {
                if (action == MapParameterList::kActionCallersProgram) {
                    // With rb+3..5 zero there is no caller's request block to
                    // copy, and the guard ahead of the fallback arm is always
                    // true during SVC 2F, so the entry is skipped, not refused.
                    trace_.csp("SVC 2F: action 5 with rb+3..5 zero builds no entry - nucm1000's this[0x3B0]==R-byte guard "
                               "(c18919c4) is always true during SVC 2F, so it skips to the loop tail");
                    return true;
                }
                trace_.csp("SVC 2F: action 7 names request block 0, whose eyecatcher is not RB - nuersvc code 83 (c1891cd4)");
                return false;
            }
            if (action == MapParameterList::kActionByRequestBlock && m_.readHalf(other) != GuestLowStorage::kEyeRequestBlock) {
                trace_.csp("SVC 2F: action 7 names {:06X}, whose eyecatcher {:04X} is not RB (c1891ccc)", other,
                           m_.readHalf(other));
                return false;
            }
            return mapAnotherRequestBlock(rb, pb, other, sourcePage, startPage, pages, whole, entries, lastEntry);
        }

        case MapParameterList::kActionByTypeAndId:
            return mapByTypeAndId(req, list, pb, sourcePage, startPage, pages, whole, entries, lastEntry);

        default:
            trace_.csp("SVC 2F: action {} is not one nucm1000 dispatches, which is nuersvc code 83 (c18917ac)", action);
            return false;
    }
}

// The common tail: unmap what the new range overlaps, check the entry still
// fits inside the request block, and write it.  A table that is full only
// because this very unmap emptied a slot is compacted first, the narrow
// Advanced/36 compatibility rule shipped SSP relies on; a genuinely full
// table is refused.
bool As36ControlStorageProcessor::appendMapEntry(int rb, int pb, int startPage, int pages, int displacement, int block,
                                                 bool whole, int& entries, int& lastEntry)
{
    MapTable::unmap(m_, rb, pb, startPage, pages, whole);

    int count = MapTable::count(m_, rb);
    int at = MapTable::base(m_, rb, pb) + count * MapTable::kEntryBytes;
    if (at >= MapTable::blockEnd(m_, rb)) {
        bool hole = false;
        int table = MapTable::base(m_, rb, pb);
        for (int i = 0; i < count; i++)
            if (m_.readByte(table + i * MapTable::kEntryBytes + MapTable::kOffPages) == 0) {
                hole = true;
                break;
            }
        if (hole) {
            compactMapTable(rb, pb, "SVC 2F A/36 replacement");
            count = MapTable::count(m_, rb);
            at = MapTable::base(m_, rb, pb) + count * MapTable::kEntryBytes;
            trace_.csp("SVC 2F: reused a map-table slot emptied by the replacement range (Advanced/36 SSP TMCRT+06D9 "
                       "compatibility)");
        }
        if (at >= MapTable::blockEnd(m_, rb))
            return refuse("SVC 2F: map table entry {} would start at {:04X}, past the end of the {}-byte request block - "
                          "nuersvc code 83 (V4R4 c1891834); no zero-length replacement slot exists",
                          count, at, MapTable::blockEnd(m_, rb) - rb);
    }

    MapTable::setCount(m_, rb, count + 1);
    MapTable::write(m_, at, startPage, pages, displacement, block);
    lastEntry = at;
    entries++;
    trace_.csp("SVC 2F: map entry {} at {:04X} - pages {}..{} of the region are block {:06X} from its page {}", count, at,
               startPage, startPage + pages - 1, block, displacement);
    return true;
}

// Actions 5 and 7: give this program the same addressability another
// request block has, for the range of ITS logical pages the caller names.
// The other block's objects are walked in the order the hardware sees them
// (its module first, then its own map table entries) and each one that
// overlaps the source range produces an entry here, moved by target -
// source and with its displacement advanced by however far into the object
// the range starts.  Nothing about the object itself is read except its
// page count.
bool As36ControlStorageProcessor::mapAnotherRequestBlock(int rb, int pb, int other, int sourcePage, int startPage,
                                                         int pages, bool whole, int& entries, int& lastEntry)
{
    int entriesBefore = entries;
    int otherPb = m_.readAddr24(other + RequestBlock::kOffProgramBlock);
    if (otherPb == 0) {
        return refuse("SVC 2F: request block {:06X} has no program block", other);
    }

    int otherRbAt = other >> machine::MachineState::kPageShift;
    int otherRbEnd = ((MapTable::blockEnd(m_, other) - 1) >> machine::MachineState::kPageShift) + 1;
    int sourceEnd = std::min(sourcePage + pages, kAtrCount);
    int shift = startPage - sourcePage;
    int table = MapTable::base(m_, other, otherPb);
    int objects = MapTable::count(m_, other) + 1;

    // Which source pages an object above already covered, so the
    // request-block-own fallback below only fills what the walk left blank.
    bool covered[kAtrCount] = {};

    for (int i = 0; i < objects; i++) {
        int block, at, count, displacement;
        if (i == 0) {
            // The module, skipped for the same pb+52 bits that stop the
            // register builder mapping it.
            if ((ProgramBlock::attribute(m_, otherPb) & ProgramBlock::kAttributeNoMapping) != 0) continue;
            block = otherPb;
            at = ProgramBlock::loadPage(m_, otherPb);
            count = ProgramBlock::pageCount(m_, otherPb);
            int regionPages = m_.readHalf(otherPb + StorageBlock::kOffSizePages);
            int disk = m_.readAddr24(otherPb + StorageBlock::kOffDiskAddress);
            if (disk != 0 && regionPages > count) count = regionPages;
            displacement = 0;
        } else {
            int e = table + (i - 1) * MapTable::kEntryBytes;
            block = m_.readAddr24(e + MapTable::kOffBlock);
            at = m_.readByte(e + MapTable::kOffStartPage);
            count = m_.readByte(e + MapTable::kOffPages);
            displacement = m_.readHalf(e + MapTable::kOffDisplacement);
        }

        trace_.csp("SVC 2F inherit source {}/{}: rb {:06X} object {:06X}, pages {}..{}, displacement {}{}", i, objects - 1,
                   other, block, at, at + std::max(0, count) - 1, displacement,
                   i == 0 ? " (program)" : " (map entry)");

        // Never past the end of the object itself.  PB and SB keep their page
        // count at different offsets (PB+18 is the module size, SB+16 the
        // work-space size, SB+18 an initially-zero "pages mapped" field), and
        // a request block's span is its allocated block.
        uint16_t eye = m_.readHalf(block);
        int objectPages;
        if (eye == GuestLowStorage::kEyeSystemBlock)
            objectPages = m_.readHalf(block + StorageBlock::kOffSizePages);
        else if (eye == GuestLowStorage::kEyeRequestBlock)
            objectPages = ((MapTable::blockEnd(m_, block) - 1) >> machine::MachineState::kPageShift) -
                          (block >> machine::MachineState::kPageShift) + 1;
        else {
            int residentPages = m_.readHalf(block + ProgramBlock::kOffPageCount);
            int regionPages = m_.readHalf(block + StorageBlock::kOffSizePages);
            int disk = m_.readAddr24(block + StorageBlock::kOffDiskAddress);
            // An ATASK program block can describe a virtual region larger
            // than its resident module.  Pages in that tail are backed by
            // the task work area at PB+24, and are valid MAP sources even
            // though PB+18 counts only the resident prefix.
            objectPages = disk != 0 && regionPages > residentPages ? regionPages : residentPages;
        }
        int available = objectPages - displacement;
        if (count > available) count = available;
        if (count <= 0) continue;

        int end = at + count;
        if (end <= sourcePage || at >= sourceEnd) continue;   // no overlap
        if (at < sourcePage) {
            displacement += sourcePage - at;
            at = sourcePage;
        }
        if (end > sourceEnd) end = sourceEnd;
        if (end - at <= 0) continue;

        if (!appendMapEntry(rb, pb, at + shift, end - at, displacement, block, whole, entries, lastEntry)) return false;
        // The inherited-map loop takes a use-count reference on precisely the
        // copied object; compaction's per-entry release is its match.
        if (eye == GuestLowStorage::kEyeProgramBlock || eye == GuestLowStorage::kEyeSystemBlock)
            activateControlBlock(block, "SVC 2F inherited map");
        for (int p = at; p < end && p < kAtrCount; p++) covered[p] = true;
    }

    // The caller's own request-block storage.  On a real System/36 the work
    // base lives in a task work-area storage block the caller's map table
    // names, so the walk above copies it.  This emulator places the request
    // block directly in main storage at its guest address, so there is no
    // such entry; the storage IS part of the caller's addressability, so the
    // source pages inside the request block that nothing above covered are
    // mapped identity through an entry naming the block itself.  Strictly
    // additive, and never at the cost of a slot the machine budgeted for a
    // real entry: when the table is full the compensating entry is dropped.
    int rbAt = otherRbAt;
    int rbEnd = otherRbEnd;
    int from = std::max(rbAt, sourcePage);
    int to = std::min(rbEnd, sourceEnd);
    for (int p = from; p < to;) {
        if (covered[p]) {
            p++;
            continue;
        }
        int run = p;
        while (run < to && !covered[run]) run++;
        int roomAt = MapTable::base(m_, rb, pb) + MapTable::count(m_, rb) * MapTable::kEntryBytes;
        if (roomAt >= MapTable::blockEnd(m_, rb)) {
            trace_.csp("SVC 2F: the request block's own storage (pages {}..{} of rb {:06X}) is left unmapped - the {}-byte "
                       "callee block is full at {} entr{}, and this emulator-only entry stands in for a work-area block "
                       "SLIC would have counted in nuprblen",
                       p, run - 1, other, MapTable::blockEnd(m_, rb) - rb, MapTable::count(m_, rb),
                       MapTable::count(m_, rb) == 1 ? "y" : "ies");
            break;
        }
        if (!appendMapEntry(rb, pb, p + shift, run - p, p - rbAt, other, whole, entries, lastEntry)) return false;
        p = run;
    }

    // A zero-entry action-5/7 MAP is valid guest behaviour, but it is
    // otherwise almost impossible to distinguish "the requested region is
    // absent from the source" from a mapping defect, so the source's actual
    // coverage is reported.  Diagnostic only.
    if (entries == entriesBefore) {
        int moduleAt = ProgramBlock::loadPage(m_, otherPb);
        int modulePages = ProgramBlock::pageCount(m_, otherPb);
        int mapCount = MapTable::count(m_, other);
        trace_.csp("SVC 2F: inherit from request block {:06X} supplied NO coverage for source pages {}..{} (target pages "
                   "{}..{}); source module covers {}..{} and its map table has {} entr{}",
                   other, sourcePage, sourceEnd - 1, startPage, startPage + (sourceEnd - sourcePage) - 1, moduleAt,
                   moduleAt + modulePages - 1, mapCount, mapCount == 1 ? "y" : "ies");
        for (int i = 0; i < mapCount; i++) {
            int e = table + i * MapTable::kEntryBytes;
            int at = m_.readByte(e + MapTable::kOffStartPage);
            int count = m_.readByte(e + MapTable::kOffPages);
            int block = m_.readAddr24(e + MapTable::kOffBlock);
            trace_.csp("SVC 2F:   source map entry {}: pages {}..{}, block {:06X}", i, at, at + count - 1, block);
        }
    }
    return true;
}

// The action-4 arm: find the work space by type and id, take the domain and
// use counts, and append the same displacement actions 6 and 9 compute.
bool As36ControlStorageProcessor::mapByTypeAndId(SvcRequest& req, int list, int pb, int sourcePage, int startPage,
                                                 int pages, bool whole, int& entries, int& lastEntry)
{
    int rb = req.requestBlock;
    uint8_t type = m_.readByte(list + MapParameterList::kOffType);
    int id = m_.readHalf(list + MapParameterList::kOffId);

    std::string why;
    int block = findWorkSpace(req.taskBlock, type, id, why);
    if (block == 0) {
        trace_.csp("SVC 2F: action 4 asks for the work space of type {:02X} and id {:04X} - {}, which is nuersvc code 83 "
                   "(c1891964)",
                   type, id, why);
        return false;
    }

    trace_.csp("SVC 2F: action 4 found type {:02X} id {:04X} at {:06X}", type, id, block);
    includeInDomain(block, "SVC 2F action 4");
    activateControlBlock(block, "SVC 2F");

    // A held Read Input Fields result for this task is delivered onto THIS
    // block's resident frame: the module maps its work space here and its
    // return copy then reads the block at the displacement the read staged,
    // so the block's own resident frame is the one frame both must share.
    deliverDeferredWorkStationInput(req.taskBlock, block, type);

    int displacement = sourcePage - m_.readByte(block + ProgramBlock::kOffLoadPage);
    return appendMapEntry(rb, pb, startPage, pages, displacement, block, whole, entries, lastEntry);
}

// Which chain a work space of a given type is on: a system work space
// (type < 128) is on queue header 42, a task work space on the owning task
// block's own chain at tb+43, where a zero id means the current task.  The
// chain link is +9..11 and the key the one-byte type at +4.
int As36ControlStorageProcessor::findWorkSpace(int taskBlock, uint8_t type, int id, std::string& why)
{
    why.clear();
    int head;

    if (type <= ControlBlock::kTypeTaskOwned - 1) {
        head = GuestLowStorage::kQueueHeaderTable + 4 * ControlBlock::kSystemWorkSpaceQueue + 3;
    } else {
        int tb = findTaskById(id, taskBlock);
        if (tb == 0) {
            why = fmt::format("no task block with id {:04X} is on queue header {} (nuidfind, c18b57d4)", id,
                              ControlBlock::kTaskQueue);
            return 0;
        }
        head = tb + ControlBlock::kTaskWorkSpaceChain;
    }

    int at = m_.readAddr24(head - 2);
    int guard = 0;
    while (at != 0 && ++guard <= 4096) {
        if (m_.readByte(at + StorageBlock::kOffType) == type) return at;
        at = m_.readAddr24(at + ProgramBlock::kOffChainLink);
    }

    why = fmt::format("no control block of type {:02X} is on the chain at {:04X} (nuquscs over +4, link +9..11)", type, head);
    return 0;
}

// A zero id is the CURRENT task; anything else a walk of queue header 39
// comparing tb+2..3 and chaining through tb+25..27.
int As36ControlStorageProcessor::findTaskById(int id, int currentTaskBlock)
{
    if (id == 0) return currentTaskBlock;
    int at = m_.readAddr24(GuestLowStorage::queueHeader(kTaskPriorityQueue));
    for (int steps = 0; at != 0; steps++) {
        if (m_.readHalf(at + TaskBlock::kOffTaskId) == id) return at;
        if (!chainStepValid(at, steps, GuestLowStorage::queueHeader(kTaskPriorityQueue))) return 0;
        at = m_.readAddr24(at + TaskBlock::kOffQueue39Link);
    }
    return 0;
}

// A saved MSP address is a PACT byte followed by a 16-bit address.  With the
// translate bit on, the low 16 bits go through the task's translation
// registers.  Otherwise only the PACT address nibble is part of the physical
// address; its other bits are processor-control flags, not high address bits.
bool As36ControlStorageProcessor::resolveTranslated(int address, int& real)
{
    if ((address & kTranslatedBit) == 0) {
        real = address & ((machine::MachineState::kPactAddressBits << 16) | 0xFFFF);
        return true;
    }
    if (m_.translate(static_cast<uint16_t>(address), machine::MachineState::kAtrTaskGroup0, false, real)) return true;
    real = 0;
    return false;
}

// ---- the translation registers ------------------------------------------------------------

// Build a task's 32 translation registers from its current request block:
// the default is "not mapped" (hex FFFF, any of bits 0-2 raising a storage
// exception when used); ATR 0 is made real for a privileged IPL task only
// and ATR 1 (the system communications area) for every privileged task; the
// module's pages follow unless pb+52 says no mapping; then the request
// block's map table.  The file just filled is the live one.
void As36ControlStorageProcessor::buildTranslationRegisters(int tb)
{
    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    int pb = m_.readAddr24(rb + RequestBlock::kOffProgramBlock);

    // The ownership check.  A mismatch has no decoded behaviour past it, so
    // this refuses to build instead.
    int handle = m_.readAddr24(rb + RequestBlock::kOffTranslationHandle);
    NuPtt* ptt = ptt_.owned(handle, rb);
    if (ptt == nullptr) {
        trace_.csp("nucratr: request block {:04X} does not own ATR file {:04X} (NuPtt[0x128] mismatch at c189717c). SLIC "
                   "would fill through a null pointer here; nothing is decoded past that, so no registers are built",
                   rb, handle);
        selectTranslationFile(rb, "nucratr");
        return;
    }
    uint16_t* atr = ptt->atr;

    for (int i = 0; i < kAtrCount; i++) atr[i] = machine::MachineState::kAtrProtect;

    if (RequestBlock::isPrivileged(m_, rb)) {
        if (tb == kIplTaskBlock) atr[0] = 0;
        atr[1] = 1;
    }

    uint8_t attribute = ProgramBlock::attribute(m_, pb);
    int mapped = 0;
    int first = ProgramBlock::loadPage(m_, pb);
    if ((attribute & ProgramBlock::kAttributeNoMapping) != 0) {
        trace_.csp("nucratr: pb+52 = {:02X} has bit 0x02 or 0x10, so no module pages are mapped", attribute);
    } else {
        // The entry is the real page of logical page pb[12] + i: a bias from
        // where a page is addressed to where its bytes are, which is the
        // identity only for a module addressed in place.
        int bytes = moduleBytes(pb);
        int pages = ProgramBlock::pageCount(m_, pb);
        for (int i = 0; i < pages && first + i < kAtrCount; i++, mapped++)
            atr[first + i] = static_cast<uint16_t>((bytes >> ProgramBlock::kLoadPageShift) + i);

        // An ATASK program owns a larger PB+16 virtual region whose tail is
        // backed by PB+24 in the task work area.  Those pages are part of the
        // program's own addressability just as surely as its resident PB+18
        // prefix; expose them in the task's base ATR image too.
        int regionPages = m_.readHalf(pb + StorageBlock::kOffSizePages);
        int disk = m_.readAddr24(pb + StorageBlock::kOffDiskAddress);
        if (disk != 0 && regionPages > pages && first + pages < kAtrCount) {
            int tail = std::min(regionPages - pages, kAtrCount - first - pages);
            const std::vector<int>* backing = workSpaceResidentPages(pb, "nucratr", pages, tail);
            if (backing != nullptr) {
                for (int i = 0; i < tail; i++, mapped++)
                    atr[first + pages + i] = static_cast<uint16_t>(
                        (*backing)[static_cast<std::size_t>(pages + i)] >> machine::MachineState::kPageShift);
                trace_.csp("nucratr: program block {:06X} task-work-area tail pages {}..{} mapped at region pages {}..{}",
                           pb, pages, pages + tail - 1, first + pages, first + pages + tail - 1);
            }
        }
    }

    applyMapTable(rb, pb, atr);

    trace_.csp("nucratr: {} module ATR(s) from index {} into ATR file {:04X}; ATR 0 {}, ATR 1 {}", mapped, first, handle,
               atr[0] == machine::MachineState::kAtrProtect ? "protected" : "real",
               atr[1] == machine::MachineState::kAtrProtect ? "protected" : "real");

    selectTranslationFile(rb, "nucratr");
}

// The request block's own map table, which is how everything that is not
// the running module becomes addressable.  The table starts at rb + 64 +
// pb[63] * 16 and holds rb+40 entries of eight bytes: +0 first logical page,
// +1 page count, +2..3 displacement into the object in pages, +5..7 the
// block.  A program block's pages use that block's resident backing; a
// storage block's are paged in from the task work area; a request block's
// are resident identity at its guest address.
void As36ControlStorageProcessor::applyMapTable(int rb, int pb, uint16_t* atr)
{
    int entries = MapTable::count(m_, rb);
    if (entries == 0) return;

    int table = MapTable::base(m_, rb, pb);

    for (int i = 0; i < entries; i++) {
        int e = table + i * MapTable::kEntryBytes;
        int start = m_.readByte(e + MapTable::kOffStartPage);
        int pages = m_.readByte(e + MapTable::kOffPages);
        int displacement = m_.readHalf(e + MapTable::kOffDisplacement);
        int block = m_.readAddr24(e + MapTable::kOffBlock);
        if (pages == 0 || block == 0) continue;

        uint16_t eye = m_.readHalf(block + StorageBlock::kOffEyecatcher);
        if (eye == GuestLowStorage::kEyeSystemBlock) {
            int sbPages = m_.readHalf(block + StorageBlock::kOffPagesMapped);
            int sbLimit = sbPages - displacement;
            if (pages > sbLimit) pages = sbLimit;
            if (pages <= 0) {
                trace_.csp("nucratr: storage block {:06X} has no swap area (+24..26 = {:06X}, {} mapped/high-water page(s)) "
                           "- its pages stay protected",
                           block, m_.readAddr24(block + StorageBlock::kOffDiskAddress), sbPages);
                continue;
            }
            const std::vector<int>* resident = workSpaceResidentPages(block, "nucratr", displacement, pages);
            if (resident == nullptr) continue;
            for (int n = 0; n < pages && start + n < kAtrCount; n++)
                atr[start + n] = static_cast<uint16_t>((*resident)[static_cast<std::size_t>(displacement + n)] >>
                                                       machine::MachineState::kPageShift);
            trace_.csp("nucratr: map entry {} - region pages {}..{} are storage block {:06X} pages {}..{} at real "
                       "{:06X}..{:06X}, independently paged from the task work area (nutwbsss -> nudiskaddr on sb+24..26)",
                       i, start, start + pages - 1, block, displacement, displacement + pages - 1,
                       (*resident)[static_cast<std::size_t>(displacement)],
                       (*resident)[static_cast<std::size_t>(displacement + pages - 1)] + machine::MachineState::kPageBytes - 1);
            continue;
        }
        if (eye == GuestLowStorage::kEyeRequestBlock) {
            int rbLast = (MapTable::blockEnd(m_, block) - 1) >> machine::MachineState::kPageShift;
            int rbLimit = rbLast - (block >> machine::MachineState::kPageShift) + 1 - displacement;
            if (pages > rbLimit) pages = rbLimit;
            int rbReal = (block >> machine::MachineState::kPageShift) + displacement;
            for (int n = 0; n < pages && start + n < kAtrCount; n++) atr[start + n] = static_cast<uint16_t>(rbReal + n);
            trace_.csp("nucratr: map entry {} - region pages {}..{} are request block {:06X}'s own storage, resident "
                       "identity at real pages {}..{}",
                       i, start, start + pages - 1, block, rbReal, rbReal + pages - 1);
            continue;
        }
        if (eye != GuestLowStorage::kEyeProgramBlock) {
            trace_.csp("nucratr: map entry {} names {:06X} (eyecatcher {:04X}, neither SB nor PB) - region pages {}..{} "
                       "stay protected",
                       i, block, eye, start, start + pages - 1);
            continue;
        }

        int requested = pages;
        int residentPages = m_.readHalf(block + ProgramBlock::kOffPageCount);
        int resident = std::max(0, std::min(requested, residentPages - displacement));
        if (resident != 0) {
            int page = (moduleBytes(block) >> machine::MachineState::kPageShift) + displacement;
            for (int n = 0; n < resident && start + n < kAtrCount; n++)
                atr[start + n] = static_cast<uint16_t>(page + n);
            trace_.csp("nucratr: map entry {} - region pages {}..{} are program block {:06X} resident pages {}..{}", i,
                       start, start + resident - 1, block, page, page + resident - 1);
        }

        // An ATASK block's PB+16 region may extend past its PB+18 resident
        // module.  The remainder lives in its task-work-area allocation and
        // is paged exactly like an SB when a MAP entry exposes it.
        int tailDisplacement = displacement + resident;
        int tail = requested - resident;
        int regionPages = m_.readHalf(block + StorageBlock::kOffSizePages);
        int disk = m_.readAddr24(block + StorageBlock::kOffDiskAddress);
        if (tail > 0 && disk != 0 && regionPages > residentPages && tailDisplacement < regionPages) {
            tail = std::min(tail, regionPages - tailDisplacement);
            const std::vector<int>* backing = workSpaceResidentPages(block, "nucratr", tailDisplacement, tail);
            if (backing != nullptr) {
                for (int n = 0; n < tail && start + resident + n < kAtrCount; n++)
                    atr[start + resident + n] = static_cast<uint16_t>(
                        (*backing)[static_cast<std::size_t>(tailDisplacement + n)] >> machine::MachineState::kPageShift);
                trace_.csp("nucratr: map entry {} - region pages {}..{} are program block {:06X} task-work-area pages "
                           "{}..{}",
                           i, start + resident, start + resident + tail - 1, block, tailDisplacement,
                           tailDisplacement + tail - 1);
            }
        }
    }
}

// Where a program block's bytes are: its resident backing when the loader
// placed it in the module arena, otherwise its load address.
int As36ControlStorageProcessor::moduleBytes(int pb)
{
    auto it = moduleStorage_.find(pb);
    return it != moduleStorage_.end() ? it->second : ProgramBlock::moduleAddress(m_, pb);
}

// The resident frames of a storage block's pages.  A storage block's pages
// have no home of their own: the block carries a task work area disk
// address at +24..26 and the bytes are reached through it.  Each requested
// page is given a frame once and read from the task work area; the frames
// are independent, with no contiguous-storage requirement.
const std::vector<int>* As36ControlStorageProcessor::workSpaceResidentPages(int sb, const std::string& call,
                                                                              int firstPage, int pageCount)
{
    int disk = m_.readAddr24(sb + StorageBlock::kOffDiskAddress);
    int capacity = m_.readHalf(sb + StorageBlock::kOffSizePages);
    int highWater = m_.readHalf(sb + StorageBlock::kOffPagesMapped);
    if (m_.readHalf(sb + StorageBlock::kOffEyecatcher) == GuestLowStorage::kEyeProgramBlock && capacity > highWater)
        highWater = capacity;
    if (disk == 0 || highWater == 0) {
        trace_.csp("{}: storage block {:06X} has no swap area (+24..26 = {:06X}, {} mapped/high-water page(s)) - its pages "
                   "stay protected",
                   call, sb, disk, highWater);
        return nullptr;
    }
    if (highWater > capacity) highWater = capacity;
    if (firstPage < 0 || firstPage >= highWater || pageCount <= 0) return nullptr;
    int lastPage = firstPage + pageCount;
    if (lastPage > highWater) lastPage = highWater;

    auto it = workSpaceStoragePages_.find(sb);
    if (it == workSpaceStoragePages_.end())
        it = workSpaceStoragePages_.emplace(sb, std::vector<int>(static_cast<std::size_t>(capacity), 0)).first;
    std::vector<int>& frames = it->second;

    // The block's allocation is capacity*8+2 sectors and the stored value
    // is allocation+1; the leading guard sector is skipped.
    int first = taskWorkAreaSectorOf(disk);
    if (first <= 0) {
        trace_.csp("{}: storage block {:06X}'s disk address {:06X} resolves to no task work area extent - its pages stay "
                   "protected",
                   call, sb, disk);
        return nullptr;
    }

    int loaded = 0;
    for (int page = firstPage; page < lastPage; page++) {
        if (frames[static_cast<std::size_t>(page)] != 0) continue;
        int at = allocateModuleStorage(1, call + " workspace page");
        if (at == 0) {
            trace_.csp("{}: no MSP frame for storage block {:06X} page {}; its ATR stays protected", call, sb, page);
            return nullptr;
        }
        std::vector<uint8_t> image(machine::MachineState::kPageBytes);
        int pageSector = first + (page << JobControlBlock::kRegionPageShift);
        for (int sector = 0; sector < (1 << JobControlBlock::kRegionPageShift); sector++)
            diskRead(pageSector - 1 + sector, image.data() + static_cast<std::ptrdiff_t>(sector) * storage::DiskBackend::kSectorBytes,
                     "region page");
        m_.write(at, image.data(), static_cast<int>(image.size()));
        frames[static_cast<std::size_t>(page)] = at;
        loaded++;
    }
    if (loaded != 0)
        trace_.csp("{}: storage block {:06X} paged in {} requested page(s) in range {}..{} (high-water {}) from 1-based "
                   "TWA sector {}; independent ATR frames, no contiguous-storage requirement",
                   call, sb, loaded, firstPage, lastPage - 1, highWater, first);

    return &frames;
}

// ---- the module storage arena --------------------------------------------------------------

// Translated real-page frames in the machine's wider backing, above the
// system queue space's ceiling and below the 13-bit ATR frame limit, so the
// 1 MB SSP reports as installed stays entirely guest-owned.  First fit over
// returned regions, then a bump pointer.
int As36ControlStorageProcessor::allocateModuleStorage(int pages, const std::string& call)
{
    int bytes = pages << machine::MachineState::kPageShift;
    for (std::size_t i = 0; i < moduleStorageFree_.size(); i++) {
        if (moduleStorageFree_[i].second >= bytes) {
            int free = moduleStorageFree_[i].first;
            int left = moduleStorageFree_[i].second - bytes;
            if (left == 0) {
                moduleStorageFree_.erase(moduleStorageFree_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                moduleStorageFree_[i].first += bytes;
                moduleStorageFree_[i].second = left;
            }
            moduleStorageSize_[free] = bytes;
            return free;
        }
    }
    if (moduleStorageNext_ + bytes > kModuleStorageHigh || moduleStorageNext_ + bytes > m_.backingBytes()) {
        int returned = 0, largest = 0;
        for (const auto& range : moduleStorageFree_) {
            returned += range.second;
            if (range.second > largest) largest = range.second;
        }
        trace_.csp("{}: no contiguous storage for {} page(s) - {:05X}..{:05X}, bump {:05X}; {} returned byte(s), largest "
                   "extent {}, {} live allocation(s)",
                   call, pages, kModuleStorageLow, kModuleStorageHigh, moduleStorageNext_, returned, largest,
                   moduleStorageSize_.size());
        return 0;
    }
    int at = moduleStorageNext_;
    moduleStorageNext_ += bytes;
    moduleStorageSize_[at] = bytes;
    return at;
}

// A task region can outgrow the main-storage count ATASK was given: SVC 13
// changes the JCB ceiling and SVC 12 publishes the larger PB page count.  The
// native machine pages that tail from the task work area.  Our PB residency is
// a contiguous host extent, so grow (or move) it before nucratr exposes those
// pages.  Otherwise the arena can hand the unreserved tail to a transient and
// both ATR mappings name the same physical frames.
bool As36ControlStorageProcessor::ensureModuleStoragePages(int block, int pages, const std::string& call)
{
    auto owner = moduleStorage_.find(block);
    if (owner == moduleStorage_.end()) {
        // Ordinary loaded program blocks without arena metadata are addressed
        // in place.  An ATASK block is identifiable by its task-work-area
        // backing flag and may legitimately start with zero resident pages.
        // Give that block its first real frames before SVC 12 maps them.
        if ((m_.readByte(block + ProgramBlock::kOffFlags) & ControlBlock::kFlagSwapArea) == 0 || pages == 0)
            return true;
        int at = allocateModuleStorage(pages, call + " initial residency");
        if (at == 0) return false;
        moduleStorage_[block] = at;
        trace_.csp("{}: program block {:06X} acquired backing for its first {} resident page(s) at {:06X}",
                   call, block, pages, at);
        return true;
    }

    const int oldAt = owner->second;
    auto oldSize = moduleStorageSize_.find(oldAt);
    if (oldSize == moduleStorageSize_.end()) {
        trace_.csp("{}: program block {:06X} has backing {:06X} with no arena allocation", call, block, oldAt);
        return false;
    }
    const int oldBytes = oldSize->second;
    const int wantedBytes = pages << machine::MachineState::kPageShift;
    if (wantedBytes <= oldBytes) return true;

    const int extraBytes = wantedBytes - oldBytes;
    const int oldEnd = oldAt + oldBytes;
    // The usual ATASK case is the arena's newest allocation. Extend its bump
    // allocation in place so no saved ATR image has to change.
    if (oldEnd == moduleStorageNext_ && moduleStorageNext_ + extraBytes <= kModuleStorageHigh &&
        moduleStorageNext_ + extraBytes <= m_.backingBytes()) {
        moduleStorageNext_ += extraBytes;
        oldSize->second = wantedBytes;
        std::fill_n(m_.raw() + oldEnd, extraBytes, static_cast<uint8_t>(0));
        trace_.csp("{}: program block {:06X} backing extended in place from {} to {} page(s) at {:06X}", call, block,
                   oldBytes >> machine::MachineState::kPageShift, pages, oldAt);
        return true;
    }

    // A returned extent can also immediately follow the allocation. Consume
    // just the extra tail and retain the existing real page numbers.
    for (std::size_t i = 0; i < moduleStorageFree_.size(); i++) {
        auto& free = moduleStorageFree_[i];
        if (free.first != oldEnd || free.second < extraBytes) continue;
        if (free.second == extraBytes) {
            moduleStorageFree_.erase(moduleStorageFree_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            free.first += extraBytes;
            free.second -= extraBytes;
        }
        oldSize->second = wantedBytes;
        std::fill_n(m_.raw() + oldEnd, extraBytes, static_cast<uint8_t>(0));
        trace_.csp("{}: program block {:06X} backing extended into returned storage from {} to {} page(s) at {:06X}",
                   call, block, oldBytes >> machine::MachineState::kPageShift, pages, oldAt);
        return true;
    }

    const int newAt = allocateModuleStorage(pages, call + " growth");
    if (newAt == 0) return false;
    std::copy_n(m_.raw() + oldAt, oldBytes, m_.raw() + newAt);
    std::fill_n(m_.raw() + newAt + oldBytes, wantedBytes - oldBytes, static_cast<uint8_t>(0));

    // Fast task switching restores saved ATR images without rebuilding them.
    // Rebase all such images before the old frames return to the arena.
    const uint16_t oldFrame = static_cast<uint16_t>(oldAt >> machine::MachineState::kPageShift);
    const uint16_t newFrame = static_cast<uint16_t>(newAt >> machine::MachineState::kPageShift);
    const int oldPages = oldBytes >> machine::MachineState::kPageShift;
    ptt_.rebaseFrames(oldFrame, newFrame, oldPages);
    const int liveBase = machine::MachineState::kAtrTaskGroup0;
    for (int i = 0; i < kAtrCount; i++) {
        uint16_t& a = m_.atr[liveBase + i];
        if ((a & machine::MachineState::kAtrInvalidMask) == 0 && a >= oldFrame && a < oldFrame + oldPages)
            a = static_cast<uint16_t>(newFrame + (a - oldFrame));
    }

    owner->second = newAt;
    moduleStorageSize_.erase(oldSize);
    returnModuleStorage(oldAt, oldBytes);
    trace_.csp("{}: program block {:06X} backing grew from {} page(s) at {:06X} to {} page(s) at {:06X}; contents "
               "preserved before nucratr publishes the new pages",
               call, block, oldBytes >> machine::MachineState::kPageShift, oldAt, pages, newAt);
    return true;
}

void As36ControlStorageProcessor::returnModuleStorage(int at, int bytes)
{
    moduleStorageFree_.emplace_back(at, bytes);
    std::sort(moduleStorageFree_.begin(), moduleStorageFree_.end(),
              [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
    for (std::size_t i = 0; i + 1 < moduleStorageFree_.size();) {
        auto& here = moduleStorageFree_[i];
        auto& next = moduleStorageFree_[i + 1];
        if (here.first + here.second == next.first) {
            here.second += next.second;
            moduleStorageFree_.erase(moduleStorageFree_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        } else {
            i++;
        }
    }
}

// Give a deleted block's region back, whichever residency map it was in.
// The work space's free-space map is host metadata keyed by the guest block
// address, so it must have the identical lifetime.
void As36ControlStorageProcessor::releaseBlockStorage(int block, const std::string& call)
{
    workSpaces_.erase(block);

    if (currentTransientProgramBlock_ == block) {
        currentTransientProgramBlock_ = 0;
        trace_.csp("{}: deleted current system-transient owner PB {:04X}; NuEmul+5E8 cleared", call, block);
    }

    auto module = moduleStorage_.find(block);
    if (module != moduleStorage_.end()) {
        int at = module->second;
        moduleStorage_.erase(module);
        int bytes = moduleStorageSize_[at];
        moduleStorageSize_.erase(at);
        returnModuleStorage(at, bytes);
        memberByProgramBlock_.erase(block);
        trace_.csp("{}: module storage {:05X} given back by block {:04X}", call, at, block);
    }
    auto ws = workSpaceStorage_.find(block);
    if (ws != workSpaceStorage_.end()) {
        int at = ws->second;
        workSpaceStorage_.erase(ws);
        int bytes = moduleStorageSize_[at];
        moduleStorageSize_.erase(at);
        returnModuleStorage(at, bytes);
        trace_.csp("{}: work space storage {:05X} given back by block {:04X}", call, at, block);
    }
    auto frames = workSpaceStoragePages_.find(block);
    if (frames != workSpaceStoragePages_.end()) {
        std::vector<int> pages = std::move(frames->second);
        workSpaceStoragePages_.erase(frames);
        int released = 0;
        for (int frame : pages) {
            if (frame == 0) continue;
            auto size = moduleStorageSize_.find(frame);
            if (size == moduleStorageSize_.end()) continue;
            int bytes = size->second;
            moduleStorageSize_.erase(size);
            returnModuleStorage(frame, bytes);
            released++;
        }
        trace_.csp("{}: {} page-granular work space frame(s) given back by block {:04X}", call, released, block);
    }
}

std::vector<std::string> As36ControlStorageProcessor::moduleStorageDiagnostics() const
{
    std::vector<std::string> rows;
    int freeBytes = 0, largest = 0, allocatedBytes = 0;
    for (const auto& range : moduleStorageFree_) {
        freeBytes += range.second;
        if (range.second > largest) largest = range.second;
    }
    for (const auto& allocation : moduleStorageSize_) allocatedBytes += allocation.second;
    rows.push_back(fmt::format("module-storage arena {:05X}..{:05X}: bump {:05X}; {} allocation(s), {} bytes live; {} returned "
                               "byte(s), largest contiguous {}",
                               kModuleStorageLow, kModuleStorageHigh, moduleStorageNext_, moduleStorageSize_.size(),
                               allocatedBytes, freeBytes, largest));
    for (const auto& range : moduleStorageFree_)
        rows.push_back(fmt::format("  free {:05X}..{:05X} ({} byte(s), {} page(s))", range.first, range.first + range.second,
                                   range.second, range.second >> machine::MachineState::kPageShift));
    std::vector<std::string> owners;
    std::set<int> ownedAt;
    for (const auto& module : moduleStorage_) {
        int bytes = 0;
        auto size = moduleStorageSize_.find(module.second);
        if (size != moduleStorageSize_.end()) bytes = size->second;
        auto member = memberByProgramBlock_.find(module.first);
        std::string name = member != memberByProgramBlock_.end() ? member->second.name : "?";
        owners.push_back(fmt::format("  live {:05X}..{:05X} PB {:04X} {} ({} byte(s))", module.second, module.second + bytes,
                                     module.first, name, bytes));
        ownedAt.insert(module.second);
    }
    for (const auto& workspace : workSpaceStorage_) {
        int bytes = 0;
        auto size = moduleStorageSize_.find(workspace.second);
        if (size != moduleStorageSize_.end()) bytes = size->second;
        owners.push_back(fmt::format("  live {:05X}..{:05X} SB {:04X} workspace ({} byte(s))", workspace.second,
                                     workspace.second + bytes, workspace.first, bytes));
        ownedAt.insert(workspace.second);
    }
    for (const auto& workspace : workSpaceStoragePages_) {
        int resident = 0;
        for (int at : workspace.second)
            if (at != 0) {
                resident++;
                ownedAt.insert(at);
            }
        owners.push_back(fmt::format("  SB {:04X} workspace: {}/{} page-granular frame(s) resident", workspace.first, resident,
                                     workspace.second.size()));
    }
    for (const auto& allocation : moduleStorageSize_)
        if (ownedAt.count(allocation.first) == 0)
            owners.push_back(fmt::format("  live {:05X}..{:05X} UNOWNED ({} byte(s))", allocation.first,
                                         allocation.first + allocation.second, allocation.second));
    std::sort(owners.begin(), owners.end());
    rows.insert(rows.end(), owners.begin(), owners.end());
    return rows;
}

// ---- control blocks ---------------------------------------------------------------------------

// Increment the block's domain-inclusion count (+0x28, halfword) and
// nothing else: it keeps a shared block pinned while mapped.
void As36ControlStorageProcessor::includeInDomain(int block, const std::string& call)
{
    uint16_t count = m_.readHalf(block + StorageBlock::kOffDomainUseCount);
    m_.writeHalf(block + StorageBlock::kOffDomainUseCount, static_cast<uint16_t>(count + 1));
    trace_.csp("{}: nucincdm - block {:06X} domain-inclusion count +0x28 = {}", call, block, count + 1);
}

// The use count is two counters: a byte at +27 that saturates at 255 and a
// halfword at +44..45 that carries the rest.
void As36ControlStorageProcessor::activateControlBlock(int block, const std::string& call)
{
    uint8_t count = m_.readByte(block + ControlBlock::kOffUseCount);
    int overflow = m_.readHalf(block + ControlBlock::kOffUseCountOverflow);

    if (count == 0xFF || overflow > 0)
        m_.writeHalf(block + ControlBlock::kOffUseCountOverflow, static_cast<uint16_t>(overflow + 1));
    else
        m_.writeByte(block + ControlBlock::kOffUseCount, static_cast<uint8_t>(count + 1));

    trace_.csp("{}: activePP - control block {:06X} use count +27 = {}, +44 = {}", call, block,
               m_.readByte(block + ControlBlock::kOffUseCount), m_.readHalf(block + ControlBlock::kOffUseCountOverflow));
}

// The use count run backwards.  Flags bit 0x02 pins the block: the count
// reaches zero and the block still is not deleted.  Flags bit 0x80
// suppresses the underflow error.
void As36ControlStorageProcessor::deactivateControlBlock(int block, const std::string& call)
{
    int overflow = m_.readHalf(block + ControlBlock::kOffUseCountOverflow);
    uint8_t count;

    if (overflow > 0) {
        m_.writeHalf(block + ControlBlock::kOffUseCountOverflow, static_cast<uint16_t>(overflow - 1));
        count = m_.readByte(block + ControlBlock::kOffUseCount);
        if (count != 0) {
            trace_.csp("{}: nucdactv - control block {:06X} still referenced (+27 = {}, +44 = {})", call, block, count,
                       overflow - 1);
            return;
        }
    } else {
        count = static_cast<uint8_t>(m_.readByte(block + ControlBlock::kOffUseCount) - 1);
        m_.writeByte(block + ControlBlock::kOffUseCount, count);

        if (count == 0xFF) {
            if ((m_.readByte(block + StorageBlock::kOffFlags) & ControlBlock::kFlagNoUnderflowError) == 0)
                trace_.csp("{}: control block {:06X} was already at zero references - nucdactv raises nuerr code 111 "
                           "(c18bb6e0), and flags bit 0x80 is not set to suppress it",
                           call, block);
            trace_.csp("{}: nucdactv - control block {:06X} left at +27 = FF, not deleted", call, block);
            return;
        }
        if (count != 0) {
            trace_.csp("{}: nucdactv - control block {:06X} still referenced (+27 = {})", call, block, count);
            return;
        }
    }

    if ((m_.readByte(block + StorageBlock::kOffFlags) & ControlBlock::kFlagPinned) != 0) {
        trace_.csp("{}: control block {:06X} has no references left, but flags bit 0x02 is on and nucdactv c18bb734 does "
                   "not delete it",
                   call, block);
        return;
    }

    deleteControlBlock(block, call);
}

// SA21-9436 3-127's sentence, in order: "dequeues and frees the program
// block and deallocates any associated swap area in the task work area.
// Any main storage allocated is freed and queued to the free page storage
// block."
void As36ControlStorageProcessor::deleteControlBlock(int block, const std::string& call)
{
    releaseBlockStorage(block, call);
    dequeueControlBlock(block, call);

    int swap = m_.readAddr24(block + StorageBlock::kOffDiskAddress);
    if (swap != 0) {
        uint8_t flags = m_.readByte(block + StorageBlock::kOffFlags);
        int sectors = swapAreaSectors(flags, m_.readHalf(block + StorageBlock::kOffSizePages));
        std::string clearWhy;
        bool cleared = clearTaskWorkArea(swap - 1, sectors, clearWhy);
        if (!cleared)
            trace_.csp("{}: NuTwaClearAction could not clear {} sector(s) at relative sector {}: {}", call, sectors,
                       swap - 1, clearWhy);
        twa_.free(swap - 1, sectors);
        m_.writeAddr24(block + StorageBlock::kOffDiskAddress, 0);
        trace_.csp("{}: swap area of {} sector(s) at relative sector {} {} and deallocated (nudtwl -> NuTwaClearAction, "
                   "c189258c / c18b81e8)",
                   call, sectors, swap - 1, cleared ? "cleared" : "not cleared");
    }

    // A chain of 32-byte elements headed at +13..15 and linked at +3..5:
    // the "main storage allocated" the sentence names.
    int element = m_.readAddr24(block + ControlBlock::kOffStorageChain);
    int freed = 0;
    while (element != 0 && freed < 4096) {
        int next = m_.readAddr24(element + ControlBlock::kStorageChainLink);
        heap_.free(element, ControlBlock::kStorageElementBytes);
        element = next;
        freed++;
    }
    if (freed != 0) {
        m_.writeAddr24(block + ControlBlock::kOffStorageChain, 0);
        trace_.csp("{}: {} main storage element(s) of {} bytes freed from the chain at +13..15 (c18bbd14)", call, freed,
                   ControlBlock::kStorageElementBytes);
    }

    bool programBlock = m_.readHalf(block) == GuestLowStorage::kEyeProgramBlock;
    int bytes = programBlock ? ProgramBlock::kBytes : StorageBlock::kBytes;
    m_.writeHalf(block + StorageBlock::kOffEyecatcher, 0);
    heap_.free(block, bytes);

    trace_.csp("{}: control block {:06X} dequeued and freed ({} bytes). IBM also clears NuEmul[0x5E8] when the block owns "
               "main storage (c18bbd68); that cache is inside SLIC's object and this machine has none",
               call, block, bytes);
}

// Ask which head the block is on and, if there is one, dequeue it.  A block
// on no queue (every task-attached block) is a no-op.
void As36ControlStorageProcessor::dequeueControlBlock(int block, const std::string& call)
{
    int head = queueHeadFor(block, call);
    if (head == 0) {
        trace_.csp("{}: control block {:06X} is on no queue - nucwsbsq returns zero and nucdeqsb has nothing to dequeue "
                   "(c18bbe1c)",
                   call, block);
        return;
    }
    queueOperation(head - 2, block, ControlBlock::kChainLast, ControlBlock::kQueueDequeue);
    trace_.csp("{}: control block {:06X} dequeued from the head at {:04X}", call, block, head);
}

// Which queue a control block lives on: a type-1 block with a sector and
// without the not-hashed attribute is on the transfer hash bucket; a
// system work space on queue header 42; a task work space on its owning
// task's chain; anything else on none.
int As36ControlStorageProcessor::queueHeadFor(int block, const std::string& call)
{
    uint8_t type = m_.readByte(block + StorageBlock::kOffType);

    if (type == ControlBlock::kTypeProgramBlock) {
        int sector = m_.readAddr24(block + ProgramBlock::kOffSector);
        if (sector == 0) return 0;
        if ((m_.readByte(block + ProgramBlock::kOffAttribute) & ControlBlock::kAttributeNotHashed) != 0) return 0;
        return programBlockHashBucket(sector);
    }

    if (type < ControlBlock::kTypeTaskOwned)
        return GuestLowStorage::kQueueHeaderTable + 4 * ControlBlock::kSystemWorkSpaceQueue + 3;

    int tb = m_.readAddr24(block + ProgramBlock::kOffOwningTask);
    if (tb == 0) {
        trace_.csp("{}: control block {:06X} is type {:02X} but has no task block at +37..39, so nucwsbsq gives it no queue "
                   "(c18bc174)",
                   call, block, type);
        return 0;
    }
    return tb + ControlBlock::kTaskWorkSpaceChain;
}

int As36ControlStorageProcessor::programBlockHashBucket(int sector)
{
    return ControlBlock::kProgramBlockHashBase + 4 * (sector & 7);
}

int As36ControlStorageProcessor::swapAreaSectors(uint8_t flags, int regionPages)
{
    return (flags & ControlBlock::kFlagMaximumSwapArea) != 0 ? ControlBlock::kMaximumSwapAreaSectors + 2
                                                             : ((regionPages << 3) & 0xFFF8) + 2;
}

// ---- the task work area ----------------------------------------------------------------------------

// Build queue header 46's "QH" blocks the way the IPL does, once, and only
// if the anchor is empty: a 16-byte block with base identifier 254, base
// sector 7167 and length guest[0x0A78] - 7177, registered whole as free; a
// second block chained after it with base identifier 255 and the disk end
// as its base sector.  Where the blocks sit is this emulator's heap.
void As36ControlStorageProcessor::ensureTaskWorkArea()
{
    int anchor = GuestLowStorage::queueHeader(TaskWorkAreaQueue::kAnchorHeader);
    if (m_.readAddr24(anchor) != 0) return;

    int ipl = heap_.allocate(TaskWorkAreaQueue::kBytes);
    int end = heap_.allocate(TaskWorkAreaQueue::kBytes);
    int free = heap_.allocate(TaskWorkAreaElement::kBytes);
    if (ipl == 0 || end == 0 || free == 0) {
        trace_.csp("task work area: the heap has no room for the two QH blocks csipl builds (16 bytes each) - queue header "
                   "46 stays zero");
        return;
    }
    for (int i = 0; i < TaskWorkAreaQueue::kBytes; i++) {
        m_.writeByte(ipl + i, 0);
        m_.writeByte(end + i, 0);
    }
    for (int i = 0; i < TaskWorkAreaElement::kBytes; i++) m_.writeByte(free + i, 0);

    int sectors = (m_.readAddr24(GuestLowStorage::kTransientRegionEnd) - TaskWorkAreaQueue::kIplLengthBias) & 0xFFFF;

    m_.writeHalf(ipl + TaskWorkAreaQueue::kOffEyecatcher, TaskWorkAreaQueue::kEyecatcher);
    m_.writeByte(ipl + TaskWorkAreaQueue::kOffBaseIdentifier, TaskWorkAreaQueue::kIplBase);
    m_.writeAddr24(ipl + TaskWorkAreaQueue::kOffBaseSector, TaskWorkAreaQueue::kIplBaseSector);
    m_.writeHalf(ipl + TaskWorkAreaQueue::kOffSectors, static_cast<uint16_t>(sectors));
    m_.writeAddr24(ipl + TaskWorkAreaQueue::kOffNext, end);
    m_.writeAddr24(ipl + TaskWorkAreaQueue::kOffFreeChain, free);

    m_.writeByte(free + TaskWorkAreaElement::kOffBaseIdentifier, TaskWorkAreaQueue::kIplBase);
    m_.writeHalf(free + TaskWorkAreaElement::kOffDisplacement, 0);
    m_.writeHalf(free + TaskWorkAreaElement::kOffSectors, static_cast<uint16_t>(sectors));

    m_.writeHalf(end + TaskWorkAreaQueue::kOffEyecatcher, TaskWorkAreaQueue::kEyecatcher);
    m_.writeByte(end + TaskWorkAreaQueue::kOffBaseIdentifier, TaskWorkAreaQueue::kEndBase);
    m_.writeAddr24(end + TaskWorkAreaQueue::kOffBaseSector, m_.readAddr24(GuestLowStorage::kTransientRegionEnd));

    m_.writeAddr24(anchor, ipl);
    trace_.csp("task work area: queue header 46 -> QH {:04X} base {:02X} sector {} for {} sector(s), all free -> QH {:04X} "
               "base {:02X} sector {} (csipl c18326ec..c18327dc)",
               ipl, TaskWorkAreaQueue::kIplBase, TaskWorkAreaQueue::kIplBaseSector, sectors, end,
               TaskWorkAreaQueue::kEndBase, m_.readAddr24(GuestLowStorage::kTransientRegionEnd));
}

// Walk the chain from queue header 46 and return the header whose +5 is this
// base identifier, or 0.
int As36ControlStorageProcessor::taskWorkAreaHeader(int baseIdentifier)
{
    ensureTaskWorkArea();
    int at = m_.readAddr24(GuestLowStorage::queueHeader(TaskWorkAreaQueue::kAnchorHeader));
    for (int steps = 0; at != 0 && steps < 256; steps++) {
        if (m_.readByte(at + TaskWorkAreaQueue::kOffBaseIdentifier) == baseIdentifier) return at;
        at = m_.readAddr24(at + TaskWorkAreaQueue::kOffNext);
    }
    return 0;
}

// A relative disk address: the high byte is a key and the low halfword an
// offset, and the answer is header(key)[11..13] + offset.  Key 255 is
// refused before the lookup.
bool As36ControlStorageProcessor::resolveRelativeDiskAddress(int relative, int& sector, std::string& why)
{
    int key = (relative >> 16) & 0xFF;
    int offset = relative & 0xFFFF;
    sector = 0;

    if (key == TaskWorkAreaQueue::kEndBase) {
        why = "key FF is not an extent - NuTwaHeap::diskAddr compares the key against 255 at c18b860c and abends with "
              "nuerabt code 108 before it looks anything up; csipl uses it for the chain-terminating QH block";
        return false;
    }
    int header = taskWorkAreaHeader(key);
    if (header == 0) {
        why = fmt::format("no QH block on queue header 46 carries base identifier {:02X}, so there is no base sector to add "
                          "the offset to (NuTwaHeap::findHeader c18b9250)",
                          key);
        return false;
    }
    int limit = m_.readHalf(header + TaskWorkAreaQueue::kOffSectors);
    if (limit != 0 && offset >= limit) {
        why = fmt::format("offset {} is past the {}-sector extent QH {:04X} describes at +14", offset, limit, header);
        return false;
    }
    sector = m_.readAddr24(header + TaskWorkAreaQueue::kOffBaseSector) + offset;
    why.clear();
    return true;
}

int As36ControlStorageProcessor::taskWorkAreaSectorOf(int relative)
{
    int sector;
    std::string why;
    return resolveRelativeDiskAddress(relative, sector, why) ? sector : -1;
}

// Zero-fill a whole disk extent before it is returned: SSP relies on a
// newly allocated work space being blank.
bool As36ControlStorageProcessor::clearTaskWorkArea(int relative, int sectors, std::string& why)
{
    int first;
    if (!resolveRelativeDiskAddress(relative, first, why)) return false;
    if (sectors <= 0) {
        why = "a zero sector count";
        return false;
    }
    if (static_cast<long long>(first) - 1 + sectors > disk_.sectorCount()) {
        why = fmt::format("sectors {}..{} are outside the volume ({})", first - 1, first - 2 + sectors, disk_.sectorCount());
        return false;
    }

    uint8_t zero[storage::DiskBackend::kSectorBytes] = {};
    for (int i = 0; i < sectors; i++) diskWrite(first - 1 + i, zero, "NuTwaClearAction");
    why.clear();
    return true;
}

// ---- SVC 51 ---------------------------------------------------------------------------------------------

// Task Work Area Accesses: "used to access the task work area or the other
// areas on the disk".  The type byte (SA21-9436 3-144): bit 4 (0x08) XR1 is
// INDIRECT, the disk address being the three bytes ENDING at XR1; bit 5
// (0x04) the disk address is the job's work space address at JCB+80..82;
// bit 6 (0x02) the address is RELATIVE; bit 7 (0x01) 0 = get, 1 = put.
// Inline 2, the key, is added to the disk address and inline 3 is the
// sector count.
bool As36ControlStorageProcessor::taskWorkAreaAccess(SvcRequest& req)
{
    constexpr uint8_t kTypeIndirect = 0x08;
    constexpr uint8_t kTypeWorkSpace = 0x04;
    constexpr uint8_t kTypeRelative = 0x02;
    constexpr uint8_t kTypePut = 0x01;

    uint8_t type = req.inline1;
    int rb = req.requestBlock;
    int xr1 = RequestBlock::readXr1Field(m_, rb);
    int address;

    if ((type & kTypeWorkSpace) != 0) {
        int jcb = m_.readAddr24(req.taskBlock + JobControlBlock::kTaskBlockPointer);
        if (jcb == 0) {
            trace_.csp("SVC 51: type {:02X} takes the address from JCBWSWA, but task block {:04X} has no JCB at +21 - nuerr "
                       "code 106 (c18922a0). s36refemu builds no job control block because nothing has initiated a job yet",
                       type, req.taskBlock);
            return false;
        }
        address = m_.readAddr24(jcb + JobControlBlock::kOffWorkSpaceDiskAddress);
    } else if ((type & kTypeIndirect) != 0) {
        address = m_.readAddr24(xr1 - 2);
    } else {
        address = xr1;
    }

    if ((type & kTypeRelative) != 0) {
        int relative = address;
        std::string why;
        if (!resolveRelativeDiskAddress(relative, address, why)) {
            trace_.csp("SVC 51: type {:02X} bit 6 makes {:06X} a RELATIVE task work area address - key {:02X}, offset {:04X} "
                       "- and {}",
                       type, relative, (relative >> 16) & 0xFF, relative & 0xFFFF, why);
            return false;
        }
        trace_.csp("SVC 51: relative {:06X} - base identifier {:02X}, offset {:04X} - resolves to 1-based sector {} "
                   "(NuTwaHeap::diskAddr c18b8640: header[11..13] + offset)",
                   relative, (relative >> 16) & 0xFF, relative & 0xFFFF, address);
    }

    long long sector = static_cast<long long>(address) + req.inline2 - 1;   // the wire address is 1-based
    int sectors = req.inline3;
    bool put = (type & kTypePut) != 0;
    int buffer = RequestBlock::readXr2Field(m_, rb);

    if (sectors == 0) {
        trace_.csp("SVC 51: inline parameter 3 is zero, so no sectors are transferred");
        return true;
    }
    if (sector < 0 || sector + sectors > disk_.sectorCount()) {
        trace_.csp("SVC 51: sectors {}..{} are outside the volume ({})", sector, sector + sectors - 1, disk_.sectorCount());
        return false;
    }

    int bytes = sectors * storage::DiskBackend::kSectorBytes;
    std::vector<std::pair<int, int>> bufferExtents;
    if (!m_.guest24Extents(buffer, bytes, !put, bufferExtents)) {
        if ((buffer & kTranslatedBit) != 0) {
            trace_.csp("SVC 51: the data address XR2 = {:06X} is translated and its page is not mapped", buffer);
        } else {
            trace_.csp("SVC 51: {} bytes at guest {:06X} run past the end of main storage", bytes, buffer);
        }
        return false;
    }

    std::vector<uint8_t> image(static_cast<std::size_t>(bytes));
    if (put) {
        m_.readGuest24Range(buffer, image.data(), bytes);
        // An overlay volume accepts writes and holds them in memory, so it is
        // writable as far as the guest is concerned; only a true read-only
        // volume refuses here.
        if (disk_.readOnly() && !disk_.isOverlay()) {
            trace_.csp("SVC 51: put REFUSED - {} is read-only. `make scratch` copies it to var/as36.scratch.img, or attach "
                       "it `overlay` to accept writes in memory without touching the file",
                       disk_.path());
            return false;
        }
        for (int i = 0; i < sectors; i++)
            diskWrite(sector + i, image.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes, "SVC 51 put");
    } else {
        for (int i = 0; i < sectors; i++)
            diskRead(sector + i, image.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes, "SVC 51 get");
        m_.writeGuest24Range(buffer, image.data(), bytes);
    }

    trace_.csp("SVC 51: {} {} sector(s) at {} (address {:06X} + key {:02X}) {} guest {:06X}; type {:02X}, {} physical "
               "span(s), first {:06X}",
               put ? "put" : "get", sectors, sector, address, req.inline2, put ? "from" : "to", buffer, type,
               bufferExtents.size(), bufferExtents.empty() ? 0 : bufferExtents[0].first);
    return true;
}

}  // namespace sim36::processors::controlstorage
