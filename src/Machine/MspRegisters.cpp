#include "Machine/MspRegisters.h"

namespace sim36::machine {

void MspRegisters::loadPsr(uint8_t value)
{
    uint8_t v = static_cast<uint8_t>(value & 0x3F);   // bits 0 and 1 do not exist
    if ((v & 0x01) != 0)            // bit 7 on -> 5 and 6 forced off
        v = static_cast<uint8_t>((v & ~0x06) | 0x01);
    else if ((v & 0x02) == 0)       // bits 6 and 7 off -> bit 5 on
        v |= 0x04;
    else                            // bit 6 on, 7 off -> bit 5 off
        v &= 0xFB;
    psr_ = v;
}

void MspRegisters::loadPsrFromWmpr(uint8_t wmpr)
{
    const uint8_t condition = (wmpr & 0x01) != 0 ? 0x01 : (wmpr & 0x04) != 0 ? 0x04 : 0x02;
    psr_ = static_cast<uint8_t>((wmpr & 0x38) | condition);
}

uint8_t MspRegisters::pmr() const
{
    uint8_t v = static_cast<uint8_t>(pmrOwnBits_ & (kPmrTaskDispatch | kPmrNotPrivileged));
    if (pactXr1Bit()) v |= kPmrPxr1;
    if (pactIarBit()) v |= kPmrPiar;
    if (pactXr2Bit()) v |= kPmrPxr2;
    if (pactPdirBit()) v |= kPmrPdir;
    return v;
}

void MspRegisters::setPmr(uint8_t value)
{
    pmrOwnBits_ = static_cast<uint8_t>(value & (kPmrTaskDispatch | kPmrNotPrivileged));
    setPactXr1Bit((value & kPmrPxr1) != 0);
    setPactIarBit((value & kPmrPiar) != 0);
    setPactXr2Bit((value & kPmrPxr2) != 0);
    setPactPdirBit((value & kPmrPdir) != 0);
}

void MspRegisters::setTranslatedAddressing(bool on)
{
    cmr = static_cast<uint8_t>(on ? (cmr | 0x01) : (cmr & ~0x01));
    pactCsp = setBit(pactCsp, on);
}

void MspRegisters::reset()
{
    iar = arr = xr1 = xr2 = 0;
    psr_ = 0x01;   // Equal: reset MSP or system reset (SY31-9035 10-650)
    pactDir = pactXr1 = pactXr2 = pactIar = pactCsp = 0;   // untranslated
    pactReg = pactAtr = 0;
    pmrOwnBits_ = cmr = 0;
    for (uint16_t& w : wr) w = 0;
}

}  // namespace sim36::machine
