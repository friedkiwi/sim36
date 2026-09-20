#include <doctest/doctest.h>

#include <vector>

#include "Devices/VirtualWorkstation.h"
#include "Host/StationBackend.h"
#include "Monitor/Tracer.h"

using namespace sim36;

TEST_CASE("workstation: Controller Text Assist selects and preserves the D932 read family")
{
    monitor::Tracer trace;
    host::WorkstationBackend terminal("127.0.0.1", 0, "test station", &trace, [] {});
    terminal.attachConsole();
    configuration::StationConfig config;
    devices::VirtualWorkstation station(config, terminal, trace, false);

    // Real DW/36 shape: CLEAR UNIT followed by WSF containing D934. The
    // 13-byte field includes an embedded 04 to prove detection follows LL
    // rather than searching the payload for byte pairs.
    const std::vector<uint8_t> defineText = {
        0x04, 0x40, 0x04, 0xF3,
        0x00, 0x0D, 0xD9, 0x34, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x40, 0x00, 0x12,
    };
    REQUIRE(station.outputData(defineText.data(), 0, static_cast<int>(defineText.size())));
    CHECK(station.inviteReadMode() == devices::PutWithInviteReadMode::StructuredField21);

    // CTA remains terminal-owned: the defining stream is forwarded exactly.
    auto history = terminal.captureDiagnosticHistory();
    REQUIRE(!history.empty());
    CHECK(history.back().data == defineText);

    // A separate System/36 Invite is not an empty RFC Invite in WP mode. It
    // carries the architected READ TEXT SCREEN structured field.
    REQUIRE(station.invite());
    history = terminal.captureDiagnosticHistory();
    REQUIRE(!history.empty());
    CHECK(history.back().opcode == host::WorkstationOpcode::PutGet);
    CHECK(history.back().data == std::vector<uint8_t>{0x04, 0xF3, 0x00, 0x08, 0xD9, 0x32, 0x00, 0x80, 0x00, 0x00});
    CHECK(station.activeReadMode() == 0x21);

    // The original terminal performs CTA editing and returns an architected
    // D932 response. Command 42 is merely the System/36 controller copy at
    // this point: DP field expansion would corrupt this structured payload.
    const std::vector<uint8_t> response = {
        0x03, 0x02, 0xF1,
        0x00, 0x0C, 0xD9, 0x32, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02,
    };
    terminal.injectInput(host::WorkstationRecord(host::WorkstationOpcode::PutGet, host::WorkstationRecordFlags::None,
                                                  response));
    REQUIRE(station.tryCompleteInviteResponse());
    std::vector<uint8_t> copied;
    REQUIRE(station.tryTakeInputFields(0x42, copied));
    CHECK(copied == response);

    // WRITE TO DISPLAY is DP-only, so returning to an ordinary panel also
    // restores the matching Read Input Fields family.
    const std::vector<uint8_t> ordinary = {0x04, 0x11, 0x00, 0x00};
    REQUIRE(station.outputData(ordinary.data(), 0, static_cast<int>(ordinary.size())));
    CHECK(station.inviteReadMode() == devices::PutWithInviteReadMode::ReadInputFields20);
}

TEST_CASE("workstation: Text Assist family tables do not falsely enter WP mode")
{
    monitor::Tracer trace;
    host::WorkstationBackend terminal("127.0.0.1", 0, "test station", &trace, [] {});
    terminal.attachConsole();
    configuration::StationConfig config;
    devices::VirtualWorkstation station(config, terminal, trace, false);

    // D930 contains D9 34 in its data. LL keeps it part of the audit table,
    // not a DEFINE TEXT SCREEN FORMAT command.
    const std::vector<uint8_t> audit = {0x04, 0xF3, 0x00, 0x08, 0xD9, 0x30, 0xD9, 0x34, 0x00, 0x00};
    REQUIRE(station.outputData(audit.data(), 0, static_cast<int>(audit.size())));
    CHECK(station.inviteReadMode() == devices::PutWithInviteReadMode::ReadInputFields20);
}

TEST_CASE("workstation: DP invite establishes Read MDT and control keys bypass field input")
{
    monitor::Tracer trace;
    host::WorkstationBackend terminal("127.0.0.1", 0, "test station", &trace, [] {});
    terminal.attachConsole();
    configuration::StationConfig config;
    devices::VirtualWorkstation station(config, terminal, trace, false);

    REQUIRE(station.invite());
    auto history = terminal.captureDiagnosticHistory();
    REQUIRE(!history.empty());
    CHECK(history.back().opcode == host::WorkstationOpcode::PutGet);
    CHECK(history.back().data == std::vector<uint8_t>{0x04, 0x52, 0x00, 0x00});
    CHECK(station.activeReadMode() == 0x20);

    terminal.injectInput(host::WorkstationRecord(
        host::WorkstationOpcode::NoOperation, host::WorkstationRecordFlags::Attention, {}));
    CHECK(terminal.attentionPending());
    CHECK(terminal.pendingInput() == 0);
    host::WorkstationRecordFlags flags;
    REQUIRE(terminal.tryTakeUnsolicitedRequest(flags));
    CHECK(flags == host::WorkstationRecordFlags::Attention);
    CHECK_FALSE(terminal.tryTakeUnsolicitedRequest(flags));

    terminal.injectInput(host::WorkstationRecord(
        host::WorkstationOpcode::NoOperation, host::WorkstationRecordFlags::SystemRequest, {}));
    CHECK(terminal.pendingInput() == 0);
    REQUIRE(terminal.tryTakeUnsolicitedRequest(flags));
    CHECK(flags == host::WorkstationRecordFlags::SystemRequest);
}
