#include <doctest/doctest.h>

#include <algorithm>
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

namespace {

void seedBasicExit(machine::MachineState& state, int request, int control = 0x2000)
{
    state.writeByte(request + RequestBlock::kOffXr1High, 0);
    state.writeHalf(request + RequestBlock::kOffXr1Low, static_cast<uint16_t>(control));
    state.writeHalf(control + 19, 0x3000);
    state.writeHalf(control + 23, 0x4000);
    state.writeHalf(control + 25, 0x4000);
    state.writeHalf(control + 27, 0x4100);
    state.writeHalf(control + 29, 0x4200);
    state.writeHalf(control + 31, 0x4200);
    state.writeHalf(control + 33, 0x42FF);
    state.writeHalf(control + 35, 0x2222);
    state.writeByte(0x3000, 0x20);
}

}  // namespace

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

TEST_CASE("FORTRAN assist stub remains callable and identifies its ABI")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;

    FortranAssist fortran;
    AssistContext fortranContext(state, trace, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    const AssistResult fortranResult = fortran.execute(fortranContext);
    CHECK(fortranResult.status == AssistStatus::NotImplemented);
    CHECK(fortranResult.detail.find("NuFortran") != std::string::npos);
    CHECK(fortranResult.detail.find("01,05") != std::string::npos);
    CHECK(std::string(fortran.name()) == "NuFortran");
}

TEST_CASE("NuBasic decodes class operands, advances its IP, and performs decimal arithmetic")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int task = 0x0F00;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    state.writeByte(task + 4, 0x0F);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);

    // Six-byte class 0, direct load 1, direct add 2, direct store/pop, exit 20.
    const uint8_t program[] = {
        0x00, 0, 0, 0, 0, 0,
        0x91, 0x31, 0x00,
        0x94, 0x31, 0x05,
        0x93, 0x31, 0x0A,
        0x20,
    };
    state.write(0x3000, program, sizeof program);
    const uint8_t one[] = {0x41, 0x01, 0, 0, 0};
    const uint8_t two[] = {0x41, 0x02, 0, 0, 0};
    state.write(0x3100, one, sizeof one);
    state.write(0x3105, two, sizeof two);

    BasicAssist basic;
    AssistContext context(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    const AssistResult result = basic.execute(context);
    REQUIRE(result.status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3010);
    CHECK(state.readHalf(control + 25) == 0x4000);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x2222);
    CHECK(state.readByte(task + 4) == 0x05);
    CHECK(state.readByte(0x310A) == 0x41);
    CHECK(state.readByte(0x310B) == 0x03);
    CHECK(state.readByte(0x310C) == 0);
}

TEST_CASE("NuBasic class 31 pushes a guest control record and selects its packed target")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    state.writeHalf(control + 25, 0x40F0);
    state.writeHalf(control + 31, 0x4200);
    state.writeHalf(control + 35, 0x2468);
    const uint8_t descriptor[] = {0x31, 0xC0, 0x30, 0x00, 0, 0, 0x11, 0x80, 0x31, 0x00};
    state.write(0x3000, descriptor, sizeof descriptor);
    state.writeByte(0x3100, 0x20);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    const AssistResult result = basic.execute(context);
    REQUIRE(result.status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3101);
    CHECK(state.readHalf(control + 25) == 0x40F0);
    CHECK(state.readHalf(control + 31) == 0x4208);
    CHECK(state.readByte(0x4200) == 0xFF);
    CHECK(state.readHalf(0x4201) == 0x3001);
    CHECK(state.readHalf(0x4204) == 0x0011);
    CHECK(state.readHalf(0x4206) == 0x40F0);
}

TEST_CASE("NuBasic comparison table uses the SSP relational opcode order")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    const uint8_t program[] = {
        0x91, 0x31, 0x00, 0x98, 0x31, 0x05, // 1 < 2
        0x91, 0x31, 0x05, 0x99, 0x31, 0x05, // 2 <= 2
        0x91, 0x31, 0x05, 0x9A, 0x31, 0x00, // 2 > 1
        0x91, 0x31, 0x00, 0x9B, 0x31, 0x00, // 1 >= 1
        0x91, 0x31, 0x00, 0x9C, 0x31, 0x05, // 1 <> 2
        0x91, 0x31, 0x00, 0x9D, 0x31, 0x00, // 1 = 1
        0x20,
    };
    const uint8_t one[] = {0x41, 1, 0, 0, 0};
    const uint8_t two[] = {0x41, 2, 0, 0, 0};
    state.write(0x3000, program, sizeof program);
    state.write(0x3100, one, sizeof one);
    state.write(0x3105, two, sizeof two);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 25) == 0x4006);
    for (int i = 0; i < 6; ++i) CHECK(state.readByte(0x4000 + i) == 1);
}

TEST_CASE("NuBasic cached and unresolved transfers preserve resumable state")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    BasicAssist basic;

    seedBasicExit(state, request, control);
    const uint8_t cached[] = {0x30, 0x80, 0x31, 0x00, 0, 0, 5};
    state.write(0x3000, cached, sizeof cached);
    state.writeByte(0x3100, 0x20);
    AssistContext cachedContext(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(cachedContext).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3101);

    seedBasicExit(state, request, control);
    state.writeHalf(control + 67, 0x3456);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);
    const uint8_t unresolved[] = {0x30, 0x10, 0, 0, 0, 0, 0};
    state.write(0x3000, unresolved, sizeof unresolved);
    AssistContext unresolvedContext(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(unresolvedContext).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3001);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x3456);
}

TEST_CASE("NuBasic tight loops yield at an interruptible XFER burst boundary")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int task = 0x0F00;
    constexpr int control = 0x2000;
    constexpr uint16_t xfer = 0x3FCA;
    seedBasicExit(state, request, control);
    state.writeByte(task + 4, 0x0F);
    state.writeHalf(request + RequestBlock::kOffIar, 0x3FCD);

    // Opcode 30's cached target is itself, so no ordinary BASIC exit can
    // provide the appliance event pump with a scheduling boundary.
    const uint8_t loop[] = {0x30, 0x80, 0x30, 0x00};
    state.write(0x3000, loop, sizeof loop);

    BasicAssist basic;
    AssistContext context(state, trace, task, request, 0x02, 0x00, xfer);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3000);
    CHECK(state.readHalf(control + 25) == 0x4000);
    CHECK(state.readHalf(control + 31) == 0x4200);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == xfer);
    CHECK(state.readByte(task + 4) == 0x05);
}

TEST_CASE("NuBasic control return uses the displacement-mode unresolved continuation")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    state.writeHalf(control + 31, 0x4208);
    state.writeHalf(control + 67, 0x1111);
    state.writeHalf(control + 69, 0x2222);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);
    // Non-FF records resolve through their own key.  Bit 0x10 forces the
    // unresolved guest path; +4 must be nonzero for opcode 38 to attempt it.
    const uint8_t record[] = {0x19, 0, 0, 0, 0, 1, 0, 0};
    state.write(0x4200, record, sizeof record);
    state.writeByte(0x3000, 0x38);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x4200);
    CHECK(state.readHalf(control + 31) == 0x4200);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x2222);
}

TEST_CASE("NuBasic FOR control selects its active transfer and keeps gate handling separate")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    const uint8_t program[] = {
        0x36, 0x31, 0x00, 0x32, 0x00,
        0x80, 0x31, 0x50, 0,
        0x80, 0x31, 0x60, 0,
    };
    const uint8_t one[] = {0x41, 1, 0, 0, 0};
    state.write(0x3000, program, sizeof program);
    state.write(0x3100, one, sizeof one);
    state.writeByte(0x3200, 1); // Explicit positive direction.
    const uint8_t limit[] = {0x41, 3, 0, 0, 0};
    state.write(0x3201, limit, sizeof limit);
    state.writeByte(0x3150, 0x20);
    state.writeByte(0x3160, 0x20);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readByte(0x3200) == 0x81);
    CHECK(state.readHalf(control + 19) == 0x3151);

    seedBasicExit(state, request, control);
    const uint8_t gateProgram[] = {0x37, 0x32, 0x50, 0x80, 0x31, 0x50, 0};
    state.write(0x3000, gateProgram, sizeof gateProgram);
    state.writeByte(0x3250, 0x80);
    AssistContext gateContext(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(gateContext).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3151);
}

TEST_CASE("NuBasic routes stack faults and rejects invalid opcodes without stale state")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int task = 0x0F00;
    constexpr int control = 0x2000;
    BasicAssist basic;

    seedBasicExit(state, request, control);
    state.writeHalf(control + 71, 0x5555);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);
    const uint8_t emptyStore[] = {0x93, 0x31, 0x00};
    state.write(0x3000, emptyStore, sizeof emptyStore);
    AssistContext stackContext(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(stackContext).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 19) == 0x3003);
    CHECK(state.readHalf(control + 25) == 0x4000);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x5555);

    seedBasicExit(state, request, control);
    state.writeByte(task + 4, 0x0F);
    state.writeByte(0x3000, 0x40);
    AssistContext invalidContext(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    const AssistResult invalid = basic.execute(invalidContext);
    CHECK(invalid.status == AssistStatus::GuestError);
    CHECK(invalid.errorCode == 61);
    CHECK(state.readHalf(control + 19) == 0x3001);
    CHECK(state.readHalf(control + 25) == 0x4000);
    CHECK(state.readByte(task + 4) == 0x05);
}

TEST_CASE("NuBasic expands, concatenates, assigns, measures, and compares guest strings")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    state.writeByte(control + 115, 0x40); // EBCDIC blank padding.

    // Expand "AB", concatenate "C", assign the result, then compare it with
    // "ABC" and compute its numeric length.  D-class operands name the byte
    // immediately before the persistent [length, characters] string.
    const uint8_t program[] = {
        0xD1, 0x31, 0x00,
        0xD6, 0x31, 0x10,
        0xD3, 0x31, 0x20,
        0xD1, 0x31, 0x20,
        0xDD, 0x31, 0x30,
        0xD7, 0x31, 0x20,
        0x20,
    };
    state.write(0x3000, program, sizeof program);
    const uint8_t ab[] = {8, 2, 0xC1, 0xC2};
    const uint8_t c[] = {8, 1, 0xC3};
    const uint8_t destination[] = {8, 0};
    const uint8_t abc[] = {8, 3, 0xC1, 0xC2, 0xC3};
    state.write(0x3100, ab, sizeof ab);
    state.write(0x3110, c, sizeof c);
    state.write(0x3120, destination, sizeof destination);
    state.write(0x3130, abc, sizeof abc);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readByte(0x3120) == 8); // Capacity metadata is preserved.
    CHECK(state.readByte(0x3121) == 3);
    CHECK(state.readByte(0x3122) == 0xC1);
    CHECK(state.readByte(0x3123) == 0xC2);
    CHECK(state.readByte(0x3124) == 0xC3);
    CHECK(state.readByte(0x4000) == 1); // Equality result.
    CHECK(state.readByte(0x4001) == 0x41);
    CHECK(state.readByte(0x4002) == 3); // Guest decimal 3.
    CHECK(state.readHalf(control + 25) == 0x4006);
}

TEST_CASE("NuBasic negate pushes a value without changing its source and integer conversion rounds")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    const uint8_t program[] = {0x9E, 0x31, 0x00, 0x9F, 0x31, 0x05, 0x20};
    const uint8_t one[] = {0x41, 1, 0, 0, 0};
    const uint8_t rounded[] = {0x41, 12, 50, 0, 0};
    state.write(0x3000, program, sizeof program);
    state.write(0x3100, one, sizeof one);
    state.write(0x3105, rounded, sizeof rounded);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readByte(0x3100) == 0x41);
    CHECK(state.readByte(0x4000) == 0xC1);
    CHECK(state.readHalf(0x4005) == 13);
    CHECK(state.readHalf(control + 25) == 0x4007);
}

TEST_CASE("NuBasic opcode 52 truncates its numeric stack value without changing SP")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    seedBasicExit(state, request, control);
    const uint8_t program[] = {0x91, 0x31, 0x00, 0x52, 0x20};
    const uint8_t onePointTwoThree[] = {0x41, 1, 23, 0, 0};
    state.write(0x3000, program, sizeof program);
    state.write(0x3100, onePointTwoThree, sizeof onePointTwoThree);

    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readHalf(control + 25) == 0x4005);
    CHECK(state.readByte(0x4000) == 0x41);
    CHECK(state.readByte(0x4001) == 1);
    CHECK(state.readByte(0x4002) == 0);
    CHECK(state.readByte(0x4003) == 0);
    CHECK(state.readByte(0x4004) == 0);
}

TEST_CASE("NuBasic arithmetic conditions obey substitution and guest-continuation modes")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int control = 0x2000;
    const uint8_t program[] = {0x91, 0x31, 0x00, 0x97, 0x31, 0x05, 0x20};
    const uint8_t one[] = {0x41, 1, 0, 0, 0};
    const uint8_t zero[] = {0, 0, 0, 0, 0};

    seedBasicExit(state, request, control);
    state.writeByte(control + 3, 1); // Substitute and continue.
    state.write(0x3000, program, sizeof program);
    state.write(0x3100, one, sizeof one);
    state.write(0x3105, zero, sizeof zero);
    BasicAssist basic;
    AssistContext context(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readByte(0x4000) == 0x7F);
    for (int i = 1; i < 5; ++i) CHECK(state.readByte(0x4000 + i) == 99);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x2222);

    seedBasicExit(state, request, control);
    state.writeByte(control + 3, 2); // Preserve the old result and take the vector.
    state.writeHalf(control + 91, 0x6789);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);
    state.write(0x3000, program, sizeof program);
    AssistContext vectorContext(state, trace, 0x0F00, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(vectorContext).status == AssistStatus::Completed);
    CHECK(state.readByte(0x4000) == 0x41);
    CHECK(state.readByte(0x4001) == 1);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x6789);
}

TEST_CASE("NuBasic numeric array modes round indices and apply descriptor strides")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    constexpr int request = 0x1000;
    constexpr int task = 0x0F00;
    constexpr int control = 0x2000;
    const uint8_t one[] = {0x41, 1, 0, 0, 0};
    const uint8_t two[] = {0x41, 2, 0, 0, 0};
    const uint8_t value[] = {0x41, 42, 0, 0, 0};
    state.writeHalf(0x3200, 0x3300);
    state.writeHalf(0x3202, 15);
    state.writeHalf(0x3204, 3);

    seedBasicExit(state, request, control);
    state.writeHalf(control + 25, 0x4005);
    state.write(0x4000, two, sizeof two);
    state.write(0x330A, value, sizeof value); // base + W*2
    const uint8_t oneDimensional[] = {0xA1, 0x32, 0x00, 0x20};
    state.write(0x3000, oneDimensional, sizeof oneDimensional);
    BasicAssist basic;
    AssistContext context(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context).status == AssistStatus::Completed);
    CHECK(state.readByte(0x4000) == 0x41);
    CHECK(state.readByte(0x4001) == 42);

    seedBasicExit(state, request, control);
    state.writeHalf(control + 25, 0x400A);
    state.write(0x4000, two, sizeof two); // j is below i because i is popped first.
    state.write(0x4005, one, sizeof one);
    state.write(0x3314, value, sizeof value); // base + W*1 + (2-LB)*15
    const uint8_t twoDimensional[] = {0xB1, 0x32, 0x00, 0x20};
    state.write(0x3000, twoDimensional, sizeof twoDimensional);
    AssistContext context2(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(context2).status == AssistStatus::Completed);
    CHECK(state.readByte(0x4000) == 0x41);
    CHECK(state.readByte(0x4001) == 42);

    seedBasicExit(state, request, control);
    state.writeHalf(control + 25, 0x4005);
    state.write(0x4000, value, sizeof value); // 42 is outside first extent.
    state.writeHalf(task + 83, 0x6A6A);
    state.writeHalf(request + RequestBlock::kOffArr, 0x4567);
    state.write(0x3000, oneDimensional, sizeof oneDimensional);
    AssistContext rangeContext(state, trace, task, request, 0x02, 0x00, 0x3FCA);
    REQUIRE(basic.execute(rangeContext).status == AssistStatus::Completed);
    CHECK(state.readHalf(request + RequestBlock::kOffIar) == 0x4567);
    CHECK(state.readHalf(request + RequestBlock::kOffXr2Low) == 0x6A6A);
}

TEST_CASE("assist trace categories filter BASIC and FORTRAN independently")
{
    machine::MachineState state(64 * 1024);
    monitor::Tracer trace;
    BasicAssist basic;
    FortranAssist fortran;
    AssistContext basicContext(state, trace, 0x0F00, 0x1000, 0x02, 0x00, 0x3FCA);
    AssistContext fortranContext(state, trace, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    seedBasicExit(state, 0x1000);

    trace.flags.store(monitor::TraceAssistBasic | monitor::TraceDefer);
    (void)basic.execute(basicContext);
    const auto basicLines = trace.copyDeferred();
    REQUIRE(basicLines.size() >= 4);
    CHECK(std::all_of(basicLines.begin(), basicLines.end(), [](const std::string& line) {
        return line.find("asstb") == 0;
    }));
    (void)fortran.execute(fortranContext);
    CHECK(trace.copyDeferred().size() == basicLines.size());

    monitor::Tracer fortranTrace;
    fortranTrace.flags.store(monitor::TraceAssistFortran | monitor::TraceDefer);
    AssistContext fortranBasic(state, fortranTrace, 0x0F00, 0x1000, 0x02, 0x00, 0x3FCA);
    AssistContext tracedFortran(state, fortranTrace, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    seedBasicExit(state, 0x1000);
    (void)basic.execute(fortranBasic);
    CHECK(fortranTrace.lines() == 0);
    (void)fortran.execute(tracedFortran);
    REQUIRE(fortranTrace.copyDeferred().size() == 1);
    CHECK(fortranTrace.copyDeferred()[0].find("asstf") == 0);

    monitor::Tracer allTrace;
    allTrace.flags.store(monitor::TraceAll | monitor::TraceDefer);
    AssistContext allBasic(state, allTrace, 0x0F00, 0x1000, 0x02, 0x00, 0x3FCA);
    AssistContext allFortran(state, allTrace, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    seedBasicExit(state, 0x1000);
    (void)basic.execute(allBasic);
    (void)fortran.execute(allFortran);
    const auto allLines = allTrace.copyDeferred();
    CHECK(std::any_of(allLines.begin(), allLines.end(), [](const std::string& line) { return line.find("asstb") == 0; }));
    CHECK(std::any_of(allLines.begin(), allLines.end(), [](const std::string& line) { return line.find("asstf") == 0; }));

    monitor::Tracer cspOnly;
    cspOnly.flags.store(monitor::TraceCsp | monitor::TraceDefer);
    AssistContext cspBasic(state, cspOnly, 0x0F00, 0x1000, 0x02, 0x00, 0x3FCA);
    AssistContext cspFortran(state, cspOnly, 0x0F00, 0x1000, 0x01, 0x05, 0x2000);
    (void)basic.execute(cspBasic);
    (void)fortran.execute(cspFortran);
    CHECK(cspOnly.lines() == 0);
}
