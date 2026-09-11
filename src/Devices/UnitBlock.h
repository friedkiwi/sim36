// The terminal unit block ("TU") and printer unit block ("PU"): the
// per-station control blocks SSP builds and the control processor reads.
// Only shapes are recovered for most fields; meanings are not invented.
// The decode is read-only and repairs nothing.
#pragma once

#include <cstdint>
#include <cstdio>

#include "Machine/MachineState.h"

namespace sim36::devices {

class UnitBlock {
public:
    static constexpr uint16_t kEyecatcher = 0xE3E4;   // "TU"
    // Status/flags; bit 0x80 is "signed on", the one question the machine
    // asks a terminal unit block and the gate IPL phase 3 waits on.  SSP
    // answers it; the emulator only asks.
    static constexpr int kOffFlags = 7;
    static constexpr uint8_t kFlagSignedOn = 0x80;
    static constexpr int kOffClass = 0x0A;
    static constexpr int kOffUnitIndex = 41;
    static constexpr int kOffState = 39;
    static constexpr int kOffQueueHeader = 40;
    static constexpr int kOffStatus = 42;
    static constexpr int kOffBusiest = 43;
    static constexpr int kOffChain = 45;
    // Owning interactive-job task block, +0x60..+0x62; cleared when the
    // owning job terminates while the JCB at +0x63..+0x65 is retained.
    static constexpr int kOffOwningJobTask = 0x60;
    static constexpr int kOwningJobTaskRightmost = 0x62;
    static constexpr int kOffJobControlBlock = 0x63;
    static constexpr int kJobControlBlockRightmost = 0x65;
    static constexpr int kOffInputWorkspacePointer = 0x43;
    static constexpr int kOffActiveSessionPointer = 0x8F;
    static constexpr int kActiveSessionPointerRightmost = 0x91;
    static constexpr int kOffPointerEnding95 = 0x93;
    static constexpr int kPointerEnding95Rightmost = 0x95;
    // Pointer to the station-entry table, +0x95..+0x97, deliberately
    // overlapping the pointer ending at +0x95.
    static constexpr int kOffCpetTablePointer = 0x95;
    static constexpr int kCpetTablePointerRightmost = 0x97;
    static constexpr int kCpetEntrySize = 16;
    static constexpr int kCpetMaximumEntries = 20;
    static constexpr int kOffConfigured = 142;
    static constexpr int kTubMinimumSize = 143;
    static constexpr int kPubSize = 96;

    static bool isWellFormed(machine::MachineState& m, int block) { return block != 0 && m.readHalf(block) == kEyecatcher; }
    static bool isSignedOn(machine::MachineState& m, int tub) { return (m.readByte(tub + kOffFlags) & kFlagSignedOn) != 0; }

    static void dump(std::FILE* out, machine::MachineState& m, int block);
};

}  // namespace sim36::devices
