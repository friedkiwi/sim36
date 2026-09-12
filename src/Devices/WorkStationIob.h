// The work station / printer input-output block, as SVC 42 and SVC 43 hand
// it over in XR1.
//
// Accessors over LIVE guest storage, never a marshalled struct: SSP writes
// these fields itself between supervisor calls, and the controller writes
// some of them back.
//
// The command is at +0x0B, NOT +0x0A.  That is the opposite of the fixed
// disk, and it is settled from the decoded controller dispatch, which reads
// byte 11 against 0x82, 0x81, 0x40 and 0x27: three of the six command names
// SA21-9436 chapter 11 gives, so the manual's command set and this byte are
// the same field.  +0x0A is a different thing entirely, the class byte.
#pragma once

#include <cstdint>
#include <string>

#include "Devices/IoBlock.h"
#include "Machine/MachineState.h"

namespace sim36::devices {

class WorkStationIob {
public:
    // Bytes 0-6 are the event control mask, as they are for every IOB the
    // machine defines.

    // EBCDIC "TU" as a halfword at +0.  When XR1's block carries it, XR1
    // points at a unit block, not at an IOB: three controller routines test
    // it and take the block itself as the unit block; without it they follow
    // kOffUnitBlock.  A fourth eyecatcher alongside "AC", "TB" and "QH".
    static constexpr uint16_t kUnitBlockEyecatcher = 0xE3E4;

    // Not the command: a class/state byte whose HIGH NIBBLE is what the code
    // tests.  The print request path requires 0xCn; the element-queueing
    // path also accepts 0xEn; the output-open path finds 0xC0 on entry and
    // STORES 0xC1 back, so the low nibble is a phase the controller
    // advances.  The nibble C is the work station class the way A is the
    // disk's.
    static constexpr int kOffClass = 0x0A;
    static constexpr int kClassMask = 0xF0;
    static constexpr int kClassWorkStation = 0xC0;
    // Accepted on the element-queueing path but not the print request path;
    // which path uses it is not established.
    static constexpr int kClassAlternate = 0xE0;

    // THE COMMAND.
    static constexpr int kOffCommand = 0x0B;

    // The unit address.  Both device SVC arms read it before anything else
    // and use it to find the controller and the station.
    static constexpr int kOffUnitAddress = 0x0C;

    // The data buffer, +0x0D..0x0F, resolved by the device-path address rule
    // for every device SVC.  Shared with the disk.
    static constexpr int kOffDataBuffer = IoBlock::kOffDataBuffer;

    // A halfword length: the two configuration commands both divide
    // (it - 1) by 6 to get a count of six-byte configuration records, so it
    // is the byte length of the buffer at +0x0D, plus one for the 0xFF
    // terminator.
    static constexpr int kOffLength = 0x10;

    // One byte read as the low half of the halfword at +0x12, only when the
    // block is an IOB rather than a "TU" unit block (the unit block's
    // equivalent is its +41).  Its meaning is not established.
    static constexpr int kOffUnitIndex = 0x13;

    // The unit block address, three bytes, resolved when +0 is not "TU".
    // SA21-9436 5-50's "the PUB address must be set".
    static constexpr int kOffUnitBlock = 0x15;

    // Where the configuration error routine puts its reason code when
    // Configure New Work Stations is rejected.
    static constexpr int kOffConfigureError = 0x18;

    // The two controller commands, from the decoded controller dispatch.
    // Every DEVICE command is in WorkStationActions.
    static constexpr int kCmdConfigureNewWorkStations = 0x81;
    static constexpr int kCmdReadCurrentConfiguration = 0x82;

    // 0x27 put: the decoded code for what SA21-9436 chapter 11 calls Output
    // Data.  The manual's own code for it is 21, which appears nowhere in
    // the decoded dispatch, so the two are recorded side by side and neither
    // is mapped onto the other.
    static constexpr int kCmdPut = 0x27;
    // 0xA7 putWithInvite: 0x27 with bit 0x80, the bit the output-open path
    // masks off before comparing.
    static constexpr int kCmdPutWithInvite = 0xA7;
    // 0x40 clear, and SA21-9436's Clear Printer under the same code and the
    // same name.
    static constexpr int kCmdClearPrinter = 0x40;
    // 0x47 getPrinterStatus.  The manual's code for the same operation is 41.
    static constexpr int kCmdGetPrinterStatus = 0x47;
    // 0xC3 cancelInvite.
    static constexpr int kCmdCancelInvite = 0xC3;

    // SA21-9436 chapter 11's Invite code, and it is NOT a command byte: the
    // controller routes on the UNIT ADDRESS, and FF there means all stations
    // and goes to the invite routine before the command byte is read at all.
    static constexpr int kCmdInvite = 0xFF;

    static int command(machine::MachineState& m, int iob) { return m.readByte(iob + kOffCommand); }
    static int classByte(machine::MachineState& m, int iob) { return m.readByte(iob + kOffClass); }
    static int unitAddress(machine::MachineState& m, int iob) { return m.readByte(iob + kOffUnitAddress); }
    static int length(machine::MachineState& m, int iob) { return m.readHalf(iob + kOffLength); }

    static bool isWorkStationClass(machine::MachineState& m, int iob)
    {
        return (classByte(m, iob) & kClassMask) == kClassWorkStation;
    }

    // True when XR1 pointed at a unit block rather than an IOB.  Three
    // routines branch on exactly this test, so an implementation that assumes
    // XR1 is always an IOB reads the wrong bytes for the other half of the
    // calls.
    static bool isUnitBlock(machine::MachineState& m, int block) { return m.readHalf(block) == kUnitBlockEyecatcher; }

    // Resolve the unit block the way the three routines do: the block itself
    // if it carries the "TU" eyecatcher, otherwise the 24-bit address at
    // +0x15.  Zero when neither yields one.
    static int resolveUnitBlock(machine::MachineState& m, int block)
    {
        if (isUnitBlock(m, block)) return block;
        return m.readAddr24(block + kOffUnitBlock);
    }

    static std::string commandName(int cmd);
};

}  // namespace sim36::devices
