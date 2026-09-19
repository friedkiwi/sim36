#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/BasicAssist.h"
#include "Processors/ControlStorage/ExtendedControlStoreAssist.h"
#include "Processors/ControlStorage/FortranAssist.h"

using namespace sim36;
using namespace sim36::processors::controlstorage;

TEST_CASE("extended-control-storage assist context crosses translated pages")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);

    // Logical 07FF is the last byte of page 0.  Put page 1 in a deliberately
    // non-adjacent frame so a flat host pointer would fail this contract.
    state.atr[machine::MachineState::kAtrTaskGroup0] = 2;
    state.atr[machine::MachineState::kAtrTaskGroup0 + 1] = 5;
    const uint8_t source[] = {0x11, 0x22, 0x33};
    REQUIRE(context.writeGuest(0x8007FF, source, sizeof source));
    CHECK(state.readByte(2 * machine::MachineState::kPageBytes + 0x7FF) == 0x11);
    CHECK(state.readByte(5 * machine::MachineState::kPageBytes) == 0x22);
    CHECK(state.readByte(5 * machine::MachineState::kPageBytes + 1) == 0x33);

    uint8_t returned[3] = {};
    REQUIRE(context.readGuest(0x8007FF, returned, sizeof returned));
    CHECK(returned[0] == 0x11);
    CHECK(returned[1] == 0x22);
    CHECK(returned[2] == 0x33);
    CHECK(AssistContext::guestOffset(0x80FFFF, 1) == 0x800000);
    CHECK(AssistContext::guestOffset(0x7FFFFF, 1) == 0x000000);
}

TEST_CASE("extended-control-storage assist context updates saved MSP state")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);

    context.setRequestByte(RequestBlock::kOffPsr, 0x04);
    context.setRequestHalf(RequestBlock::kOffIar, 0x4567);
    context.setRequestAddress(RequestBlock::kOffProgramBlock, 0x812345);
    context.setRequestWorkRegister(5, 0xA55A);
    context.setTaskByte(4, 0x08);
    context.setTaskHalf(7, 0xC700);
    context.setTaskAddress(65, request);

    CHECK(context.requestByte(RequestBlock::kOffPsr) == 0x04);
    CHECK(context.requestHalf(RequestBlock::kOffIar) == 0x4567);
    CHECK(context.requestAddress(RequestBlock::kOffProgramBlock) == 0x812345);
    CHECK(context.requestWorkRegister(5) == 0xA55A);
    CHECK(context.taskByte(4) == 0x08);
    CHECK(context.taskHalf(7) == 0xC700);
    CHECK(context.taskAddress(65) == request);
}

TEST_CASE("language assist stubs are callable and identify their ABI")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;

    BasicAssist basic;
    AssistContext basicContext(state, trace, 0x0F00, 0x1000, 0x02, 0x00, 0x3FCA);
    const AssistResult basicResult = basic.execute(basicContext);
    CHECK(basicResult.status == AssistStatus::NotImplemented);
    CHECK(basicResult.detail.find("NuBasic") != std::string::npos);
    CHECK(std::string(basic.name()) == "NuBasic");

    FortranAssist fortran;
    AssistContext fortranContext(state, trace, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    const AssistResult fortranResult = fortran.execute(fortranContext);
    CHECK(fortranResult.status == AssistStatus::NotImplemented);
    CHECK(fortranResult.detail.find("NuFortran") != std::string::npos);
    CHECK(fortranResult.detail.find("01,05") != std::string::npos);
    CHECK(std::string(fortran.name()) == "NuFortran");
}
