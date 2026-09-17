// The action control element, the event control mask and the request block:
// the three guest structures every delayed supervisor call runs through.
// Offsets are named here so the emulator never uses a bare number.
#pragma once

#include <cstdint>

#include "Machine/MachineState.h"

namespace sim36::processors::controlstorage {

// A 32-byte block in guest storage that every delayed SVC runs through.
// Delayed is exactly R-bytes 40-52, so this is how all System/36 I/O is
// represented.
class ActionControlElement {
public:
    static constexpr int kSize = 32;
    static constexpr uint16_t kEyecatcher = 0xC1C3;   // EBCDIC "AC"

    static constexpr int kOffEyecatcher = 0;   // 2 bytes
    static constexpr int kOffChainLink = 2;    // 3 bytes, 24-bit guest address
    static constexpr int kOffTbByte7 = 5;      // 1
    static constexpr int kOffRbCounter = 6;    // 2, from rb+24
    static constexpr int kOffInlineParm1 = 8;  // 1, the system queue header number
    static constexpr int kOffPointer9 = 9;     // 3, read as one 24-bit value
    static constexpr int kOffXr1 = 13;         // 3, also the event control mask address
    static constexpr int kOffXr2 = 16;         // 3
    static constexpr int kOffTaskBlock = 19;   // 3
    // EVENT TYPE, 2 bytes: the WR6 value the device SVCs and SVC 2B pass when
    // their Q bit 5 is on, tested against the waiter's WR6 and handed back.
    static constexpr int kOffEventType = 22;
    static constexpr int kOffZero24 = 24;      // 4
    static constexpr int kOffFlags = 28;       // 1: 0x80 | (Q & 0x08), plus producer flags
    static constexpr int kOffXr1Copy = 29;     // 3

    static constexpr uint8_t kFlagsBase = 0x80;
    static constexpr uint8_t kFlagsMultipleWait = 0x08;      // from Q-byte bit 4
    // Marks an internally generated condition; an internal post produces 0xC8.
    static constexpr uint8_t kFlagsInternalCondition = 0x40;
    // Q-byte bit 3 (IBM numbering): post completion to the task block whose
    // address the caller supplied in XR2.
    static constexpr uint8_t kQDifferentTask = 0x10;

    // The chain field as the queue engine is told about it: byte 4, the LAST
    // of the three at +2..4.
    static constexpr int kChainLastByte = kOffChainLink + 2;

    static void build(machine::MachineState& m, int ace, int rb, int tb, uint8_t qByte);
    static void applyTaskAssociation(machine::MachineState& m, int ace, uint8_t qByte);
};

static_assert(ActionControlElement::kOffXr1Copy + 3 == ActionControlElement::kSize, "ACE layout is 32 bytes");

// The event control mask: seven bytes, and the first seven bytes of every
// device IOB.
class Ecm {
public:
    static constexpr int kSize = 7;
    static constexpr int kOffAceAddress = 2;   // 3 bytes; zero means "do not post"
    static constexpr int kOffMultiWait = 5;    // bit 0x80
    static constexpr int kOffCompletion = 6;   // bit 0x40 = complete, low nibble = code
    // General Post treats the two bytes immediately after the ordinary ECM
    // as its 16-bit condition mask (NuEmul::nugpstcs reads ECM+7).
    static constexpr int kOffGeneralPostMask = 7;
    static constexpr uint8_t kComplete = 0x40;

    static bool isComplete(machine::MachineState& m, int ecm)
    {
        return (m.readByte(ecm + kOffCompletion) & kComplete) != 0;
    }
    // Arm the mask for a newly accepted asynchronous action: the whole high
    // nibble goes, so a bit an earlier request left standing does not
    // survive into the next one.
    static uint8_t arm(uint8_t completion) { return static_cast<uint8_t>((completion & ~0x02) & 0x0F); }
    // Post completion: the 7th byte becomes hex 4n, and the mask is unlinked.
    static void post(machine::MachineState& m, int ecm, int completionCode);
};

// The request block the supervisor receives, and the MSP's register save
// area, which is the same thing.  XR1 and XR2 are split: a high byte and a
// low halfword, because an index register is 24 bits and its top byte IS the
// PACT prefix for that addressing path.
class RequestBlock {
public:
    // The guest address of the request block this one was called from:
    // request blocks are a stack, and this is its link.
    static constexpr int kOffPrevious = 3;
    static constexpr int kOffPdir = 7;
    static constexpr int kOffXr1High = 9;
    static constexpr int kOffXr2High = 11;
    static constexpr int kOffPiar = 13;
    static constexpr int kOffInline1 = 16;
    static constexpr int kOffInline2 = 17;
    static constexpr int kOffInline3 = 18;
    static constexpr int kOffPrivilege = 19;   // bit 0 tested against the privilege table
    static constexpr int kOffQByte = 21;
    static constexpr int kOffRByte = 22;
    static constexpr int kOffPsr = 23;
    static constexpr int kOffIar = 24;         // 2 bytes: THE instruction address
    static constexpr int kOffXr1Low = 26;
    static constexpr int kOffXr2Low = 28;
    static constexpr int kOffArr = 30;
    static constexpr int kOffWr4 = 32;         // WR4..WR7, 2 bytes each
    static constexpr int kOffLengthUnits = 2;  // the block's own length, in 16-byte units
    // The number of entries in this block's map table, one byte; the block is
    // sized to carry its own map table starting at +64.
    static constexpr int kOffMapEntryCount = 40;
    static constexpr int kOffMapTableBase = 64;
    static constexpr int kOffProgramBlock = 41;      // 3 bytes
    static constexpr int kOffAtrStale = 44;          // bit 0x40: the ATRs are rebuilt on exit
    static constexpr int kOffTransferFlags = 48;
    static constexpr uint8_t kTransferFromFastExit = 0x40;
    static constexpr int kOffSentinel = 49;          // 3 bytes: 0xFFFFFF
    static constexpr int kOffTranslationHandle = 56; // 3 bytes

    static uint16_t readWr(machine::MachineState& m, int rb, int n) { return m.readHalf(rb + kOffWr4 + (n - 4) * 2); }
    static void writeWr(machine::MachineState& m, int rb, int n, uint16_t v) { m.writeHalf(rb + kOffWr4 + (n - 4) * 2, v); }

    // The saved XR1/XR2 pair, by convention: `field` is the raw prefix:reg 24
    // bits, `count` the 2-byte register alone, `realAddress` the pair with bit
    // 0x800000 dropped.  A new call site must choose.
    static int readXr1Field(machine::MachineState& m, int rb) { return (m.readByte(rb + kOffXr1High) << 16) | m.readHalf(rb + kOffXr1Low); }
    static int readXr2Field(machine::MachineState& m, int rb) { return (m.readByte(rb + kOffXr2High) << 16) | m.readHalf(rb + kOffXr2Low); }
    static int readXr1Count(machine::MachineState& m, int rb) { return m.readHalf(rb + kOffXr1Low); }
    static int readXr2Count(machine::MachineState& m, int rb) { return m.readHalf(rb + kOffXr2Low); }
    static int readXr1RealAddress(machine::MachineState& m, int rb) { return readXr1Field(m, rb) & 0x7FFFFF; }
    static int readXr2RealAddress(machine::MachineState& m, int rb) { return readXr2Field(m, rb) & 0x7FFFFF; }
    static void writeXr1(machine::MachineState& m, int rb, int v)
    {
        m.writeByte(rb + kOffXr1High, static_cast<uint8_t>(v >> 16));
        m.writeHalf(rb + kOffXr1Low, static_cast<uint16_t>(v));
    }
    static void writeXr2(machine::MachineState& m, int rb, int v)
    {
        m.writeByte(rb + kOffXr2High, static_cast<uint8_t>(v >> 16));
        m.writeHalf(rb + kOffXr2Low, static_cast<uint16_t>(v));
    }
    // Bit 0 clear means privileged.
    static bool isPrivileged(machine::MachineState& m, int rb) { return (m.readByte(rb + kOffPrivilege) & 0x01) == 0; }
};

}  // namespace sim36::processors::controlstorage
