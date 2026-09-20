// `selftest`: IBM's own worked examples from SA21-9436 chapter 3 as vectors.
// Every one is transcribed from the printed "Example" panel: the instruction
// bytes, the "before" operands, the "after" operands and, where IBM printed
// one, the program status byte.  Nothing here is inferred.
#include <fmt/format.h>

#include "Monitor/MonitorCli.h"

namespace sim36::monitor {

namespace {

using machine::MachineState;

void pokeBytes(MachineState& m, int addr, std::initializer_list<int> bytes)
{
    int i = 0;
    for (int b : bytes) m.writeByte(addr + i++, static_cast<uint8_t>(b));
}

bool same(MachineState& m, int addr, std::initializer_list<int> want)
{
    int i = 0;
    for (int b : want)
        if (m.readByte(addr + i++) != static_cast<uint8_t>(b)) return false;
    return true;
}

struct Score { int pass = 0, fail = 0; };

void report(const std::string& what, bool ok, Score& s)
{
    fmt::print("  {:<42} {}\n", what, ok ? "PASS" : "FAIL");
    if (ok) ++s.pass; else ++s.fail;
}

void reportBytes(const std::string& what, MachineState& m, int addr, std::initializer_list<int> want, Score& s)
{
    const bool ok = same(m, addr, want);
    report(what, ok, s);
    if (ok) return;
    fmt::print("     got  ");
    for (std::size_t i = 0; i < want.size(); ++i) fmt::print("{:02x} ", m.readByte(addr + static_cast<int>(i)));
    fmt::print("\n     want ");
    for (int b : want) fmt::print("{:02x} ", b);
    fmt::print("\n");
}

void reportPsr(const std::string& what, MachineState& m, uint8_t want, Score& s)
{
    const bool ok = m.msp.psr() == want;
    report(what, ok, s);
    if (!ok) fmt::print("     got  {:02x}  want {:02x}\n", m.msp.psr(), want);
}

}  // namespace

SelfTestResult runSelfTest(machine::MachineState& m, processors::MainStorageProcessor& msp)
{
    Score sc;
    // Point the IAR at an instruction and execute exactly one.
    auto runAt = [&](int addr) {
        m.msp.iar = static_cast<uint16_t>(addr);
        msp.start();
        if (!msp.step()) fmt::print("     processor stopped: {}\n", msp.stopReason());
    };

    // Edit, SA21-9436 3-25.  Instruction 0A 0A 00BF 0007.  Operand 1 is 11
    // bytes ending at 00BF; operand 2 is the zoned field 010809 with a D
    // sign, ending at 0007.
    pokeBytes(m, 0x00B5, {0x5C, 0x20, 0x6B, 0x20, 0x20, 0x20, 0x4B, 0x20, 0x20, 0x40, 0x5C});
    pokeBytes(m, 0x0002, {0xF0, 0xF1, 0xF0, 0xF8, 0xF0, 0xD9});
    pokeBytes(m, 0x1000, {0x0A, 0x0A, 0x00, 0xBF, 0x00, 0x07});
    m.msp.iar = 0x1000;
    msp.start();
    msp.step();
    reportBytes("ED  (manual 3-25)", m, 0x00B5, {0x5C, 0xF0, 0x6B, 0xF1, 0xF0, 0xF8, 0x4B, 0xF0, 0xF9, 0x40, 0x5C}, sc);
    // The manual prints the resulting status byte as 00000010: Low, i.e.
    // operand 2 negative.
    report("ED  status byte = Low (operand 2 negative)", (m.msp.psr() & 0x02) != 0, sc);

    // Insert and Test Characters, SA21-9436 3-27.  Instruction 0B 09 00B6
    // 0010.  Operand 1 is 10 bytes addressed by its LEFTMOST byte at 00B6.
    pokeBytes(m, 0x00B6, {0xF0, 0x6B, 0xF1, 0xF0, 0xF8, 0x4B, 0xF0, 0xF9, 0x40, 0x5C});
    pokeBytes(m, 0x0010, {0x5C});                       // the fill character '*'
    pokeBytes(m, 0x1000, {0x0B, 0x09, 0x00, 0xB6, 0x00, 0x10});
    m.msp.iar = 0x1000;
    msp.start();
    msp.step();
    report("ITC (manual 3-27)", same(m, 0x00B6, {0x5C, 0x5C, 0xF1, 0xF0, 0xF8, 0x4B, 0xF0, 0xF9, 0x40, 0x5C}), sc);
    report("ITC address recall register = 00B8", m.msp.arr == 0x00B8, sc);

    // Add Logical Characters, 3-6.  Instruction 5E 03 00 10, both operands
    // indexed off XR1 = 0CC0.
    m.msp.xr1 = 0x0CC0;
    pokeBytes(m, 0x0CBD, {0x35, 0xCB, 0xED, 0x64});
    pokeBytes(m, 0x0CCD, {0x5B, 0x55, 0x78, 0xCD});
    pokeBytes(m, 0x1000, {0x5E, 0x03, 0x00, 0x10});
    m.msp.loadPsr(0x01);                                // printed before: 00000001
    runAt(0x1000);
    reportBytes("ALC (manual 3-6)", m, 0x0CBD, {0x91, 0x21, 0x66, 0x31}, sc);
    reportPsr("ALC status byte = 00000010 (Low)", m, 0x02, sc);

    // Add Logical Immediate, 3-8.  ALI X'0021',X'0B' assembles to 3F F5 00 21.
    pokeBytes(m, 0x0021, {0x00});
    pokeBytes(m, 0x1000, {0x3F, 0xF5, 0x00, 0x21});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("ALI (manual 3-8)", m, 0x0021, {0x0B}, sc);
    reportPsr("ALI status byte = 00000010 (Low)", m, 0x02, sc);

    // Add to Register, 3-12.  Instruction 36 02 00 04; Q of 02 selects XR2.
    pokeBytes(m, 0x0003, {0x48, 0x20});
    pokeBytes(m, 0x1000, {0x36, 0x02, 0x00, 0x04});
    m.msp.xr2 = 0x356A;
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    report("A   (manual 3-12) XR2 = 7D8A", m.msp.xr2 == 0x7D8A, sc);
    reportPsr("A   status byte = 00000010 (Low)", m, 0x02, sc);

    // Branch On Condition, 3-18.  Instruction C0 88 02 BF at 0BCC, so the
    // ARR is 0BD0.  Q-byte 10001000 tests decimal overflow, on in 00011001.
    pokeBytes(m, 0x0BCC, {0xC0, 0x88, 0x02, 0xBF});
    m.msp.loadPsr(0x19);                                // printed before: 00011001
    runAt(0x0BCC);
    report("BC  (manual 3-18) IAR = 02BF", m.msp.iar == 0x02BF, sc);
    report("BC  address recall register = 0BD0", m.msp.arr == 0x0BD0, sc);
    reportPsr("BC  status byte = 00010001", m, 0x11, sc);

    // PACT 1-28: every untranslated prefix bit is concatenated with the
    // 16-bit register.  Advanced/36 queue space routinely crosses 1 MB, so
    // prefix 10 must not alias prefix 00.
    int pactReal = 0;
    report("PACT 10:DB50 resolves above 1 MB",
           m.resolve(0xDB50, 0x10, MachineState::kAtrTaskGroup0, false, pactReal) && pactReal == 0x10DB50, sc);

    // A BC target is a logical control-flow address, never a storage access:
    // condition false, XR1 zero, page 0 protected must not raise level 5.
    const uint16_t oldAtr0 = m.atr[MachineState::kAtrTaskGroup0];
    const uint8_t oldPactXr1 = m.msp.pactXr1;
    pokeBytes(m, 0x1000, {0xD0, 0x01, 0x00});
    m.msp.xr1 = 0;
    m.msp.pactXr1 = 0x80;
    m.atr[MachineState::kAtrTaskGroup0] = MachineState::kAtrProtect;
    m.msp.loadPsr(0x01);                                // q=01 condition is false
    runAt(0x1000);
    report("BC  untaken indexed target does not access protected page 0", !msp.stopped() && m.msp.iar == 0x1003, sc);
    m.msp.pactXr1 = oldPactXr1;
    m.atr[MachineState::kAtrTaskGroup0] = oldAtr0;

    // Compare Logical Characters, 3-20.  Instruction 0D 02 00 12 00 02.
    pokeBytes(m, 0x0010, {0x27, 0xFA, 0x26});
    pokeBytes(m, 0x0000, {0x23, 0xFA, 0x26});
    pokeBytes(m, 0x1000, {0x0D, 0x02, 0x00, 0x12, 0x00, 0x02});
    m.msp.loadPsr(0x21);                                // printed before: 00100001
    runAt(0x1000);
    report("CLC (manual 3-20) operands unchanged",
           same(m, 0x0010, {0x27, 0xFA, 0x26}) && same(m, 0x0000, {0x23, 0xFA, 0x26}), sc);
    reportPsr("CLC status byte = 00100100 (High)", m, 0x24, sc);

    // Compare Logical Immediate, 3-22.  Instruction 3D 7F 00 21; 75 < 7F, so Low.
    pokeBytes(m, 0x0021, {0x75});
    pokeBytes(m, 0x1000, {0x3D, 0x7F, 0x00, 0x21});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("CLI (manual 3-22)", m, 0x0021, {0x75}, sc);
    reportPsr("CLI status byte = 00000010 (Low)", m, 0x02, sc);

    // Jump On Condition, 3-30.  Instruction F2 30 0F at 0BBD: the IAR is 0BC0
    // past the jump and 0BC0 + 0F = 0BCF.  Condition false on bits both off.
    pokeBytes(m, 0x0BBD, {0xF2, 0x30, 0x0F});
    m.msp.loadPsr(0x09);                                // printed before: 00001001
    runAt(0x0BBD);
    report("JC  (manual 3-30) IAR = 0BCF", m.msp.iar == 0x0BCF, sc);
    reportPsr("JC  status byte = 00001001 (unchanged)", m, 0x09, sc);

    // Load Address, 3-33.  Instruction D2 02 05: byte 3 added to XR1 goes to XR2.
    pokeBytes(m, 0x1000, {0xD2, 0x02, 0x05});
    m.msp.xr1 = 0x2A15;
    m.msp.xr2 = 0x0000;
    runAt(0x1000);
    report("LA  (manual 3-33) XR2 = 2A1A", m.msp.xr2 == 0x2A1A, sc);

    // Load Register, 3-37.  Instruction 35 02 00 11; the field is 0010-0011.
    pokeBytes(m, 0x0010, {0x00, 0x02});
    pokeBytes(m, 0x1000, {0x35, 0x02, 0x00, 0x11});
    m.msp.xr2 = 0x0C31;
    runAt(0x1000);
    report("L   (manual 3-37) XR2 = 0002", m.msp.xr2 == 0x0002, sc);

    // Move Characters, 3-39.  Instruction 0C 05 1A 06 2B 5A: six bytes each.
    pokeBytes(m, 0x1A01, {0xD1, 0xC1, 0xD4, 0xC5, 0xE2, 0x40});
    pokeBytes(m, 0x2B55, {0xD9, 0xD6, 0xC2, 0xC5, 0xD9, 0xE3});
    pokeBytes(m, 0x1000, {0x0C, 0x05, 0x1A, 0x06, 0x2B, 0x5A});
    runAt(0x1000);
    reportBytes("MVC (manual 3-39)", m, 0x1A01, {0xD9, 0xD6, 0xC2, 0xC5, 0xD9, 0xE3}, sc);

    // A translated field is a logical byte range, not one resolved host
    // pointer: put each of two logical pages behind a non-adjacent real frame.
    const uint8_t oldPactDir = m.msp.pactDir;
    const uint8_t oldPactIar = m.msp.pactIar;
    uint16_t oldRangeAtr[4];
    for (int i = 0; i < 4; ++i) oldRangeAtr[i] = m.atr[MachineState::kAtrTaskGroup0 + 4 + i];
    m.msp.pactDir = 0x80;
    m.msp.pactIar = 0x00;                               // keep the test instruction itself real
    m.atr[MachineState::kAtrTaskGroup0 + 4] = 0x0030;
    m.atr[MachineState::kAtrTaskGroup0 + 5] = 0x0050;
    m.atr[MachineState::kAtrTaskGroup0 + 6] = 0x0060;
    m.atr[MachineState::kAtrTaskGroup0 + 7] = 0x0070;
    pokeBytes(m, 0x0307FE, {0xA1, 0xA2});               // logical 37FE..37FF, frame 60
    pokeBytes(m, 0x038000, {0xA3, 0xA4});               // logical 3800..3801, frame 70
    pokeBytes(m, 0x0187FE, {0x00, 0x00});               // logical 27FE..27FF, frame 30
    pokeBytes(m, 0x028000, {0x00, 0x00});               // logical 2800..2801, frame 50
    pokeBytes(m, 0x027FFE, {0xDE, 0xAD});               // physically adjacent, logically unrelated
    pokeBytes(m, 0x1000, {0x0C, 0x03, 0x28, 0x01, 0x38, 0x01});
    runAt(0x1000);
    report("MVC translates every byte across a noncontiguous ATR boundary",
           same(m, 0x0187FE, {0xA1, 0xA2}) && same(m, 0x028000, {0xA3, 0xA4}) && same(m, 0x027FFE, {0xDE, 0xAD}), sc);

    // The same contract is shared by device bulk transfers; exercise the
    // range primitive with the first byte at the end of one logical page.
    pokeBytes(m, 0x0187FF, {0x00});
    pokeBytes(m, 0x028000, {0x00, 0x00, 0x00});
    pokeBytes(m, 0x018800, {0xDE, 0xAD});
    const uint8_t bulk[4] = {0x11, 0x22, 0x33, 0x44};
    m.writeGuest24Range(0x8027FF, bulk, 4);
    report("translated bulk transfer follows noncontiguous ATR extents",
           same(m, 0x0187FF, {0x11}) && same(m, 0x028000, {0x22, 0x33, 0x44}) && same(m, 0x018800, {0xDE, 0xAD}), sc);
    m.msp.pactDir = oldPactDir;
    m.msp.pactIar = oldPactIar;
    for (int i = 0; i < 4; ++i) m.atr[MachineState::kAtrTaskGroup0 + 4 + i] = oldRangeAtr[i];

    // Move Hexadecimal Character, 3-41.  Instruction 98 01 A0 65: the operand
    // 2 NUMERIC half into the operand 1 ZONE half, 2F with the 4 of 4C is CF.
    m.msp.xr1 = 0x2B15;
    m.msp.xr2 = 0x1F20;
    pokeBytes(m, 0x1FC0, {0x2F});
    pokeBytes(m, 0x2B7A, {0x4C});
    pokeBytes(m, 0x1000, {0x98, 0x01, 0xA0, 0x65});
    runAt(0x1000);
    reportBytes("MVX (manual 3-41)", m, 0x1FC0, {0xCF}, sc);
    reportBytes("MVX operand 2 unchanged", m, 0x2B7A, {0x4C}, sc);

    // Move Logical Immediate, 3-42.  Instruction 3C AF 2F CB.
    pokeBytes(m, 0x2FCB, {0x00});
    pokeBytes(m, 0x1000, {0x3C, 0xAF, 0x2F, 0xCB});
    runAt(0x1000);
    reportBytes("MVI (manual 3-42)", m, 0x2FCB, {0xAF}, sc);

    // Set Bits Off Masked, 3-43.  Instruction 3B 10000001 00 30.
    pokeBytes(m, 0x0030, {0x79});
    pokeBytes(m, 0x1000, {0x3B, 0x81, 0x00, 0x30});
    runAt(0x1000);
    reportBytes("SBF (manual 3-43)", m, 0x0030, {0x78}, sc);

    // Set Bits On Masked, 3-44.  Instruction 3A 01011010 00 20.
    pokeBytes(m, 0x0020, {0x0C});
    pokeBytes(m, 0x1000, {0x3A, 0x5A, 0x00, 0x20});
    runAt(0x1000);
    reportBytes("SBN (manual 3-44)", m, 0x0020, {0x5E}, sc);

    // Shift Right Character, 3-46.  Instruction BE 31 00, XR2 = 1FC0: four
    // bits over a 2-byte field ending at 1FC0.
    m.msp.xr2 = 0x1FC0;
    pokeBytes(m, 0x1FBF, {0x1E, 0x2F});
    pokeBytes(m, 0x1000, {0xBE, 0x31, 0x00});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("SRC (manual 3-46)", m, 0x1FBF, {0x01, 0xE2}, sc);
    reportPsr("SRC status byte = 00100010", m, 0x22, sc);

    // Store Register, 3-48.  Instruction 34 00001000 32 BB; Q 08 is the ARR.
    m.msp.arr = 0x0ACD;
    pokeBytes(m, 0x32BA, {0x2F, 0xC2});
    pokeBytes(m, 0x1000, {0x34, 0x08, 0x32, 0xBB});
    runAt(0x1000);
    reportBytes("ST  (manual 3-48)", m, 0x32BA, {0x0A, 0xCD}, sc);

    // Subtract from Register, 3-50.  Instruction 37 02 00 04.
    pokeBytes(m, 0x0003, {0x48, 0x20});
    pokeBytes(m, 0x1000, {0x37, 0x02, 0x00, 0x04});
    m.msp.xr2 = 0x756A;
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    report("S   (manual 3-50) XR2 = 2D4A", m.msp.xr2 == 0x2D4A, sc);
    reportPsr("S   status byte = 00000100 (High)", m, 0x04, sc);

    // Subtract Logical Characters, 3-53.  Instruction AF 03 00 10.  The page
    // says XR1 = 0CC0 while AF takes both off XR2; both are loaded.
    m.msp.xr1 = 0x0CC0;
    m.msp.xr2 = 0x0CC0;
    pokeBytes(m, 0x0CBD, {0x96, 0x5A, 0x77, 0xBF});
    pokeBytes(m, 0x0CCD, {0x74, 0x86, 0x62, 0xA4});
    pokeBytes(m, 0x1000, {0xAF, 0x03, 0x00, 0x10});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("SLC (manual 3-53)", m, 0x0CBD, {0x21, 0xD4, 0x15, 0x1B}, sc);
    reportPsr("SLC status byte = 00000100 (High)", m, 0x04, sc);

    // Subtract Logical Immediate, 3-55.  Instruction 3F F5 00 21: 75 minus F5.
    pokeBytes(m, 0x0021, {0x75});
    pokeBytes(m, 0x1000, {0x3F, 0xF5, 0x00, 0x21});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("SLI (manual 3-55)", m, 0x0021, {0x80}, sc);
    reportPsr("SLI status byte = 00000010 (Low)", m, 0x02, sc);

    // Subtract Zoned Decimal, 3-58.  Instruction 07 22 00 10 00 20: 76369 -
    // 425 = 75944, positive so the sign zone is F.
    pokeBytes(m, 0x000C, {0xF7, 0xF6, 0xF3, 0xF6, 0xF9});
    pokeBytes(m, 0x001E, {0xF4, 0xF2, 0xF5});
    pokeBytes(m, 0x1000, {0x07, 0x22, 0x00, 0x10, 0x00, 0x20});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("SZ  (manual 3-58)", m, 0x000C, {0xF7, 0xF5, 0xF9, 0xF4, 0xF4}, sc);
    reportBytes("SZ  operand 2 unchanged", m, 0x001E, {0xF4, 0xF2, 0xF5}, sc);
    reportPsr("SZ  status byte = 00000100 (High)", m, 0x04, sc);

    // Test Bits Off Masked, 3-60.  Instruction 39 01101100 00 25: a tested
    // bit is on, so test false goes on.
    pokeBytes(m, 0x0025, {0x94});
    pokeBytes(m, 0x1000, {0x39, 0x6C, 0x00, 0x25});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("TBF (manual 3-60) operand unchanged", m, 0x0025, {0x94}, sc);
    report("TBF test false on", (m.msp.psr() & 0x10) != 0, sc);

    // Test Bits On Masked, 3-62.  Instruction 38 00010110 00 21: a tested
    // bit is off, so test false goes on.
    pokeBytes(m, 0x0021, {0x95});
    pokeBytes(m, 0x1000, {0x38, 0x16, 0x00, 0x21});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("TBN (manual 3-62) operand unchanged", m, 0x0021, {0x95}, sc);
    report("TBN test false on", (m.msp.psr() & 0x10) != 0, sc);

    // Test-false is sticky across subsequent successful bit tests; a JC
    // which selects it both consumes the result and clears the indicator.
    pokeBytes(m, 0x0021, {0x01});
    pokeBytes(m, 0x1000, {0x38, 0x02, 0x00, 0x21});     // TBN 02: false
    pokeBytes(m, 0x1004, {0x39, 0x02, 0x00, 0x21});     // TBF 02: true
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    runAt(0x1004);
    report("TBN/TBF test false is sticky", (m.msp.psr() & 0x10) != 0, sc);
    pokeBytes(m, 0x1008, {0xF2, 0x90, 0x04});           // JC +4 if TF
    runAt(0x1008);
    report("JC consumes and clears test false", m.msp.iar == 0x100F && (m.msp.psr() & 0x10) == 0, sc);

    // Zero and Add Zoned, 3-66.  Instruction 04 22 00 10 00 20: the extra
    // positions take EBCDIC zeros and the rightmost byte keeps an F zone.
    pokeBytes(m, 0x000C, {0xF7, 0xF6, 0xF3, 0xF6, 0xF9});
    pokeBytes(m, 0x001E, {0xF4, 0xF2, 0xF5});
    pokeBytes(m, 0x1000, {0x04, 0x22, 0x00, 0x10, 0x00, 0x20});
    m.msp.loadPsr(0x01);
    runAt(0x1000);
    reportBytes("ZAZ (manual 3-66)", m, 0x000C, {0xF0, 0xF0, 0xF4, 0xF2, 0xF5}, sc);
    reportPsr("ZAZ status byte = 00000100 (High)", m, 0x04, sc);

    // The Advanced/36's private SVC 14 consumes three inline bytes; a length
    // of three would desynchronise the caller at the descriptor bytes.
    pokeBytes(m, 0x1000, {0xF4, 0x00, 0x14, 0x12, 0x34, 0x02});
    const processors::Instruction axfer = msp.decode(0x1000);
    report("SVC 14 nuaxfer is six bytes",
           axfer.length == 6 && axfer.raw.size() == 6 && axfer.raw[3] == 0x12 && axfer.raw[4] == 0x34 && axfer.raw[5] == 0x02, sc);

    fmt::print("{} passed, {} failed\n", sc.pass, sc.fail);
    SelfTestResult result;
    result.passed = sc.pass;
    result.failed = sc.fail;
    return result;
}

void MonitorCli::selfTest() { runSelfTest(m_.state, m_.msp()); }

}  // namespace sim36::monitor
