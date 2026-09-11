// The work station I/O block: the fields the control processor's device
// path reads before it hands a request to a device.  The controller itself
// arrives with milestone 6; these are the offsets the dispatcher needs.
#pragma once

#include <cstdint>

#include "Devices/IoBlock.h"
#include "Machine/MachineState.h"

namespace sim36::devices {

class WorkStationIob {
public:
    static constexpr uint16_t kUnitBlockEyecatcher = 0xE3E4;   // "TU"
    // The class byte: high nibble C is a work station, E the alternate.
    static constexpr int kOffClass = 0x0A;
    static constexpr int kClassMask = 0xF0;
    static constexpr int kClassWorkStation = 0xC0;
    static constexpr int kClassAlternate = 0xE0;
    static constexpr int kOffCommand = 0x0B;
    static constexpr int kOffUnitAddress = 0x0C;
    static constexpr int kOffDataBuffer = IoBlock::kOffDataBuffer;
    static constexpr int kOffLength = 0x10;
    static constexpr int kOffUnitIndex = 0x13;
    static constexpr int kOffUnitBlock = 0x15;
    static constexpr int kOffConfigureError = 0x18;

    static constexpr int kCmdConfigureNewWorkStations = 0x81;
    static constexpr int kCmdReadCurrentConfiguration = 0x82;
    static constexpr int kCmdPut = 0x27;
    static constexpr int kCmdPutWithInvite = 0xA7;
    static constexpr int kCmdClearPrinter = 0x40;
    static constexpr int kCmdGetPrinterStatus = 0x47;
    static constexpr int kCmdCancelInvite = 0xC3;
    static constexpr int kCmdInvite = 0xFF;

    static int command(machine::MachineState& m, int iob) { return m.readByte(iob + kOffCommand); }
};

}  // namespace sim36::devices
