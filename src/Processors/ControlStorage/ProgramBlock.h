// The program block, the job control block, the storage block, the constants
// the program and storage blocks share, and the load member header.
//
// The three control blocks are accessors over LIVE guest storage, deliberately,
// rather than structs that are unmarshalled, mutated and written back.  Two
// things make copy-in/copy-out the wrong shape here:
//
// The guest owns these bytes too.  Real SSP code reads and writes control
// blocks between supervisor calls - `LA q=A1` is a write to the request
// block's XR1 field - so a private copy would diverge from what the MSP sees
// for as long as it was held.  Reading and writing through to main storage
// means there is exactly one copy and no window.
//
// The layout is partly known.  Naming a field is a finding; the bytes in
// between are not padding, they are unread.  A struct implies a completeness
// the research does not have, and marshalling one back would write zeros over
// guest data that something else put there.
//
// The load member header IS a snapshot and is parsed by value, because unlike
// a control block it is a read - sectors copied off the volume into a buffer
// nothing else can touch.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Machine/MachineState.h"

namespace sim36::processors::controlstorage {

// Offsets and constants shared by the 64-byte program block and the 48-byte
// storage block.  Kept apart from ProgramBlock and StorageBlock because these
// are the fields the two forms SHARE - every one of them is written by the one
// control-block factory.
struct ControlBlock {
    // The factory's first argument: type 1 selects the 64-byte "PB".
    static constexpr uint8_t kTypeProgramBlock = 1;

    // Type hex CF also gets the 64-byte form.
    static constexpr uint8_t kTypeProgramBlockAlt = 0xCF;

    // The world splits at 128: below it a block is a SYSTEM work space on
    // queue header 42, at or above it a TASK work space on the owning task
    // block's own chain.  SA21-9436's own MAP example uses type hex 81 and
    // calls it "the task work space type".
    static constexpr uint8_t kTypeTaskOwned = 128;

    // Queue header 42, guest 0xBAB - where every system work space is chained.
    static constexpr int kSystemWorkSpaceQueue = 42;

    // Queue header 39, the task block chain the post-by-id walks.
    static constexpr int kTaskQueue = 39;

    // tb+43 - the head of a task's own work space chain, named by its LAST
    // byte the way every System/36 pointer field is.
    static constexpr int kTaskWorkSpaceChain = 43;

    // The eight-way transfer hash at guest 0xD00, whose chain heads end at
    // 0xD03 + 4n.
    static constexpr int kProgramBlockHashBase = 0xD03;

    // The chain field every one of these queues links through, named by its
    // LAST byte - the queue engine is passed 11, and the three bytes are
    // +9..11, which is ProgramBlock::kOffChainLink.
    static constexpr int kChainLast = 11;

    // The factory queues with flags 16 and the block delete dequeues with 32 -
    // LIFO and dequeue in SVC 0E's inline 3.
    static constexpr uint8_t kQueueLifo = 0x10, kQueueDequeue = 0x20;

    // +8 bit 0x40: this block owns a task work area swap area.  The factory
    // allocates one, the block delete frees it.
    static constexpr uint8_t kFlagSwapArea = 0x40;

    // +8 bit 0x10: the swap area is the fixed maximum rather than region-sized.
    // SA21-9436 3-126's Q-byte bit 3, "allocate maximum swap area".
    static constexpr uint8_t kFlagMaximumSwapArea = 0x10;

    // +8 bit 0x02: the deactivate will not delete the block even when its last
    // reference goes.
    static constexpr uint8_t kFlagPinned = 0x02;

    // +8 bit 0x80: suppress the deactivate's underflow error 111.
    static constexpr uint8_t kFlagNoUnderflowError = 0x80;

    // +52 bit 0x40 keeps a program block off the transfer hash chain.
    static constexpr uint8_t kAttributeNotHashed = 0x40;

    // The "maximum" swap area is a flat 256 sectors, before the +2 both arms
    // add.
    static constexpr int kMaximumSwapAreaSectors = 256;

    // The task work area allocator's failure return is 0x00FF4000; the factory
    // turns it into 0x800000 | (code & 0xFFFF), and the task attach uses the
    // low sixteen bits two ways: it tests the high byte for 0x40 and passes
    // the whole halfword to the general wait as a mask.  Those sixteen bits
    // are TB_WMASK with inline parameter 1 bit 0x40 on - SA21-9436 3-69's
    // "task work area allocate failure".
    static constexpr int kTaskWorkAreaFullCode = 0x4000;

    // +22, a halfword the factory stores 0xFFFF into.  Nothing read so far
    // reads it back.
    static constexpr int kOffMinusOne = 22;

    // +27, the use count the activate increments and the deactivate
    // decrements.  A byte, saturating at 255.
    static constexpr int kOffUseCount = 27;

    // +44..45, the halfword the use count overflows into.
    static constexpr int kOffUseCountOverflow = 44;
    // +42, the factory's second copy of +18.
    static constexpr int kOffPagesMappedCopy = 42;
    // +13..15, the head of a chain of 32-byte main storage elements the
    // delete frees one at a time; the link is each element's +3..5.
    static constexpr int kOffStorageChain = 13;
    static constexpr int kStorageChainLink = 3;
    static constexpr int kStorageElementBytes = 32;
};

struct LoadMemberHeader;

// The 64-byte program block a transfer builds.
struct ProgramBlock {
    // The factory allocates 64 bytes for eyecatcher "PB", matching the
    // 64-byte stride the IPL uses for its own pb/rb pair.
    static constexpr int kBytes = 64;

    static constexpr int kOffEyecatcher = 0;
    static constexpr int kOffFlags = 8;          // 0x40 cleared on build, set when ready
    static constexpr int kOffChainLink = 9;      // 3 bytes ending at +11: the hash chain
    static constexpr int kOffLoadPage = 12;      // the module's page; address is this << 11

    // NOTE: pb+24..26 is NOT here on purpose.  It is the swap area / task work
    // area reference - a one-based DISK sector, +1 - and
    // StorageBlock::kOffDiskAddress is its definition.  Where a module's bytes
    // sit in MAIN STORAGE is a System/36 divergence from the Advanced/36 and
    // is tracked outside guest storage.

    // A third page count.  The region get writes +18's value into it and into
    // +20 in the same two stores, so it tracks the others; what distinguishes
    // it is NOT established - nothing read so far ever reads it back.
    static constexpr int kOffPagesThird = 16;    // 2 bytes
    static constexpr int kOffPageCount = 18;     // 2 bytes
    static constexpr int kOffPagesReady = 20;    // 2 bytes; the transfer compares 20 against 18
    static constexpr int kOffOwningTask = 37;    // 3 bytes; a 0xFFFFFF sentinel on this path
    static constexpr int kOffSector = 48;        // 3 bytes: the search key, 1-based
    static constexpr int kOffSectors = 51;       // length, in sectors
    static constexpr int kOffAttribute = 52;     // the transfer control table entry's +4
    static constexpr int kOffMode = 56;          // bit 0x80 -> the module runs translated
    static constexpr int kOffFlags57 = 57;
    static constexpr int kOffName = 58;          // 5 bytes, the member name
    static constexpr int kNameBytes = 5;
    static constexpr int kOffRequestBlockUnits = 63;   // a FLOOR, not the length

    // Bit 0x40 of +8: set once the module is in.
    static constexpr uint8_t kFlagReady = 0x40;

    // Module page number to its guest byte address.  2 KB pages, the same
    // unit the translation hardware uses.
    static constexpr int kLoadPageShift = 11;

    // Bits of the attribute byte that suppress the module mapping entirely:
    // the ATR build tests pb+52 & 0x12 and returns.
    static constexpr uint8_t kAttributeNoMapping = 0x12;

    // Attribute bit 0x02: the module is addressed IN PLACE.  Three routines
    // read it as one switch, and phase 1's entry attribute is 0x24, so for
    // phase 1 it is CLEAR and the module gets storage of its own: the ready
    // path reads into the pb+24..26 block, the transfer resolves the entry
    // point table through the task's ATRs, and the ATR build maps the ATRs to
    // that storage.
    static constexpr uint8_t kAttributeAddressedInPlace = 0x02;

    static int sector(machine::MachineState& m, int pb) { return m.readAddr24(pb + kOffSector); }
    static int sectors(machine::MachineState& m, int pb) { return m.readByte(pb + kOffSectors); }
    static uint8_t attribute(machine::MachineState& m, int pb) { return m.readByte(pb + kOffAttribute); }
    static uint8_t mode(machine::MachineState& m, int pb) { return m.readByte(pb + kOffMode); }
    static int loadPage(machine::MachineState& m, int pb) { return m.readByte(pb + kOffLoadPage); }
    static int pageCount(machine::MachineState& m, int pb) { return m.readHalf(pb + kOffPageCount); }
    static int pagesReady(machine::MachineState& m, int pb) { return m.readHalf(pb + kOffPagesReady); }
    static int chainLink(machine::MachineState& m, int pb) { return m.readAddr24(pb + kOffChainLink); }

    // The module's LOGICAL address, pb[12] << 11 - where it is addressed, not
    // where its bytes are.
    static int moduleAddress(machine::MachineState& m, int pb) { return loadPage(m, pb) << kLoadPageShift; }

    // The factory's block plus the transfer's transcription of the transfer
    // control table entry - the two are one step from a caller's point of
    // view.
    static void build(machine::MachineState& m, int pb, int sector, int sectors, uint8_t attribute);

    // The ready path's second half: stamp the module's own header into the
    // block.  Every field here is one store in the ready routine.
    static void applyHeader(machine::MachineState& m, int pb, const LoadMemberHeader& hdr);
};

// The job control block - the storage-region half of it, which is all that
// can be sourced.
//
// SA21-9436 names the JCB repeatedly (SVC 13 takes "the address of the JCB"
// in XR1 for its region functions; SVC 51 takes a disk address from
// "JCBWSWA") and defers its layout to the System Data Areas manual.  The
// fields below are therefore taken from the machine's own code, each
// corroborated by more than one routine:
//
// - tb+21..23 is the JCB pointer, three ways: the region get reads the region
//   through it, the task work area access reads it and raises error 106 when
//   it is zero, and the translated-assign heap reaches it the same way.
// - +137 is the region size in pages: the entire body of SVC 13 function 04
//   stores WR6's low byte there and does nothing else, and both the region
//   get and the heap read it back as the ceiling on growth.
// - +92 is the region's current size, held as pages * 8: the region get
//   writes it as (pages & 0x1F) * 8 and reads it back on the arm that does
//   not grow.
// - +90..91 is a halfword counter the heap increments and the region get
//   tests masked to 0x7F to decide whether to grow at all.
// - +89 takes +137's value when the counter is zero.
// - +80..82 is JCBWSWA, the work space's disk address, read by the task work
//   area access.
struct JobControlBlock {
    // tb+21..23, decimal: the 3-byte guest address of the JCB.
    static constexpr int kTaskBlockPointer = 21;

    static constexpr int kOffWorkSpaceDiskAddress = 80;  // 3 bytes, "JCBWSWA"
    // #CPON sets bit 0x20 after constructing the JCB; #CLSS tests bit 0x40
    // before using its job work spaces.
    static constexpr int kOffStatus = 0x35;
    static constexpr uint8_t kStatusBuilt = 0x20;
    static constexpr uint8_t kStatusWorkSpacesPresent = 0x40;
    // Job/class state byte observed by the #CIML/#CLSS chain.
    static constexpr int kOffClassState = 0x36;
    // Task associated with this JCB, also used by QH112 activation elements
    // and JCB-owned resource queues.
    static constexpr int kOffCurrentTask = 77;
    static constexpr int kOffRegionCeiling = 89;         // 1: +137 latched when +90 is zero
    static constexpr int kOffGrowthCounter = 90;         // 2: the region get tests this & 0x7F
    static constexpr int kOffRegionCurrent = 92;         // 1: the current size, pages * 8
    static constexpr int kOffRegionPages = 137;          // 1: the region size, in 2 KB pages

    // +92 is stored and read as pages * 8 - eight 256-byte sectors to a 2 KB
    // page, the same unit the load member header counts in.
    static constexpr int kRegionPageShift = 3;

    static int regionPages(machine::MachineState& m, int jcb) { return m.readByte(jcb + kOffRegionPages); }

    static int currentPages(machine::MachineState& m, int jcb)
    {
        return m.readByte(jcb + kOffRegionCurrent) >> kRegionPageShift;
    }

    static void setCurrentPages(machine::MachineState& m, int jcb, int pages)
    {
        // The region get keeps five bits before it shifts, so a page count of
        // 32 or more wraps to zero here on the real machine too.
        m.writeByte(jcb + kOffRegionCurrent, static_cast<uint8_t>((pages & 0x1F) << kRegionPageShift));
    }
};

// The storage block - the "SB" a work space is described by, and the operand
// of SVC 2C, SVC 2D and half of SVC 2F.
//
// SA21-9436 defers it to System Data Areas.  The fields are recoverable
// anyway, from the routine that BUILDS one: it takes a type, a flags byte, a
// size and a page count, and its stores are the layout.
//
//   +0     2  eyecatcher, "SB" hex E2C2 - "PB" hex D7C2 for the 64-byte form
//   +4     1  the type
//   +8     1  flags; the assign heap sets bit 0 on a failed assign
//   +10..     the allocation table SVC 2C works over
//   +16    2  the work space's size in 2 KB pages; an assign longer than
//             sb[16] * 2048 is refused
//   +18    2  pages currently mapped; the ATR build bounds its inner loop by it
//   +24..26 3 a task work area DISK address, one-based: the block's backing
//             store
//   +42    2  a second copy of +18
//
// A storage block is 48 bytes and a program block 64.
//
// +24..26 being a disk address is the whole of the paging problem.  The
// Advanced/36 points a translation register straight at the memory-mapped
// sector, because its host has a single-level store.  A real System/36 has to
// bring those sectors into real storage first.
struct StorageBlock {
    static constexpr int kBytes = 48;
    static constexpr int kOffEyecatcher = 0;
    static constexpr int kOffType = 4;
    static constexpr int kOffFlags = 8;
    static constexpr int kOffAllocationTable = 10;
    static constexpr int kOffSizePages = 16;      // 2 bytes: the arena, in 2 KB pages
    static constexpr int kOffPagesMapped = 18;    // 2 bytes
    static constexpr int kOffDiskAddress = 24;    // 3 bytes, one-based sector

    // 2 bytes: the domain-inclusion count.  SVC 2F bumps it whenever it maps
    // a work-space block into a request block's domain, alongside the use
    // count at +27/+44.  This count is SEPARATE from the use count: it tracks
    // how many domains reference the block, so a shared block (e.g. the IPL
    // task's console E6 work space) is not freed while another task has it
    // mapped.
    static constexpr int kOffDomainUseCount = 40; // 2 bytes, 0x28

    // The block records its own assign failure.
    static constexpr uint8_t kFlagAssignFailed = 0x01;

    // The arena SVC 2C assigns from, in bytes.
    static int sizeBytes(machine::MachineState& m, int sb)
    {
        return m.readHalf(sb + kOffSizePages) * machine::MachineState::kPageBytes;
    }
};

// The first bytes of a load member.
struct LoadMemberHeader {
    static constexpr int kOffName = 4;
    static constexpr int kOffEntryPointTable = 10;   // 2 bytes, relative to the module
    static constexpr int kOffMode = 14;
    static constexpr int kOffFlags = 15;
    static constexpr int kOffRequestBlockUnits = 16;
    static constexpr int kOffLoadPage = 17;
    static constexpr int kOffSectors = 18;
    // A load member header needs at least 19 bytes.
    static constexpr int kMinimumBytes = kOffSectors + 1;

    std::array<uint8_t, ProgramBlock::kNameBytes> name{};   // 5 bytes, EBCDIC
    uint8_t mode = 0;                    // -> pb+56; bit 0x80 = run translated
    uint8_t flags = 0;                   // -> pb+57 (& 0x3F)
    uint8_t requestBlockUnits = 0;       // -> pb+63
    uint8_t loadPage = 0;                // -> pb+12
    uint8_t sectors = 0;                 // the member's own size

    // Size in 2 KB pages: eight 256-byte sectors to a page.
    int pages() const { return sectors >> 3; }

    // False when the image is shorter than 19 bytes; `hdr` is then untouched.
    static bool parse(const uint8_t* image, int length, LoadMemberHeader& hdr);

    std::string toString() const;
};

}  // namespace sim36::processors::controlstorage
