// The control storage processor's own structures: the system queue space
// allocator, the task work area, the direct areas and the queue engine as
// SVC 0E drives it.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/DirectArea.h"
#include "Processors/ControlStorage/GuestHeap.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/NuPtt.h"
#include "Processors/ControlStorage/TaskWorkArea.h"

using namespace sim36;
using namespace sim36::processors::controlstorage;

TEST_CASE("workspace heap checkpoints cannot exceed their live block capacity")
{
    WorkSpaceHeap heap(2 * machine::MachineState::kPageBytes);
    CHECK(heap.capacity() == 4096);
    CHECK(heap.restoreCheckpoint({0, 4096}));
    CHECK_FALSE(heap.restoreCheckpoint({0xB000, 0x2000}));
}

TEST_CASE("ATR pool rebases saved real frames when resident storage moves")
{
    NuPttPool pool;
    NuPtt* first = pool.allocate(0x1000);
    NuPtt* second = pool.allocate(0x2000);
    first->atr[2] = 0x210;
    first->atr[3] = 0x211;
    first->atr[4] = 0xFFFF;
    second->atr[7] = 0x215;
    second->atr[8] = 0x216;

    pool.rebaseFrames(0x210, 0x310, 6);

    CHECK(first->atr[2] == 0x310);
    CHECK(first->atr[3] == 0x311);
    CHECK(first->atr[4] == 0xFFFF);
    CHECK(second->atr[7] == 0x315);
    CHECK(second->atr[8] == 0x216);
}

TEST_CASE("guest heap: 16-byte granularity, power-of-two classes, contains")
{
    machine::MachineState m(1024 * 1024);
    monitor::Tracer trace;
    GuestHeap heap(m, 0x2000, 0x10000 - 0x2000, 0x10000, trace);
    heap.reset();
    CHECK(GuestHeap::roundedSize(1) == 16);
    CHECK(GuestHeap::roundedSize(16) == 16);
    CHECK(GuestHeap::roundedSize(17) == 32);
    CHECK(GuestHeap::roundedSize(100) == 128);
    int a = heap.allocate(32);
    REQUIRE(a != 0);
    CHECK(a >= 0x2000);
    CHECK(a < 0x10000);
    CHECK(heap.contains(a));
    CHECK_FALSE(heap.contains(0xF00));
    int b = heap.allocate(32);
    REQUIRE(b != 0);
    CHECK(b != a);
    heap.free(a, 32);
    // A freed block of the same class is handed back first.
    CHECK(heap.allocate(32) == a);
}

TEST_CASE("task work area: first fit, exact fit removal and coalescing")
{
    TaskWorkArea twa;
    int a = twa.allocate(10);
    int b = twa.allocate(20);
    CHECK(a == 0);
    CHECK(b == 10);
    twa.free(a, 10);
    // The hole at 0 is too small for 20 sectors and is skipped.
    CHECK(twa.allocate(20) == 30);
    CHECK(twa.allocate(10) == 0);
    twa.free(0, 10);
    twa.free(10, 20);
    // The two runs coalesce, so 30 sectors fit at 0 again.
    CHECK(twa.allocate(30) == 0);
    CHECK(twa.allocate(0) == -1);
}

TEST_CASE("direct area: word addressing and the range the access call honours")
{
    CHECK(DirectArea::word(0x40, 0x32) == 1074);
    CHECK(DirectArea::inRange(1074));
    CHECK(DirectArea::inRange(1124));
    CHECK_FALSE(DirectArea::inRange(0));
    DirectArea d;
    CHECK_FALSE(d.wasWritten(1074));
    d.write(1074, 0x1234);
    CHECK(d.wasWritten(1074));
    CHECK(d.read(1074) == 0x1234);
}

TEST_CASE("guest low storage: queue headers are 3-byte values at 0B03 + 4n")
{
    CHECK(GuestLowStorage::queueHeader(0) == 0xB01);
    CHECK(GuestLowStorage::queueHeader(37) == 0xB01 + 4 * 37);
    CHECK(GuestLowStorage::kTaskBlock == 0xF00);
    CHECK(GuestLowStorage::kEyeTaskBlock == 0xE3C2);
}

TEST_CASE("guest low storage seeds the blank-disk system customize selector")
{
    machine::MachineState m(1024 * 1024);
    monitor::Tracer trace;
    GuestLowStorage::HostInfo host;
    std::string error;

    REQUIRE(GuestLowStorage::build(m, trace, 819200, host, error));
    CHECK(error.empty());
    CHECK(m.readByte(0x0850) == 0x8D);
    CHECK(m.readByte(0x08BD) == 0x8D);

    // The seed is only a pre-UDT default.  A system entry's first customize
    // byte replaces both copies when an installed volume supplies one.
    std::vector<uint8_t> udt(4096);
    udt[0] = 0x01;   // system entry
    udt[1] = 0xFF;
    udt[2] = 0xFF;
    udt[8] = 1;      // customize area length
    udt[11] = 0x89;
    GuestLowStorage::walkUnitDefinitionTable(m, trace, udt.data(), static_cast<int>(udt.size()));
    CHECK(m.readByte(0x0850) == 0x89);
    CHECK(m.readByte(0x08BD) == 0x89);
}
