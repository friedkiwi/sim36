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
#include "Devices/WorkStationIob.h"
#include "Devices/WorkStationController.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Processors/ControlStorage/DirectArea.h"
#include "Processors/ControlStorage/GuestHeap.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/NuPtt.h"
#include "Processors/ControlStorage/TaskBlock.h"
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

TEST_CASE("Advanced/36 dispatches XFER to the BASIC and FORTRAN assist stubs")
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
    std::string failure;
    REQUIRE(csp.restoreCheckpointMemory(std::vector<uint8_t>(state.backingBytes()), task, request, failure));
    state.msp.iar = 0x3FCD;
    state.msp.xr1 = 0x3B00;
    state.msp.pactIar = machine::MspRegisters::kPactTranslate;

    CHECK_FALSE(csp.extendedControlStore(0x02, 0x00, 0x3FCA));
    CHECK(csp.lastRefusal().find("NuBasic") != std::string::npos);
    CHECK(state.readByte(request + RequestBlock::kOffOpcode) == 0xF5);
    CHECK(state.readByte(request + RequestBlock::kOffQByte) == 0x02);
    CHECK(state.readByte(request + RequestBlock::kOffRByte) == 0x00);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x3FCD);
    CHECK(state.readHalf(request + RequestBlock::kOffXr1Low) == 0x3B00);

    CHECK_FALSE(csp.extendedControlStore(0x01, 0x05, 0x2000));
    CHECK(csp.lastRefusal().find("NuFortran") != std::string::npos);
    CHECK(state.readByte(request + RequestBlock::kOffQByte) == 0x01);
    CHECK(state.readByte(request + RequestBlock::kOffRByte) == 0x05);
}

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

TEST_CASE("retained termination context keeps its native slot-4 continuation")
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
    std::vector<uint8_t> memory(static_cast<std::size_t>(state.backingBytes()), 0);
    memory[task] = 0xE3;
    memory[task + 1] = 0xC2;
    memory[task + TaskBlock::kOffRequestBlock] = static_cast<uint8_t>(request >> 16);
    memory[task + TaskBlock::kOffRequestBlock + 1] = static_cast<uint8_t>(request >> 8);
    memory[task + TaskBlock::kOffRequestBlock + 2] = static_cast<uint8_t>(request);

    std::string failure;
    REQUIRE(csp.restoreCheckpointMemory(memory, task, request, failure));
    As36ControlStorageProcessor::CheckpointState checkpoint;
    REQUIRE(csp.captureCheckpoint(checkpoint, failure));
    checkpoint.nativeTransferContinuations = {
        task, 1, static_cast<int>(As36ControlStorageProcessor::NativeTransferContinuation::NuptermSlot4)};
    REQUIRE(csp.restoreCheckpoint(checkpoint, failure));

    SvcRequest exit;
    exit.r = 0x11;
    // This minimal fixture has no transfer table, so the root exit itself
    // cannot complete.  The assertion here is that merely encountering the
    // retained root exit does not consume its suspended native frame.
    CHECK_FALSE(csp.svc(exit));

    REQUIRE(csp.captureCheckpoint(checkpoint, failure));
    CHECK(checkpoint.nativeTransferContinuations ==
          std::vector<int>{task, 1, static_cast<int>(As36ControlStorageProcessor::NativeTransferContinuation::NuptermSlot4)});
}

TEST_CASE("a printer put completes when the issuing task waits, not inside its SVC")
{
    CspEmptyVolume volume;
    configuration::EmulatorConfig config;
    machine::MachineState state(128 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadOnly);
    devices::DeviceSet devices(state, disk, trace);
    As36ControlStorageProcessor csp(state, config, devices, disk, trace);

    const std::filesystem::path output = std::filesystem::temp_directory_path() /
        ("sim36-spwrt-event-" + std::to_string(reinterpret_cast<std::uintptr_t>(&state)) + ".bin");
    std::filesystem::remove(output);
    host::PrinterBackend printerBackend("127.0.0.1", 0, "SPWRT test printer", &trace, [] {}, "file", output.string());
    configuration::StationConfig printerConfig;
    printerConfig.address = 1;
    printerConfig.role = "printer";
    printerConfig.deviceCode = "PB";
    printerConfig.printerOutput = "file";
    printerConfig.printerOutputPath = output.string();
    devices::VirtualPrinter printer(printerConfig, printerBackend, trace, false);
    devices.addPrinter(printer);

    constexpr int task = 0x1000;
    constexpr int requestBlock = 0x1100;
    constexpr int iob = 0x1200;
    constexpr int data = 0x1300;
    std::string failure;
    REQUIRE(csp.restoreCheckpointMemory(std::vector<uint8_t>(state.backingBytes()), task, requestBlock, failure));
    state.writeHalf(task, TaskBlock::kEyecatcher);
    state.writeAddr24(task + TaskBlock::kOffRequestBlock, requestBlock);
    csp.bringUpControlProcessor();

    // This is the shape captured from SSP 7.5 SPWRT: a multiple-wait ECM,
    // printer command 27, and the initial three-byte vertical-position record.
    state.writeByte(iob + Ecm::kOffMultiWait, 0x80);
    state.writeByte(iob + devices::WorkStationIob::kOffClass, 0xC2);
    state.writeByte(iob + devices::WorkStationIob::kOffCommand, devices::WorkStationIob::kCmdPut);
    state.writeByte(iob + devices::WorkStationIob::kOffUnitAddress, 0x01);
    state.writeAddr24(iob + devices::WorkStationIob::kOffDataBuffer, data);
    state.writeHalf(iob + devices::WorkStationIob::kOffLength, 3);
    state.writeByte(data, 0x34);
    state.writeByte(data + 1, 0xC4);
    state.writeByte(data + 2, 0x01);
    state.msp.pactXr1 = 0;
    state.msp.xr1 = iob;

    SvcRequest print;
    print.r = 0x42;
    print.q = 0x08;  // multiple-wait action element
    REQUIRE(csp.svc(print));
    // The record has left the machine but the operation is not over: the
    // mask stays armed and nothing is queued, so SPWRT's no-wait poll of its
    // disk reads (Q=0C, type 0020) cannot consume the printer's event.
    CHECK((state.readByte(iob + Ecm::kOffCompletion) & 0x40) == 0);
    CHECK(state.readAddr24(task + TaskBlock::kOffCompleteQueue) == 0);
    state.msp.wr[6] = 0x0020;
    SvcRequest poll;
    poll.r = 0x02;
    poll.q = 0x0C;  // multiple wait, event type supplied, without blocking
    REQUIRE(csp.svc(poll));
    CHECK(state.readAddr24(task + TaskBlock::kOffCompleteQueue) == 0);

    // SPWRT then waits for the printer.  Giving the processor away is what
    // ends the operation: the element is posted with type 2000 and the
    // wait it enters is satisfied by it.
    state.msp.wr[6] = 0x2020;
    SvcRequest wait;
    wait.r = 0x02;
    wait.q = 0x4D;  // multiple wait, event type supplied, blocking
    REQUIRE(csp.svc(wait));
    CHECK(state.readByte(iob + Ecm::kOffCompletion) == 0x40);
    CHECK(state.readAddr24(task + TaskBlock::kOffCompleteQueue) == 0);
    CHECK(state.msp.wr[6] == 0x2000);

    REQUIRE(printer.endJob());
    std::filesystem::remove(output);
}

TEST_CASE("general post completes a queued ACE when ECM address return was not requested")
{
    CspEmptyVolume volume;
    configuration::EmulatorConfig config;
    machine::MachineState state(128 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadOnly);
    devices::DeviceSet devices(state, disk, trace);
    As36ControlStorageProcessor csp(state, config, devices, disk, trace);

    constexpr int task = 0x1000;
    constexpr int requestBlock = 0x1100;
    constexpr int ecm = 0x1200;
    std::string failure;
    REQUIRE(csp.restoreCheckpointMemory(std::vector<uint8_t>(state.backingBytes()), task, requestBlock, failure));
    state.writeHalf(task, TaskBlock::kEyecatcher);
    state.writeAddr24(task + TaskBlock::kOffRequestBlock, requestBlock);
    csp.bringUpControlProcessor();
    state.msp.pactXr1 = 0;
    state.msp.xr1 = ecm;
    state.writeHalf(ecm + Ecm::kOffGeneralPostMask, 0x2200);

    SvcRequest build;
    build.r = 0x4C;
    build.q = 0x00; // specifically do not return the ACE through ECM+2
    build.inline1 = 30;
    REQUIRE(csp.svc(build));
    CHECK(state.readAddr24(ecm + Ecm::kOffAceAddress) == 0);
    const int ace = state.readAddr24(GuestLowStorage::queueHeader(30));
    REQUIRE(ace != 0);

    SvcRequest post;
    post.r = 0x01;
    post.inline1 = 0x22;
    post.inline2 = 0x00;
    REQUIRE(csp.svc(post));
    CHECK(state.readByte(ecm + Ecm::kOffCompletion) == Ecm::kComplete);
    CHECK(state.readAddr24(GuestLowStorage::queueHeader(30)) == 0);
    CHECK(state.readAddr24(task + TaskBlock::kOffCompleteQueue) == ace);
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
    CHECK(m.readByte(0x084E) == devices::WorkStationController::kPortCount);
    CHECK(m.readByte(0x084F) == devices::WorkStationController::kAddressableDevices);

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
    CHECK(m.readByte(0x084E) == devices::WorkStationController::kPortCount);
    CHECK(m.readByte(0x084F) == devices::WorkStationController::kAddressableDevices);
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

TEST_CASE("power-on migrates the incompatible synthetic Advanced/36 personality")
{
    CspEmptyVolume volume(9000);
    configuration::EmulatorConfig config;
    machine::MachineState state(1024 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadWrite);
    const std::vector<uint8_t> obsolete = GuestLowStorage::synthesizeUnitDefinitionTable(0x89);
    for (int base : {As36ControlStorageProcessor::kUdtSector,
                     As36ControlStorageProcessor::kUdtMirrorSector}) {
        for (int i = 0; i < As36ControlStorageProcessor::kUdtPersistedSectors; i++)
            REQUIRE(disk.writeSector(base + i,
                obsolete.data() + i * storage::DiskBackend::kSectorBytes));
    }
    devices::DeviceSet devices(state, disk, trace);
    As36ControlStorageProcessor csp(state, config, devices, disk, trace);

    csp.bringUpControlProcessor();
    csp.iplMainProcessor();

    std::vector<uint8_t> primary;
    std::vector<uint8_t> mirror;
    REQUIRE(disk.readSector(As36ControlStorageProcessor::kUdtSector, primary));
    REQUIRE(disk.readSector(As36ControlStorageProcessor::kUdtMirrorSector, mirror));
    CHECK(primary[11] == 0x8D);
    CHECK(mirror[11] == 0x8D);
    CHECK(state.readByte(0x0850) == 0x8D);
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
