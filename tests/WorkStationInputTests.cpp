#include <doctest/doctest.h>

#include "Machine/MachineState.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Processors/ControlStorage/TaskBlock.h"

using namespace sim36;
using namespace sim36::processors::controlstorage;

TEST_CASE("WSU Read Input Fields uses the display-format field-data displacement")
{
    machine::MachineState m(1024 * 1024);
    constexpr int task = 0x1000;
    constexpr int work = 0x2000;
    m.writeHalf(task, TaskBlock::kEyecatcher);
    m.writeAddr24(task + TaskBlock::kOffWorkBase, work);

    // CREATE DOCUMENT: descriptor value 0x21 means a 0x20-byte leading area.
    m.writeByte(work + 0x17, 0x21);
    CHECK(workStationInputFieldDataOffset(m, task, true) == 0x20);

    // WORK WITH DOCUMENTS carries one more leading indicator byte.
    m.writeByte(work + 0x17, 0x22);
    CHECK(workStationInputFieldDataOffset(m, task, true) == 0x21);

    // Sign-on and MAIN use the direct SSP path, regardless of stale WSU data
    // in the task work area.
    CHECK(workStationInputFieldDataOffset(m, task, false) == 0);
}
