// A device input/output block.  Its first seven bytes are the event control
// mask, which holds for every IOB the machine defines.
#pragma once

#include <cstdint>
#include <string>

#include "Machine/MachineState.h"

namespace sim36::devices {

class IoBlock {
public:
    // "The IOB contains a command in bytes 0A and 0B of the input/output
    // block" (SA21-9436 5-50).  The disk dispatches on +0x0A alone (A0..A4)
    // and reads +0x0B only inside a command's own path, so +0x0A is the
    // COMMAND and +0x0B a MODIFIER.
    static constexpr int kOffCommand = 0x0A;
    static constexpr int kOffCommandModifier = 0x0B;

    // Modifier on command A2: byte 13 is a fill character rather than a
    // buffer address.
    static constexpr int kModifierFill = 0xC0;
    // Modifier on command A2: data field wrap, the same 256-byte area is
    // written to every sector of the transfer (SA21-9243-4 figure 6-4 part
    // 6 bit 0).
    static constexpr int kModifierWrap = 0x80;

    // Disk-specific: the count at 16 (held as count-1), the sector at 19
    // (1-based on the wire), the work sector at 23 (advanced during the
    // transfer).
    static constexpr int kOffDiskCount = 16;
    static constexpr int kOffDiskSector = 19;
    static constexpr int kOffDiskSectorWork = 23;

    // Command A3, scan: where the keys are within each sector.
    //   for p = S + iob[37]; p <= S + iob[39]; p += iob[38] + 1 + iob[40]
    static constexpr int kOffScanFirstKey = 37;
    static constexpr int kOffScanKeyLength = 38;   // key length MINUS ONE
    static constexpr int kOffScanLastKey = 39;
    static constexpr int kOffScanKeyGap = 40;
    static constexpr int kOffScanHit = 42;         // written on a hit: the key's RIGHTMOST byte
    static constexpr int kScanEqual = 0, kScanLowOrEqual = 1, kScanHighOrEqual = 2, kScanId = 3;

    // The data buffer address.  Bit 0x800000 means the low 16 bits are a
    // task-TRANSLATED address; otherwise the value is a real address up to
    // 0x7FFFFF.  Overloaded: for command A2 with modifier C0, byte 13 alone
    // is the fill character.
    static constexpr int kOffDataBuffer = 13;
    static constexpr int kDataBufferTranslated = 0x800000;

    static int command(machine::MachineState& m, int iob) { return m.readHalf(iob + kOffCommand); }
    static void complete(machine::MachineState& m, int iob, int code);
    // The decoded block, one field per line, as the monitor prints it.
    static std::string dump(machine::MachineState& m, int iob);
};

}  // namespace sim36::devices
