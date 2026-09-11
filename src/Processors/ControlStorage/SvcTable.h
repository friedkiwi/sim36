// The SVC dispatch classes, the implemented set and the inline parameter
// counts.  The R-byte ranges are structural: 0x00-0x36 dispatch through one
// table, 0x40-0x4C through another, 0x50-0x53 on a third path, and the gaps
// are rejected outright.
#pragma once

#include <cstdint>

#include "Processors/ControlStorage/IControlStorageProcessor.h"

namespace sim36::processors::controlstorage {

class SvcTable {
public:
    // Refused before the switch: ten are undocumented on the System/36 and
    // two the manual documents but address hardware this model does not
    // carry (47, the magnetic character reader; 49, LAN/X.21).  An emulator
    // modelling decision, not "error 6 from the machine".
    static bool isRejected(uint8_t r);
    static bool isImplemented(uint8_t r);
    // The eight immediate calls; everything else in 0x00-0x36 is overlapped;
    // delayed is exactly the contiguous range 40-52.
    static DispatchClass classify(uint8_t r);
    // SVC 51 is delayed, but reads are regarded as immediate when the disk
    // cache is active and the data is found in it.
    static DispatchClass classifyRequest(uint8_t r, bool diskCacheHit);
    // Inline parameter counts.  SVC is 3 to 6 bytes; without this the
    // instruction stream desynchronises.
    static int inlineParameters(uint8_t r);
    static int instructionLength(uint8_t r) { return 3 + inlineParameters(r); }
};

}  // namespace sim36::processors::controlstorage
