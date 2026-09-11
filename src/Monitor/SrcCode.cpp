#include "Monitor/SrcCode.h"

namespace sim36::monitor {

namespace {

const char* selfTest(int code)
{
    switch (code) {
        case 0xFFFF: return "module 0: initialise the machine-check MAB to E880";
        case 0xE880: return "module 1: unconditional branching";
        case 0xE881: return "module 2: conditional branch (PCR)";
        case 0xE882: return "module 3: subroutine linkage";
        case 0xE883: return "module 4: test-mask instruction";
        case 0xE884: return "module 5: byte selection in work registers";
        case 0xE885: return "module 6: compare-immediate";
        case 0xE886: return "module 7: subtract-immediate";
        case 0xE887: return "module 8: 1-byte logical instructions";
        case 0xE888: return "module 9: 1-byte arithmetic instructions";
        case 0xE889: return "module 10: 2-byte logical instructions";
        case 0xE88A: return "module 11: 2-byte arithmetic instructions";
        case 0xE88C: return "module 12: hexadecimal branch instructions";
        case 0xE88E: return "module 13: hexadecimal move instructions";
        case 0xE88F: return "module 14: initialise low 32K of control storage";
        case 0xE890: case 0xE893: return "module 15: storage data and ECC check bits";
        case 0xE894: return "module 16: base + displacement instructions";
        case 0xE895: case 0xE896: case 0xE897:
            return "module 17: full/half-word load-store; load from a direct area";
        default: return nullptr;
    }
}

}  // namespace

std::string SrcCode::deviceClass(int code)
{
    code &= 0xFFFF;
    if (code >= 0xE880 && code <= 0xE897) return "CSP self-test progress";
    if (code == 0xFFFF) return "CSP self-test progress";
    if (code < 0x0100) return "program";
    if (code < 0x1000) return "work station";
    if (code < 0x1800) return "CSP/MSP/channel";
    if (code < 0x1900) return "communications";
    if (code < 0x1A00) return "21ED file";
    if (code < 0x1B00) return "10SR file";
    if (code < 0x1D00) return "8809 tape";
    if (code < 0x1E00) return "diskette";
    return "unclassified";
}

std::string SrcCode::describe(int code)
{
    code &= 0xFFFF;
    if (code == 0x0000) return "running normally";
    const char* st = selfTest(code);
    if (st != nullptr) return std::string("CSP self-test - ") + st;
    return deviceClass(code);
}

}  // namespace sim36::monitor
