// The MAP parameter list - SVC 2F's only real input, addressed by XR2 - and
// the request block's map table, which is SVC 2F's output and the ATR build's
// input.
//
// SA21-9436 3-123 defers the list to the System Data Areas manual five times
// on one page, so it was recovered from the machine's own code and then
// checked against the manual's worked example, which prints a list and says
// what it does:
//
//   Assuming XR1 contains hex 800000 and location hex 802080 contains (the
//   parameter list) hex 5148010800810000 then the SVC maps the first page
//   (virtual page 0) of the task work space type (hex 81) to logical address
//   hex 5000.  At the end of the SVC, XR1 contains hex 805000 and the program
//   is able to address data from hex 5000 through hex 57FF.  (3-125)
//
// Every field below is confirmed by that one line:
//
//   +0     51    (parm[0] & 0xF8) << 8         "to logical address hex 5000"
//   +1     48    high nibble 4 = the action,   the printed list is exactly 8 bytes
//                low nibble 8 = this entry's length
//   +2     01    selector 1 = USING-XR1        "XR1 contains hex 805000"
//   +3..4  0800  the length in bytes           "hex 5000 through hex 57FF" - 2 KB
//   +5     81    the object's type             "the task work space type (hex 81)"
//   +6..7  0000  the object's id               "the first page (virtual page 0)"
//
// +0 is a packed field: the page is the TOP five bits and the flags are the
// bottom three.  0x51 >> 3 is 10, and 10 pages of 2 KB is hex 5000, which is
// the number the manual prints.
//
// Corroborated a third time by a real caller.  $SYRP, the SSP module packed
// into #MSTWA's extent at member offset 0x4000 and loaded at page 2, issues
// SVC 2F at four sites, each copying one or two entries out of a four-entry
// table of 11-byte templates into XR1+0x48 and pointing XR2 there.  The
// templates differ only in the target page:
//
//   18 5B 08 1000 00 0000 800000     page 3, action 5, USING the list's own
//   29 5B 08 1000 00 0000 800000     +8..10 field, 4096 bytes, and the answer
//   29 5B 08 1000 00 0000 800000     is read back out of that field:
//   29 5B 08 1000 00 0000 800000       MVC q=02 0E(XR1), 52(XR1)
//
// 0x5B's low nibble is 11 - the exact number of bytes the caller's MVC
// copies - and 0x18 >> 3 is 3 while 0x29 >> 3 is 5, two pages apart, which is
// the 0x1000 length.  An eight-byte entry for the action that needs +5..7 and
// an eleven-byte entry for the action that needs +8..10: the length nibble is
// what makes one list hold both.
#pragma once

#include <cstdint>

#include "Machine/MachineState.h"

namespace sim36::processors::controlstorage {

struct MapParameterList {
    // ---- byte 0: the target page, and three flags -------------------------

    // Target logical page, bits 0-4 in IBM numbering.  The byte address is
    // (parm[0] & 0xF8) << 8, i.e. page * 2048.
    static constexpr uint8_t kTargetPageMask = 0xF8;
    static constexpr int kTargetPageShift = 8;

    // Bit 6: the length is not in +3..4; that halfword is instead a halfword
    // index into the register save area naming the register that carries it -
    // the manual's LNGTH-XR1 / LNGTH-WR4...
    static constexpr uint8_t kFlagLengthInRegister = 0x02;

    // Bit 7: this is the last entry.  CLEAR means another entry follows,
    // parm[1] & 0x0F bytes further on.  It also selects what a zero length
    // means: to the end of the region for the last entry, up to the next
    // entry's target otherwise.
    static constexpr uint8_t kFlagLastEntry = 0x01;

    // ---- byte 1: the action, and this entry's length ----------------------

    // High nibble: which object is being mapped.  A ten-way dispatch;
    // anything not listed is supervisor error code 83.
    static constexpr int kActionUnmap = 1;             // register only, and unmap the range
    static constexpr int kActionAddressOnly = 2;       // register only
    static constexpr int kActionAddressOnlyAlt = 3;    // register only
    static constexpr int kActionByTypeAndId = 4;       // find by id and scan the chain
    static constexpr int kActionCallersProgram = 5;    // the request block at rb+3..5
    static constexpr int kActionByBlockAddress = 6;    // the "PB"/"SB" at +8..10
    static constexpr int kActionByRequestBlock = 7;    // the "RB" at +8..10
    static constexpr int kActionOwnProgram = 9;        // this request block's own pb

    // Low nibble: the length of this entry in bytes, and therefore the step to
    // the next one.
    static constexpr uint8_t kEntryLengthMask = 0x0F;

    // ---- byte 2: which register carries the address, and two flags --------

    // Low nibble, minus one, indexes a ten-way jump table.  The value is also
    // the halfword index into the register save area for 1-7, which is why
    // LNGTH- and USING- share a numbering.
    static constexpr uint8_t kRegisterMask = 0x0F;
    static constexpr int kRegisterXr1 = 1, kRegisterXr2 = 2, kRegisterArr = 3;
    static constexpr int kRegisterWr4 = 4, kRegisterWr7 = 7;
    static constexpr int kRegisterListUpdated = 8;    // the +8..10 field, written back
    static constexpr int kRegisterNone = 9;           // no register; source is 0x800000
    static constexpr int kRegisterListReadOnly = 10;  // the +8..10 field, not written back

    // Bit 1: start where the previous entry ended rather than at parm[0]'s
    // page (ignored on the first entry, which is why a template can carry it
    // in every entry).
    static constexpr uint8_t kFlagFollowPrevious = 0x40;

    // Bit 0: passed to the unmap as its fourth argument.  Set means an
    // overlapping entry is DELETED WHOLE; clear means it is trimmed.
    static constexpr uint8_t kFlagUnmapWhole = 0x80;

    // ---- the remaining fields --------------------------------------------

    static constexpr int kOffTargetPage = 0;
    static constexpr int kOffAction = 1;
    static constexpr int kOffRegister = 2;
    static constexpr int kOffLength = 3;        // 2 bytes, or a save-area index
    static constexpr int kOffType = 5;          // 1 byte, action 4
    static constexpr int kOffId = 6;            // 2 bytes, action 4
    static constexpr int kOffAddress = 8;       // 3 bytes, actions 6 and 7, selectors 8 and 10

    // What parm[0]'s page bits mean as a byte address.
    static int targetAddress(uint8_t parm0) { return (parm0 & kTargetPageMask) << kTargetPageShift; }

    // The mask applied to a source address before adding it to the target:
    // keep the offset within the 2 KB page, keep the prefix byte EXCEPT its
    // translated bit, drop the page number.  Every register case uses it.
    static constexpr int kSourceOffsetMask = 0x7F07FF;
};

// The request block's map table.  Its location is rb + 64 + pb[63] * 16: the
// request block's fixed 64 bytes, then the program's private area in 16-byte
// units.  Three routines that do not derive from each other agree on it - the
// MAP writes it, the ATR build reads it, the block sizing reserves room for it.
struct MapTable {
    static constexpr int kEntryBytes = 8;

    static constexpr int kOffStartPage = 0;      // first logical page of the region
    static constexpr int kOffPages = 1;          // how many
    static constexpr int kOffDisplacement = 2;   // 2 bytes, into the object, in 2 KB pages
    static constexpr int kOffBlock = 5;          // 3 bytes, the guest address of the block

    // The heap's 16-byte allocation unit, which is also the request block's
    // length unit (rb+2) and the program's private-area unit (pb+63).
    static constexpr int kUnitBytes = 16;

    static int base(machine::MachineState& m, int rb, int pb);
    static int count(machine::MachineState& m, int rb);
    static void setCount(machine::MachineState& m, int rb, int n);

    // The first byte past this request block: rb + rb[2] * 16.  The MAP
    // raises supervisor error 83 when the next entry would reach it.
    static int blockEnd(machine::MachineState& m, int rb);

    static void write(machine::MachineState& m, int at, int startPage, int pages, int displacement, int block);

    // Make room for a new mapping by removing what the range overlaps.  Every
    // entry of the table is examined:
    //
    // - one that starts at or after the new range's end is left alone;
    // - with `whole` set (parm[2] bit 0), any survivor is deleted by zeroing
    //   its page count;
    // - otherwise the entry is TRIMMED: an entry that ends inside the new
    //   range keeps its head, one that starts inside keeps its tail with its
    //   start page and displacement both advanced, and one that would be cut
    //   in two is left alone.
    static void unmap(machine::MachineState& m, int rb, int pb, int startPage, int pages, bool whole);
};

static_assert(MapTable::kOffBlock + 3 == MapTable::kEntryBytes, "a map table entry is 8 bytes");

}  // namespace sim36::processors::controlstorage
