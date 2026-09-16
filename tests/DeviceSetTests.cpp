#include <doctest/doctest.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "Devices/DeviceSet.h"
#include "Devices/VirtualDiskette.h"
#include "Devices/WorkStationIob.h"
#include "Host/StationBackend.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Storage/DiskBackend.h"

using namespace sim36;

namespace {

struct EmptyVolume
{
    std::filesystem::path path;

    EmptyVolume()
    {
        path = std::filesystem::temp_directory_path() /
               ("sim36-device-set-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".img");
        std::vector<uint8_t> bytes(32 * storage::DiskBackend::kSectorBytes, 0);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    ~EmptyVolume() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("virtual CSP acknowledges diskette control-storage operations")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    trace.flags = monitor::TraceCsp;
    std::FILE* log = std::tmpfile();
    REQUIRE(log != nullptr);
    trace.to(log);
    devices::VirtualDiskette diskette(state, trace);

    constexpr int iob = 0x1000;
    for (int command : {devices::DisketteIoBlock::kCommandControlStorageDe,
                        devices::DisketteIoBlock::kCommandControlStorageDf}) {
        state.writeByte(iob + devices::DisketteIoBlock::kOffCommand, static_cast<uint8_t>(command));
        for (int n = 0; n < 4; ++n) state.writeByte(iob + devices::DisketteIoBlock::kOffStatus0 + n, 0xFF);
        REQUIRE(diskette.execute(iob, 0));
        CHECK(state.readByte(iob + 6) == 0x40);
        for (int n = 0; n < 4; ++n) CHECK(state.readByte(iob + devices::DisketteIoBlock::kOffStatus0 + n) == 0);
    }
    CHECK(diskette.virtualCspOperations() == 2);

    std::rewind(log);
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, log) != nullptr) output += buffer;
    CHECK(output.find("control-storage operation DE acknowledged successfully") != std::string::npos);
    CHECK(output.find("control-storage operation DF acknowledged successfully") != std::string::npos);
    std::fclose(log);
}

TEST_CASE("workstation: an A7 response survives until the same IOB's C1 status "
          "phase")
{
    EmptyVolume volume;
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadOnly);
    devices::DeviceSet devices(state, disk, trace);

    host::WorkstationBackend terminal("127.0.0.1", 0, "test station", &trace, [] {});
    terminal.attachConsole();
    configuration::StationConfig config;
    config.address = 1;
    devices::VirtualWorkstation station(config, terminal, trace, false);
    devices.addStation(station);

    constexpr int tub = 0x1000;
    constexpr int data = 0x2000;
    constexpr int iob = 0x3000;
    constexpr int requestBlock = 0x3800;
    state.writeHalf(tub, devices::WorkStationIob::kUnitBlockEyecatcher);
    state.writeByte(tub + devices::WorkStationIob::kOffUnitAddress, 0x01);
    state.writeByte(tub + devices::WorkStationIob::kOffClass, 0xC1);
    state.writeByte(iob + devices::WorkStationIob::kOffClass, devices::WorkStationIob::kClassWorkStation);
    state.writeByte(iob + devices::WorkStationIob::kOffCommand, devices::WorkStationIob::kCmdPutWithInvite);
    state.writeByte(iob + devices::WorkStationIob::kOffUnitAddress, 0x01);
    state.writeAddr24(iob + devices::WorkStationIob::kOffDataBuffer, data);
    state.writeHalf(iob + devices::WorkStationIob::kOffLength, 4);
    state.writeAddr24(iob + devices::WorkStationIob::kOffUnitBlock, tub);
    state.writeByte(data + 0, 0x04);
    state.writeByte(data + 1, 0x11);
    state.writeByte(data + 2, 0x00);
    state.writeByte(data + 3, 0x00);
    processors::controlstorage::RequestBlock::writeXr1(state, requestBlock, iob);

    processors::controlstorage::SvcRequest request;
    request.r = 0x43;
    request.requestBlock = requestBlock;
    REQUIRE(devices.deviceSvc(request));
    CHECK(devices.pendingPutWithInviteCount() == 1);
    CHECK(state.readByte(iob + devices::WorkStationIob::kOffClass) == 0xC1);

    terminal.injectInput(host::WorkstationRecord(host::WorkstationOpcode::PutGet, host::WorkstationRecordFlags::None,
                                                 std::vector<uint8_t>{0x01, 0x01, 0xF1}));
    CHECK(devices.hasPendingInputForUnit(0x01));
    CHECK_FALSE(devices.hasPendingInputForUnit(0x02));
    REQUIRE(devices.tryDeliverInputStatus(tub));
    int completedIob = 0;
    REQUIRE(devices.tryCompletePendingInput(completedIob));
    CHECK(completedIob == iob);
    CHECK(devices.pendingPutWithInviteCount() == 0);
    CHECK_FALSE(devices.hasPendingInputForUnit(0x01));

    // SSP now submits the same C1 IOB to import the response WSCF.  This
    // must complete from the already accepted Enter, without inviting and
    // waiting for a second keystroke.
    REQUIRE(devices.deviceSvc(request));
    CHECK_FALSE(devices.isPending(iob));
    CHECK(state.readByte(tub + 0x14) == 0xF1);
    CHECK(state.readByte(tub + 0x15) == 0x01);
    CHECK(state.readByte(tub + 0x16) == 0x01);
}

TEST_CASE("workstation: command 27 unwraps an SSP saved-screen envelope as RFC 1205 restore")
{
    EmptyVolume volume;
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    storage::DiskBackend disk(volume.path.string(), storage::VolumeMode::ReadOnly);
    devices::DeviceSet devices(state, disk, trace);

    host::WorkstationBackend terminal("127.0.0.1", 0, "test station", &trace, [] {});
    terminal.attachConsole();
    configuration::StationConfig config;
    config.address = 1;
    devices::VirtualWorkstation station(config, terminal, trace, false);
    devices.addStation(station);

    constexpr int tub = 0x1000;
    constexpr int data = 0x2000;
    constexpr int iob = 0x3000;
    constexpr int requestBlock = 0x3800;
    constexpr int supplied = 300;
    const std::vector<uint8_t> terminalBody = {
        0x04, 0x12, 0x09, 0x00, 0xD5, 0xD5, 0x00, 0x00, 0x00, 0xD5,
    };

    // Pair an opaque terminal image with the locally decoded panel that
    // produced it, then replace the live panel before requesting restore.
    const std::vector<uint8_t> originalPanel = {
        0x04, 0x40, 0x04, 0x11, 0x00, 0x00,
        0x11, 0x04, 0x05, 0x1D, 0x48, 0x00, 0x20, 0x00, 0x01, 0xE7,
    };
    REQUIRE(station.outputData(originalPanel.data(), 0, static_cast<int>(originalPanel.size())));
    REQUIRE(station.beginSaveScreen());
    terminal.injectInput(host::WorkstationRecord(host::WorkstationOpcode::SaveScreen, host::WorkstationRecordFlags::None,
                                                 terminalBody));
    std::vector<uint8_t> savedResponse;
    REQUIRE(station.tryTakeSaveScreen(savedResponse));
    CHECK(savedResponse == terminalBody);
    const std::vector<uint8_t> replacementPanel = {0x04, 0x40, 0x04, 0x11, 0x00, 0x00};
    REQUIRE(station.outputData(replacementPanel.data(), 0, static_cast<int>(replacementPanel.size())));
    CHECK(station.deviceDisplay().fields().empty());

    state.writeHalf(tub, devices::WorkStationIob::kUnitBlockEyecatcher);
    state.writeByte(tub + devices::WorkStationIob::kOffUnitAddress, 0x01);
    state.writeByte(iob + devices::WorkStationIob::kOffClass, devices::WorkStationIob::kClassWorkStation);
    state.writeByte(iob + devices::WorkStationIob::kOffCommand, devices::WorkStationIob::kCmdPut);
    state.writeByte(iob + devices::WorkStationIob::kOffUnitAddress, 0x01);
    state.writeAddr24(iob + devices::WorkStationIob::kOffDataBuffer, data);
    state.writeHalf(iob + devices::WorkStationIob::kOffLength, supplied);
    state.writeAddr24(iob + devices::WorkStationIob::kOffUnitBlock, tub);

    // This is the exact envelope shape found in the CNFIGSSP panic: the
    // command-27 length can exceed the saved record's allocation, and the
    // terminal-owned 0412 body begins after SSP's 22-byte prefix.
    state.writeHalf(data + 0, 0x0412);
    state.writeHalf(data + 2, 0x0100);
    state.writeByte(data + 18, 0x00);
    state.writeHalf(data + 20, static_cast<uint16_t>(terminalBody.size() - 2));
    for (std::size_t n = 0; n < terminalBody.size(); ++n)
        state.writeByte(data + 22 + static_cast<int>(n), terminalBody[n]);
    processors::controlstorage::RequestBlock::writeXr1(state, requestBlock, iob);

    processors::controlstorage::SvcRequest request;
    request.r = 0x43;
    request.requestBlock = requestBlock;
    REQUIRE(devices.deviceSvc(request));

    auto history = terminal.captureDiagnosticHistory();
    REQUIRE(!history.empty());
    CHECK(history.back().opcode == host::WorkstationOpcode::RestoreScreen);
    CHECK(history.back().data == terminalBody);
    REQUIRE(station.deviceDisplay().fields().size() == 1);
    CHECK(station.deviceDisplay().fields()[0].row == 4);
    CHECK(station.deviceDisplay().fields()[0].col == 6);
    REQUIRE(terminal.console()->fields().size() == 1);
    CHECK(terminal.console()->fields()[0].row == 4);
    CHECK(terminal.console()->fields()[0].col == 6);
}
