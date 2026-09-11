// The MSP architectural register file.
//
// Deliberately not a struct of public fields for the PSR and the PMR: the PSR
// cannot be loaded as a plain byte (bits 5, 6 and 7 are mutually normalised
// by the hardware) and the PMR's addressing bits are the same physical bits
// as bit 0 of the PACT registers.  SA21-9436 chapter 1, pages 1-26 to 1-32.
#pragma once

#include <cstdint>

namespace sim36::machine {

class MspRegisters {
public:
    uint16_t iar = 0;          // instruction address register
    uint16_t arr = 0;          // address recall register
    uint16_t xr1 = 0, xr2 = 0; // index registers
    uint16_t wr[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // work registers; WR6 carries the event type on device SVCs

    // Program status register.  Read freely; load through loadPsr.
    uint8_t psr() const { return psr_; }

    // Load the PSR the way the hardware does: bits 5, 6 and 7 cannot be
    // loaded at the same time.  Bit 7 on forces 5 and 6 off; bits 6 and 7
    // off forces 5 on; bit 6 on with 7 off forces 5 off.
    void loadPsr(uint8_t value);

    // Load the PSR from the control processor's WMPR instruction: only WMPR
    // bits 5 and 7 participate.
    void loadPsrFromWmpr(uint8_t wmpr);

    // ---- PACT: one register per addressing path ------------------------
    // Bit 0 (mask 0x80) selects translation through the ATRs; otherwise the
    // low four bits are concatenated with the 2-byte register to form a
    // 20-bit address.  Reset value is 0, untranslated: PACT bit 0 IS the
    // corresponding PMR bit, and phase 1 runs in real storage.
    static constexpr uint8_t kPactTranslate = 0x80;
    static constexpr uint8_t kPactPmrBit = 0x80;

    uint8_t pactDir = 0;    // A0 direct operand addresses
    uint8_t pactXr1 = 0;    // A1
    uint8_t pactXr2 = 0;    // A2
    uint8_t pactIar = 0;    // A3 instruction fetch
    uint8_t pactReg = 0;    // A4 fast task switch, MSP registers
    uint8_t pactAtr = 0;    // A5 fast task switch, ATRs
    uint8_t pactCsp = 0;    // A7 main storage operations from the CSP

    // PMR bit assignments, SA21-9436 3-34; bit 0 is mask 0x80.
    static constexpr uint8_t kPmrTaskDispatch = 0x80;   // bit 0
    static constexpr uint8_t kPmrPxr1 = 0x10;           // bit 3
    static constexpr uint8_t kPmrPiar = 0x08;           // bit 4
    static constexpr uint8_t kPmrPxr2 = 0x04;           // bit 5
    static constexpr uint8_t kPmrPdir = 0x02;           // bit 6
    static constexpr uint8_t kPmrNotPrivileged = 0x01;  // bit 7

    // Program mode register.  Bits 3-6 are projected from the PACT bytes
    // rather than stored twice; writing it writes through.
    uint8_t pmr() const;
    void setPmr(uint8_t value);

    uint8_t cmr = 0;   // control mode register

    bool taskDispatchingEnabled() const { return (pmr() & kPmrTaskDispatch) != 0; }
    bool privileged() const { return (pmr() & kPmrNotPrivileged) == 0; }

    bool pactPdirBit() const { return (pactDir & kPactPmrBit) != 0; }
    bool pactXr1Bit() const { return (pactXr1 & kPactPmrBit) != 0; }
    bool pactXr2Bit() const { return (pactXr2 & kPactPmrBit) != 0; }
    bool pactIarBit() const { return (pactIar & kPactPmrBit) != 0; }
    void setPactPdirBit(bool on) { pactDir = setBit(pactDir, on); }
    void setPactXr1Bit(bool on) { pactXr1 = setBit(pactXr1, on); }
    void setPactXr2Bit(bool on) { pactXr2 = setBit(pactXr2, on); }
    void setPactIarBit(bool on) { pactIar = setBit(pactIar, on); }

    // CMR bit 7 and PCSP bit 0 are the same physical bit, numbered from
    // opposite ends: CMR bit 7 is mask 0x01, PCSP bit 0 is mask 0x80.
    bool translatedAddressing() const { return (cmr & 0x01) != 0; }
    void setTranslatedAddressing(bool on);

    void reset();

private:
    static uint8_t setBit(uint8_t v, bool on)
    {
        return static_cast<uint8_t>(on ? (v | kPactPmrBit) : (v & ~kPactPmrBit));
    }
    uint8_t psr_ = 0;
    uint8_t pmrOwnBits_ = 0;   // bits 0 and 7, which live only here
};

}  // namespace sim36::machine
