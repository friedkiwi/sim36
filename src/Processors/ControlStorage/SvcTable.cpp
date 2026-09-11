#include "Processors/ControlStorage/SvcTable.h"

namespace sim36::processors::controlstorage {

bool SvcTable::isRejected(uint8_t r)
{
    switch (r) {
        case 0x15: case 0x16: case 0x1C: case 0x1F: case 0x27: case 0x28: case 0x29: case 0x2A:
        case 0x47: case 0x49: case 0x4A: case 0x4B:
            return true;
        default:
            return false;
    }
}

bool SvcTable::isImplemented(uint8_t r)
{
    if (isRejected(r)) return false;
    if (r <= 0x36) return true;
    if (r >= 0x40 && r <= 0x4C) return true;
    if (r >= 0x50 && r <= 0x53) return true;   // 0x53 is undocumented but dispatched
    return false;
}

DispatchClass SvcTable::classify(uint8_t r)
{
    switch (r) {
        case 0x09: case 0x0A: case 0x0C: case 0x0D: case 0x0E: case 0x1A: case 0x1B: case 0x22:
            return DispatchClass::Immediate;
        default:
            break;
    }
    if (r >= 0x40 && r <= 0x52) return DispatchClass::Delayed;
    return DispatchClass::Overlapped;
}

DispatchClass SvcTable::classifyRequest(uint8_t r, bool diskCacheHit)
{
    if (r == 0x51 && diskCacheHit) return DispatchClass::Immediate;
    return classify(r);
}

int SvcTable::inlineParameters(uint8_t r)
{
    switch (r) {
        case 0x00: case 0x01: case 0x03: case 0x12: case 0x19: case 0x1B: case 0x22:
            return 2;
        // 0F is 3: SA21-9436 3-89 names "Inline parameter 3", its worked
        // example is the six-byte F4000F204700, and phase 1 only decodes
        // coherently with 3.  14 is the Advanced/36's private transfer,
        // whose handler reads all three saved inline bytes.
        case 0x0E: case 0x0F: case 0x10: case 0x14: case 0x1A: case 0x50: case 0x51:
            return 3;
        // 1E Task Wait (3-102) prints Byte 4; 08 (3-97) Byte 4 only; 05
        // (3-103) takes op code, Q-byte, R-byte and nothing else.
        case 0x04: case 0x08: case 0x0B: case 0x0C: case 0x0D: case 0x13: case 0x17: case 0x1D:
        case 0x1E: case 0x21: case 0x23: case 0x24: case 0x2B: case 0x4C: case 0x52:
            return 1;
        default:
            return 0;
    }
}

}  // namespace sim36::processors::controlstorage
