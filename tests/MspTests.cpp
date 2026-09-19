// The MSP over a bare machine state with a control storage processor that
// refuses everything: the manual's worked examples, breakpoints, patches and
// the refusal path, none of which need a volume.
#include <doctest/doctest.h>

#include "Machine/MachineState.h"
#include "Monitor/MonitorCli.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"
#include "Processors/InstructionSet.h"
#include "Processors/MainStorageProcessor.h"

using namespace sim36;
using namespace sim36::processors;

namespace {

class RefusingCsp : public controlstorage::IControlStorageProcessor {
public:
    explicit RefusingCsp(machine::MachineState& m, monitor::Tracer& t) : msp_(m, *this, t) {}
    std::string modelName() const override { return "test"; }
    MainStorageProcessor& mainStorage() override { return msp_; }
    void bringUpControlProcessor() override {}
    void iplMainProcessor() override {}
    void controlStorageTerminate() override {}
    controlstorage::DispatchClass classify(uint8_t) const override { return controlstorage::DispatchClass::Immediate; }
    bool isImplemented(uint8_t) const override { return false; }
    bool svc(controlstorage::SvcRequest& req) override { lastR = req.r; return false; }
    std::string lastRefusal() const override { return "refused by the test"; }
    bool raiseStorageProtection(uint16_t, bool) override { return false; }
    bool consumeInvalidOpcodeCheck(uint16_t resumeIar) override
    {
        lastInvalidResume = resumeIar;
        bool v = invalidOpcodeCheck;
        invalidOpcodeCheck = false;
        return v;
    }
    bool extendedControlStore(uint8_t q, uint8_t r, uint16_t sourceIar) override
    {
        lastXferQ = q;
        lastXferR = r;
        lastXferIar = sourceIar;
        return acceptXfer;
    }
    controlstorage::ITransientArea& transients() override { return transients_; }
    int lastR = -1;
    bool invalidOpcodeCheck = true;
    uint16_t lastInvalidResume = 0;
    bool acceptXfer = true;
    int lastXferQ = -1, lastXferR = -1;
    uint16_t lastXferIar = 0;

private:
    class T : public controlstorage::ITransientArea {
    public:
        bool busy() const override { return false; }
        int queueDepth() const override { return 0; }
        void schedule(uint8_t, uint8_t, uint8_t, int, int, int, int) override {}
        void setNotBusy() override {}
    } transients_;
    MainStorageProcessor msp_;
};

}  // namespace

TEST_CASE("msp: every manual vector passes on a bare machine")
{
    machine::MachineState m(1024 * 1024, 16 * 1024 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    monitor::SelfTestResult r = monitor::runSelfTest(m, csp.mainStorage());
    CHECK(r.passed == 51);
    CHECK(r.failed == 0);
}

TEST_CASE("msp: decode lengths and text")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    const uint8_t bytes[] = {0xF4, 0x00, 0x0F, 0x31, 0x35, 0x00, 0xC2, 0x10, 0x17, 0x69, 0xF1, 0x81, 0x07, 0x5E, 0x03, 0x00, 0x10};
    m.write(0x1000, bytes, sizeof bytes);
    Instruction svc = csp.mainStorage().decode(0x1000);
    CHECK(svc.length == 6);
    CHECK(svc.toString() == "SVC    q=00 0F(ARR)");
    Instruction la = csp.mainStorage().decode(0x1006);
    CHECK(la.length == 4);
    CHECK(la.toString() == "LA     q=10 $1769");
    Instruction jc = csp.mainStorage().decode(0x100A);
    CHECK(jc.toString() == "JC     q=81 07(ARR)");
    CHECK(jc.opcode == InstructionSet::kJumpBackwardOpcode);
    Instruction alc = csp.mainStorage().decode(0x100D);
    CHECK(alc.toString() == "ALC    q=03 00(XR1), 10(XR1)");
    // Opcode 00 is an unassigned two-address form: six bytes of .byte.
    Instruction bad = csp.mainStorage().decode(0x1011);
    CHECK_FALSE(bad.valid());
    CHECK(bad.length == 6);
    CHECK(bad.toString() == ".byte   0x00 0x00 0x00 0x00 0x00 0x00");
    CHECK(InstructionSet::length(0xFC, 0x1E) == 4);   // the 5.1 SVC form, one inline byte
}

TEST_CASE("msp: a refused SVC stops the processor and names the reason")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    const uint8_t bytes[] = {0xF4, 0x00, 0x0F, 0x31, 0x35, 0x00};
    m.write(0x1000, bytes, sizeof bytes);
    m.msp.iar = 0x1000;
    MainStorageProcessor& msp = csp.mainStorage();
    CHECK_FALSE(msp.step());
    CHECK(msp.stopped());
    CHECK(csp.lastR == 0x0F);
    CHECK(m.msp.iar == 0x1006);   // control returns after the last byte
    CHECK(msp.instructionsExecuted() == 1);
    CHECK(msp.stopReason().find("SVC 0F at 1000 refused by the test control storage processor") == 0);
    CHECK(msp.stopReason().find("refused by the test\n") == std::string::npos);
    CHECK(msp.stopReason().find(":\n  refused by the test") != std::string::npos);
}

TEST_CASE("msp: ZAZ preserves digit nibbles and ITC requires an F-zone digit")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();

    // Q=02 gives two three-byte fields.  V4R4 emmsp copies the low nibbles
    // verbatim; A and C must not be accumulated as decimal 10 and 12.
    const uint8_t code[] = {
        0x04, 0x02, 0x20, 0x12, 0x20, 0x22,
        0x0B, 0x02, 0x20, 0x30, 0x20, 0x40,
    };
    const uint8_t zoned[] = {0xF1, 0xFA, 0xDC};
    const uint8_t edit[] = {0xC1, 0xF0, 0xF2};
    m.write(0x1000, code, sizeof code);
    m.write(0x2020, zoned, sizeof zoned);
    m.write(0x2030, edit, sizeof edit);
    m.writeByte(0x2040, 0x40);
    m.msp.iar = 0x1000;

    REQUIRE(msp.step());
    CHECK(m.readByte(0x2010) == 0xF1);
    CHECK(m.readByte(0x2011) == 0xFA);
    CHECK(m.readByte(0x2012) == 0xDC);
    CHECK(m.msp.psr() == 0x02);       // negative, therefore Low

    REQUIRE(msp.step());
    CHECK(m.readByte(0x2030) == 0x40); // C1 is not a zoned decimal digit
    CHECK(m.readByte(0x2031) == 0x40); // F0 is not significant
    CHECK(m.readByte(0x2032) == 0xF2);
    CHECK(m.msp.arr == 0x2032);
}

TEST_CASE("msp: Advanced/36 invalid-opcode escape obeys the request flag")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();
    const uint8_t bytes[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x3C, 0x5A, 0x20, 0x00,
    };
    m.write(0x1000, bytes, sizeof bytes);
    m.msp.iar = 0x1000;

    csp.invalidOpcodeCheck = false;
    CHECK(msp.step());
    CHECK_FALSE(msp.stopped());
    CHECK(m.msp.iar == 0x1006);       // high-nibble operand decoding precedes the SLIC escape
    CHECK(msp.atPreemptionPoint());
    CHECK(msp.instructionsExecuted() == 1);
    CHECK(msp.step());
    CHECK(m.readByte(0x2000) == 0x5A);

    msp.start();
    m.msp.iar = 0x1000;
    csp.invalidOpcodeCheck = true;
    CHECK_FALSE(msp.step());
    CHECK(msp.stopped());
    CHECK(m.msp.iar == 0x1006);       // emmsp saves the decoded next IAR even on error 15
    CHECK_FALSE(csp.invalidOpcodeCheck);  // nudspchA consumes rb+0x30 bit 0
    CHECK(csp.lastInvalidResume == 0x1006);
}

TEST_CASE("msp: XFER is a synchronous extended-control-store call")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();
    const uint8_t code[] = {0xF5, 0x02, 0x00, 0x3C, 0x5A, 0x20, 0x00};
    m.write(0x1000, code, sizeof code);
    m.msp.iar = 0x1000;

    REQUIRE(msp.step());
    CHECK_FALSE(msp.stopped());
    CHECK(m.msp.iar == 0x1003);
    CHECK(csp.lastXferQ == 0x02);
    CHECK(csp.lastXferR == 0x00);
    CHECK(csp.lastXferIar == 0x1000);
    REQUIRE(msp.step());
    CHECK(m.readByte(0x2000) == 0x5A);
}

TEST_CASE("msp: a refused XFER reports a check instead of silently halting")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();
    const uint8_t code[] = {0xF5, 0x02, 0x00};
    m.write(0x1000, code, sizeof code);
    m.msp.iar = 0x1000;
    csp.acceptXfer = false;

    CHECK_FALSE(msp.step());
    CHECK(msp.stopped());
    CHECK(m.msp.iar == 0x1003);
    CHECK(msp.atPreemptionPoint());
    CHECK(msp.instructionsExecuted() == 1);
    CHECK(msp.stopReason().find("XFER 02,00 at 1000 refused by the test control storage processor") == 0);
    const auto checks = m.copyCheckHistory();
    REQUIRE(checks.size() == 1);
    CHECK(checks.front().find("XFER not serviced") != std::string::npos);
}

TEST_CASE("msp: breakpoints halt before the fetch and step past once")
{
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();
    // Three MVIs, then a jump back to the first.
    const uint8_t bytes[] = {0x3C, 0x01, 0x20, 0x00, 0x3C, 0x02, 0x20, 0x01, 0x3C, 0x03, 0x20, 0x02, 0xF1, 0x00, 0x0F};
    m.write(0x1000, bytes, sizeof bytes);
    m.msp.iar = 0x1000;
    msp.setBreakpoint(0x1004, "second");
    CHECK(msp.step());
    CHECK_FALSE(msp.step());
    CHECK(msp.stopReason() == "breakpoint at 1004 (second)");
    CHECK(m.readByte(0x2001) == 0);
    msp.start();
    CHECK(msp.step());          // steps past the breakpoint once
    CHECK(m.readByte(0x2001) == 2);
    CHECK(msp.step());
    CHECK(msp.step());          // the jump back
    CHECK(m.msp.iar == 0x1000);
    CHECK(msp.step());
    CHECK_FALSE(msp.step());    // and it breaks again on the loop
    CHECK(msp.instructionsExecuted() == 5);
    CHECK(msp.clearBreakpoint(0x1004));
    CHECK_FALSE(msp.clearBreakpoint(0x1004));
}

TEST_CASE("msp: a protected page is a level 5 interrupt, and an unmapped survivor stops the machine")
{
    machine::MachineState m(64 * 1024, 1024 * 1024);
    monitor::Tracer t;
    RefusingCsp csp(m, t);
    MainStorageProcessor& msp = csp.mainStorage();
    // MVC of 4 bytes whose destination crosses into a protected page.
    const uint8_t bytes[] = {0x0C, 0x03, 0x08, 0x01, 0x30, 0x03};
    m.write(0x1000, bytes, sizeof bytes);
    m.msp.iar = 0x1000;
    m.msp.pactDir = 0x80;
    m.atr[machine::MachineState::kAtrTaskGroup0 + 0] = 0x0010;   // logical 0000-07FF -> real 8000
    m.atr[machine::MachineState::kAtrTaskGroup0 + 1] = machine::MachineState::kAtrProtect;
    m.atr[machine::MachineState::kAtrTaskGroup0 + 6] = 0x0020;   // source page mapped
    for (int i = 0; i < 4; ++i) m.writeByte((0x20 << 11) + 0x000 + i, static_cast<uint8_t>(0xA0 + i));
    CHECK_FALSE(msp.step());
    CHECK(msp.stopped());
    // The operand is resolved for read before the instruction runs, so the
    // violation is reported as a read of the rightmost byte.
    CHECK(msp.stopReason().find("storage protection violation at 0801 (read)") == 0);
    // The bytes before the fault were written, the rest were not.
    CHECK(m.readByte(0x8000 + 0x7FE) == 0);
    CHECK(m.readByte(0x8000 + 0x7FF) == 0);
}
