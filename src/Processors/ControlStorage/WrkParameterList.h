// The SVC 35 WRK parameter list, addressed by XR1.  SA21-9436 3-130 defers
// the layout to System Data Areas and then prints a worked example on 3-131
// that gives all twelve bytes:
//
//   805000 = 02 81 001000 00 0002 00 000000
//   "Issuing this SVC unconditionally creates a 2 page (4096 byte) task
//    work space for the task with a task ID of hex 0002.  The storage block
//    address is returned in index register one."
//
//   +0     command: 1 conditional create, 2 unconditional create, 3 delete
//   +1     the work space type; over hex 7F it is a TASK work space and
//          +6..7 names the task, at or under 7F a SYSTEM one
//   +2..4  the size in bytes; (size + 2047) >> 11 is the page count
//   +5     flags, selecting the block form
//   +6..7  the task id
//   +8     not read
//   +9..11 an anchor; zero selects the ordinary type search
#pragma once

#include <cstdint>

namespace sim36::processors::controlstorage {

struct WrkParameterList {
    static constexpr int kBytes = 12;

    static constexpr int kOffCommand = 0;
    static constexpr int kOffType = 1;
    static constexpr int kOffSizeBytes = 2;
    static constexpr int kOffFlags = 5;
    static constexpr int kOffTaskId = 6;
    static constexpr int kOffAnchor = 9;

    static constexpr uint8_t kConditionalCreate = 1;
    static constexpr uint8_t kUnconditionalCreate = 2;
    static constexpr uint8_t kDelete = 3;

    // Over hex 7F is a task work space; the manual's own example type, hex
    // 81, takes that arm.
    static constexpr uint8_t kTaskWorkSpaceFloor = 0x80;
};

}  // namespace sim36::processors::controlstorage
