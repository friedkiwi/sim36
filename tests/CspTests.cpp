// The control storage processor's own structures: the system queue space
// allocator, the task work area, the direct areas and the queue engine as
// SVC 0E drives it.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Devices/WorkStationController.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Processors/ControlStorage/DirectArea.h"
#include "Processors/ControlStorage/GuestHeap.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/NuPtt.h"
#include "Processors/ControlStorage/TaskWorkArea.h"
#include "Storage/DiskBackend.h"

using namespace sim36;
using namespace sim36::processors::controlstorage;

namespace {

struct CspEmptyVolume {
    std::filesystem::path path;

    explicit CspEmptyVolume(int sectors = 32)
    {
        path = std::filesystem::temp_directory_path() /
               ("sim36-csp-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".img");
        std::vector<uint8_t> bytes(static_cast<std::size_t>(sectors) * storage::DiskBackend::kSectorBytes, 0);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    ~CspEmptyVolume() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("SSP's final #CCPW power-control wait stops the emulated machine")
{
    CspEmptyVolume volume;
    configuration::EmulatorConfig config;
    machine::MachineState state(128 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadOnly);
    devices::DeviceSet devices(state, disk, trace);
    As36ControlStorageProcessor csp(state, config, devices, disk, trace);

    constexpr int task = 0x1000;
    constexpr int request = 0x1100;
    constexpr int program = 0x1200;
    constexpr int logicalBase = 0x1000;
    constexpr uint16_t iar = 0x13E5;
    constexpr int frame = 0x20;
    constexpr int physical = frame * machine::MachineState::kPageBytes + (iar & 0x7FF);

    std::string failure;
    REQUIRE(csp.restoreCheckpointMemory(std::vector<uint8_t>(state.backingBytes()), task, request, failure));
    state.writeAddr24(task + 65, request);
    state.writeAddr24(request + RequestBlock::kOffProgramBlock, program);
    state.atr[machine::MachineState::kAtrTaskGroup0 + (iar >> machine::MachineState::kPageShift)] = frame;
    state.writeByte(physical, 0xF1);
    state.writeByte(physical + 1, 0x87);
    state.writeByte(physical + 2, 0x03);
    state.msp.iar = iar;
    state.msp.pactIar = machine::MspRegisters::kPactTranslate;

    As36ControlStorageProcessor::CheckpointState checkpoint;
    REQUIRE(csp.captureCheckpoint(checkpoint, failure));
    checkpoint.loadedMemberData = {program, 94214, logicalBase};
    checkpoint.loadedMemberNames = {"#OTHER"};
    REQUIRE(csp.restoreCheckpoint(checkpoint, failure));
    CHECK_FALSE(csp.detectSystemPowerOff());
    CHECK_FALSE(csp.mainStorage().stopped());

    checkpoint.loadedMemberNames = {"#CCPW"};
    REQUIRE(csp.restoreCheckpoint(checkpoint, failure));
    state.writeByte(physical + 2, 0x04);
    CHECK_FALSE(csp.detectSystemPowerOff());
    CHECK_FALSE(csp.mainStorage().stopped());
    state.writeByte(physical + 2, 0x03);

    REQUIRE(csp.detectSystemPowerOff());
    CHECK(csp.systemPowerOffRequested());
    CHECK(csp.mainStorage().stopped());
    CHECK(csp.mainStorage().stopReason().find("#CCPW+03E5") != std::string::npos);

    // A new power-on or restored machine is live again; the shutdown latch
    // is host state, not persistent SSP state.
    REQUIRE(csp.restoreCheckpointMemory(std::vector<uint8_t>(state.backingBytes()), 0, 0, failure));
    CHECK_FALSE(csp.systemPowerOffRequested());
    CHECK_FALSE(csp.mainStorage().stopped());
}

TEST_CASE("workspace heap checkpoints cannot exceed their live block capacity")
{
    WorkSpaceHeap heap(2 * machine::MachineState::kPageBytes);
    CHECK(heap.capacity() == 4096);
    CHECK(heap.restoreCheckpoint({0, 4096}));
    CHECK_FALSE(heap.restoreCheckpoint({0xB000, 0x2000}));
}

TEST_CASE("translated assign Q bit 2 chooses the placement spanning the fewest pages")
{
    WorkSpaceHeap ordinary(2 * machine::MachineState::kPageBytes);
    WorkSpaceHeap compact(2 * machine::MachineState::kPageBytes);

    // Leave a free run beginning 64 bytes before a page boundary.  First fit
    // crosses that boundary; the architected Q-bit-2 placement advances to
    // the boundary and occupies one page only.
    CHECK(ordinary.allocate(0x7C0) == 0);
    CHECK(compact.allocate(0x7C0) == 0);
    CHECK(ordinary.allocate(0x80) == 0x7C0);
    CHECK(compact.allocate(0x80, true) == 0x800);
    CHECK(compact.available() == ordinary.available());

    // The skipped 64-byte head remains allocatable.
    CHECK(compact.allocate(0x40) == 0x7C0);
    CHECK(compact.allocate(0, true) == -1);
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

    REQUIRE(GuestLowStorage::build(m, trace, 819200, host, 0x8D, error));
    CHECK(error.empty());
    CHECK(m.readByte(0x0850) == 0x8D);
    CHECK(m.readByte(0x08BD) == 0x8D);
    CHECK(m.readByte(0x08C3) == devices::WorkStationController::kMaxDevices);

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
    // A UDT without an id-61/class-C0 entry must not turn the live
    // controller's capacity into the number of currently defined stations,
    // or into zero.
    CHECK(m.readByte(0x08C3) == devices::WorkStationController::kMaxDevices);
}

TEST_CASE("power-on UDT describes the hardware implemented by the emulator")
{
    std::vector<uint8_t> udt = GuestLowStorage::synthesizeUnitDefinitionTable(0x8D);
    REQUIRE(udt.size() == 4096);
    CHECK(udt[0] == 0x01);
    CHECK(udt[11] == 0x8D);

    machine::MachineState m(1024 * 1024);
    monitor::Tracer trace;
    GuestLowStorage::walkUnitDefinitionTable(m, trace, udt.data(), static_cast<int>(udt.size()));
    CHECK(m.readByte(0x0849) == 0x45);
    CHECK(m.readByte(0x0851) == 0x01);
    CHECK(m.readByte(0x08B2) == 0xC0);
    CHECK(m.readByte(0x08C3) == devices::WorkStationController::kMaxDevices);
}

TEST_CASE("blank volumes receive the power-on UDT at its primary and mirror sectors")
{
    CspEmptyVolume volume(9000);
    configuration::EmulatorConfig config;
    machine::MachineState state(1024 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadWrite);
    devices::DeviceSet devices(state, disk, trace);
    As36ControlStorageProcessor csp(state, config, devices, disk, trace);

    csp.bringUpControlProcessor();
    csp.iplMainProcessor();

    const std::vector<uint8_t> expected =
        GuestLowStorage::synthesizeUnitDefinitionTable(config.systemCustomize1());
    for (int i = 0; i < As36ControlStorageProcessor::kUdtPersistedSectors; i++) {
        std::vector<uint8_t> primary;
        std::vector<uint8_t> mirror;
        REQUIRE(disk.readSector(As36ControlStorageProcessor::kUdtSector + i, primary));
        REQUIRE(disk.readSector(As36ControlStorageProcessor::kUdtMirrorSector + i, mirror));
        const auto first = expected.begin() + i * storage::DiskBackend::kSectorBytes;
        const auto last = first + storage::DiskBackend::kSectorBytes;
        CHECK(std::equal(first, last, primary.begin()));
        CHECK(primary == mirror);
    }
}

TEST_CASE("guest low storage accepts the 5363 system customize selector")
{
    machine::MachineState m(1024 * 1024);
    monitor::Tracer trace;
    GuestLowStorage::HostInfo host;
    std::string error;

    REQUIRE(GuestLowStorage::build(m, trace, 819200, host, 0x8B, error));
    CHECK(m.readByte(0x0850) == 0x8B);
    CHECK(m.readByte(0x08BD) == 0x8B);
}
