#include <doctest/doctest.h>

#include "Machine/MachineState.h"
#include "Machine/MspRegisters.h"
#include "Machine/Scheduler.h"

using namespace sim36::machine;

TEST_CASE("psr: loading normalises bits 5, 6 and 7")
{
    MspRegisters r;
    r.loadPsr(0x01);            // equal on -> high and low forced off
    CHECK(r.psr() == 0x01);
    r.loadPsr(0x07);
    CHECK(r.psr() == 0x01);
    r.loadPsr(0x00);            // low and equal off -> high on
    CHECK(r.psr() == 0x04);
    r.loadPsr(0x02);            // low on, equal off -> high off
    CHECK(r.psr() == 0x02);
    r.loadPsr(0x06);
    CHECK(r.psr() == 0x02);
    r.loadPsr(0x20 | 0x02);     // binary overflow survives
    CHECK(r.psr() == 0x22);
}

TEST_CASE("psr: WMPR load uses only bits 5 and 7")
{
    MspRegisters r;
    r.loadPsr(0x04);
    r.loadPsrFromWmpr(0x01);
    CHECK(r.psr() == 0x01);
    r.loadPsrFromWmpr(0x00);    // bits 5 and 7 off -> low on
    CHECK((r.psr() & 0x02) != 0);
    CHECK((r.psr() & 0x01) == 0);
    r.loadPsrFromWmpr(0x04);    // bit 5 on, 7 off -> low off
    CHECK((r.psr() & 0x02) == 0);
}

TEST_CASE("pmr: bits 3-6 are projected from the PACT bytes")
{
    MspRegisters r;
    r.setPmr(0x80 | 0x10 | 0x02 | 0x01);
    CHECK(r.pactXr1Bit());
    CHECK(r.pactPdirBit());
    CHECK_FALSE(r.pactIarBit());
    CHECK(r.pmr() == 0x93);
    CHECK_FALSE(r.privileged());
    CHECK(r.taskDispatchingEnabled());
    r.pactIar = 0x80;
    CHECK((r.pmr() & MspRegisters::kPmrPiar) != 0);
    r.setTranslatedAddressing(true);
    CHECK(r.cmr == 0x01);
    CHECK(r.pactCsp == 0x80);
    r.reset();
    CHECK(r.pmr() == 0);
    CHECK(r.pactCsp == 0);
}

TEST_CASE("state: untranslated addressing concatenates four prefix bits")
{
    MachineState m(64 * 1024, 1024 * 1024);
    int real = 0;
    REQUIRE(m.resolve(0x82CB, 0x40, MachineState::kAtrTaskGroup0, false, real));
    CHECK(real == 0x0082CB);   // flag bits above the nibble are not address
    REQUIRE(m.resolve(0x1234, 0x05, MachineState::kAtrTaskGroup0, false, real));
    CHECK(real == 0x051234);
}

TEST_CASE("state: translation selects an ATR by address bits 0-4 and honours protection")
{
    MachineState m(64 * 1024, 1024 * 1024);
    m.atr[MachineState::kAtrTaskGroup0 + 3] = 0x0010;        // logical page 3 -> frame 0x10
    m.atr[MachineState::kAtrTaskGroup0 + 4] = MachineState::kAtrProtect;
    int real = 0;
    REQUIRE(m.resolve(0x1805, 0x80, MachineState::kAtrTaskGroup0, false, real));
    CHECK(real == (0x10 << 11) + 0x005);
    StorageProtection fault;
    CHECK_FALSE(m.resolve(0x2000, 0x80, MachineState::kAtrTaskGroup0, true, real, &fault));
    CHECK(fault.address == 0x2000);
    CHECK(fault.forWrite);
    CHECK(fault.message() == "storage protection violation at 2000 (write)");
    // A 24-bit device-path address is real unless bit 0x800000 is set.
    CHECK(m.resolveGuest24(0x001234, false, real));
    CHECK(real == 0x1234);
    CHECK(m.resolveGuest24(0x801805, false, real));
    CHECK(real == (0x10 << 11) + 0x005);
    CHECK_FALSE(m.resolveGuest24(0x802000, false, real));
}

TEST_CASE("state: translated extents follow the registers page by page")
{
    MachineState m(64 * 1024, 1024 * 1024);
    m.atr[MachineState::kAtrTaskGroup0 + 0] = 0x0020;
    m.atr[MachineState::kAtrTaskGroup0 + 1] = 0x0021;    // adjacent frame -> one extent
    m.atr[MachineState::kAtrTaskGroup0 + 2] = 0x0005;    // elsewhere -> a second extent
    std::vector<std::pair<int, int>> extents;
    REQUIRE(m.translatedExtents(0x0700, MachineState::kAtrTaskGroup0, 0x1400, true, extents));
    REQUIRE(extents.size() == 3);
    CHECK(extents[0].first == (0x20 << 11) + 0x700);   // pages 0 and 1 are adjacent frames
    CHECK(extents[0].second == 0x100 + 0x800);
    CHECK(extents[1].first == (0x05 << 11));           // page 2 lives elsewhere
    CHECK(extents[1].second == 0x800);
    CHECK(extents[2].first == 0);                      // page 3: ATR 0 names frame 0
    CHECK(extents[2].second == 0x300);
}

TEST_CASE("state: out-of-range access faults instead of throwing")
{
    MachineState m(4096);
    CHECK(m.readByte(4096) == 0);
    CHECK(m.faulted());
    CHECK(m.faultMessage() == "guest access 001000+1 outside 001000 bytes of main storage");
    m.clearFault();
    m.writeHalf(10, 0x1234);
    CHECK(m.readHalf(10) == 0x1234);
    CHECK(m.readByte(11) == 0x34);
    m.writeAddr24(20, 0xABCDEF);
    CHECK(m.readAddr24(20) == 0xABCDEF);
    CHECK_FALSE(m.faulted());
}

TEST_CASE("state: watchpoints report before and after")
{
    MachineState m(4096);
    int hits = 0;
    m.onWatchWrite = [&](int addr, int len, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
        ++hits;
        CHECK(addr == 0x102);
        CHECK(len == 2);
        CHECK(before[0] == 0);
        CHECK(after[1] == 0x44);
    };
    m.addWatch(0x102, 0x103);
    uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
    m.write(0x100, data, 4);
    m.write(0x200, data, 4);
    CHECK(hits == 1);
}

TEST_CASE("scheduler: events run in time order, ties by insertion")
{
    Scheduler s;
    std::vector<int> order;
    s.at(10, "b", [&]() { order.push_back(2); });
    s.at(5, "a", [&]() { order.push_back(1); });
    s.at(10, "c", [&]() { order.push_back(3); });
    s.after(1, "d", [&]() { order.push_back(0); });
    s.runUntil(9);
    CHECK(order == std::vector<int>{0, 1});
    CHECK(s.now() == 9);
    s.runUntil(100);
    CHECK(order == std::vector<int>{0, 1, 2, 3});
    CHECK(s.now() == 100);
    CHECK(s.pending() == 0);
    CHECK(s.restoreClock(7));
    s.at(1, "late", [&]() { order.push_back(9); });
    CHECK(s.peek().front().at == 7);   // never before now
    CHECK_FALSE(s.restoreClock(0));
}
