// The task block, as the task/event/queue supervisor calls address it.
//
// Accessors over live guest storage rather than a marshalled struct: SSP
// writes these bytes itself between calls, and only part of the 68-plus-byte
// layout is known, so a copy would both diverge and zero what it does not
// model.
//
// Offsets are DECIMAL.  Three-byte fields are named here by their FIRST byte,
// because that is what readAddr24 wants; the manual names the same field by
// its LAST byte, so the queue-header and chain-field arguments handed to the
// queue engine are two higher than the constants here.
#pragma once

#include <cstdint>

#include "Machine/MachineState.h"

namespace sim36::processors::controlstorage {

struct TaskBlock {
    // EBCDIC "TB", validated by the task post before it will use a block.
    static constexpr uint16_t kEyecatcher = 0xE3C2;

    // Halfword TASK ID - what SVC 2B's WR5 names.  The post-by-id walk of
    // queue 39 compares this against WR5, so it is the identity SSP knows a
    // task by.  The control storage IPL gives the IPL task 0x0009.
    static constexpr int kOffTaskId = 2;

    // State flags: 0x80 waiting.  A task post clears 0x90 when it makes a
    // task runnable; a general post clears 0x04 when 0x08 is on.
    static constexpr int kOffState = 4;

    // TB_STAT2 - the task's wait conditions, and the byte a task post has to
    // drive to zero.  SA21-9436 3-102: "Inline parameter 1 is read into
    // TB_STAT2.  The task remains in the wait state until all the bits in
    // TB_STAT2 are set off by task post (supervisor call 1D)."  It is tb+5,
    // not tb+6: every wait ORs its wait code into tb+5, the task post clears
    // the posted bits from tb+5 and readies the task only when that byte
    // reaches zero, and SA21-9436 3-112's SVC 25 tests "task block status
    // byte 2 (TB_STAT2)" for an event wait, which is bit 0x80 here.
    static constexpr int kOffStat2 = 5;

    // Deferred wait conditions.  SVC 1D clears the posted bits from here
    // before it clears them from TB_STAT2, and the wait tail folds a non-zero
    // value into TB_STAT2 the next time the task waits.  SVC 17 is what puts a
    // value here: a wait asked for asynchronously that cannot be granted now
    // is parked in the target's tb+6.  A value of 0xFF sends the next wait
    // into task termination instead.
    static constexpr int kOffDeferredWait = 6;

    // Dispatching priority, the sort key both task queues order on.  SVC 24
    // computes it; the IPL sets 0xFC.
    static constexpr int kOffPriority = 7;

    // TB_WMASK, 2 bytes - the general wait mask SVC 00 stores and SVC 01 posts
    // against.  Sixteen bits for the manual's "16 separate conditions".
    static constexpr int kOffGeneralWaitMask = 8;

    // Lock byte 1.  SVC 23 Q bit 2 sets the WR5 bits here; SVC 01 Q bit 2
    // clears them.
    static constexpr int kOffLockByte1 = 11;

    // Set to 250 when a task takes a long wait.
    static constexpr int kOffLongWaitMark = 13;

    // 3 bytes at +17..19: the ACE through which an asynchronously attached
    // task returns control to its creator.  The same field is the head of the
    // ACE chain task termination scans in every task.
    static constexpr int kOffReturnAce = 17;

    // Priority floor - the priority may not be lowered past it.  SVC 24
    // stores its requested priority here as well as at +28.
    static constexpr int kOffPriorityFloor = 20;

    // 3 bytes ending at 23: the JOB CONTROL BLOCK this task belongs to.  Four
    // independent readers agree: the region get, the task work area access,
    // the translated-assign heap and the resource enqueue's queue-by-JCB arm
    // (SA21-9436 3-107, Q bit 3), which takes the owner from here when the
    // Q-byte did not pass a JCB in XR1.
    static constexpr int kOffJobControlBlock = 21;

    // Termination state byte.  Task termination sets bit 0x80 on entry and
    // its queue-39 dependency scan excludes tasks which already have it set.
    static constexpr int kOffTerminationState = 24;
    static constexpr uint8_t kTerminationActive = 0x80;

    // 3 bytes ending at 27: the chain link for queue 39, the task priority
    // queue.
    static constexpr int kOffQueue39Link = 25;

    // The priority SVC 24 was asked for, before the bias is applied.
    static constexpr int kOffPriorityRequested = 28;

    // 3 bytes ending at 35: the chain link for queue 40, the ready list.
    static constexpr int kOffQueue40Link = 33;

    // 3 bytes ending at 47: TB_CMPLQ, the head of this task's COMPLETE EVENT
    // QUEUE.  An event post queues a posted action control element here and
    // the event wait walks it looking for one that satisfies a wait.
    static constexpr int kOffCompleteQueue = 45;

    // 3 bytes ending at 51: the system measurement block SVC 08 increments
    // its counters in, biased by 64.  Zero means no measurement block, which
    // is the state at IPL.
    static constexpr int kOffMeasurementBlock = 49;

    // 3 bytes ending at 55: the head of this task's ALLOCATION QUEUE ELEMENT
    // queue - the task-block counterpart of the job control block's JCBDAQEQ
    // at +211..213.  The resource enqueue's queue-by-TB arm queues the new
    // element on owner+55 with chain displacement 11.  The System/34 queues
    // allocation elements by TCB only, which is why its AQE has an owner
    // field and its JCB has no queue at all.
    static constexpr int kOffAqeQueue = 53;

    // 3 bytes: the pb a transfer is in flight to.
    static constexpr int kOffTransferInterlock = 57;

    // TB_STAT - the byte carrying the ABNORMAL TERMINATION bit, mask 0x10.
    // SA21-9436 3-109 says SVC 22 sets it, unless the MIC is one that does
    // not abend; it is read back to detect a RECURSIVE abend - a second
    // SVC 22 on a task already terminating goes straight to the abort path.
    // The System/34 main-storage TCB carries the same pair of flags at
    // TCBSTAT4 - "10 Task is in abnormal termination", "08 Recursive
    // termination due to SVC 22" (LY21-0049 figure 2-257 part 9).
    static constexpr int kOffStatus = 32;

    // Abnormal termination, tb+32 bit 0x10 (IBM bit 3).
    static constexpr uint8_t kStatusAbnormalTermination = 0x10;

    // SVC 22 has assigned its 32-byte native dump/termination context, tb+32
    // bit 0x01, set immediately after that assign succeeds and before
    // entering #FETDP.  The name is structural: the downstream guest meaning
    // of the bit is not assumed here.
    static constexpr uint8_t kStatusNuabContextAssigned = 0x01;

    // Required by task termination before an ACE targeting a dying task can
    // make this task a termination dependent.  New tasks start with this bit
    // in the task attach's literal status 0x60.
    static constexpr uint8_t kStatusTerminationScanEligible = 0x40;

    // Task termination could not enter CTEI in this dependent immediately;
    // its next wait must honour tb+6 = FF and terminate it.
    static constexpr uint8_t kStatusDeferredTermination = 0x04;

    // Byte whose bit 0x01 task termination sets on a task selected by its
    // dependency scan.  Other bits remain unidentified.
    static constexpr int kOffTerminationDependencyFlags = 48;
    static constexpr uint8_t kTerminationDependencySelected = 0x01;

    // Task termination entry depth/state counter.  It is incremented on every
    // entry; nonzero selects immediate CTEI when the suspend accepts the task.
    static constexpr int kOffTerminationDepth = 52;

    // A second task termination entry counter at tb+60, advanced beside
    // tb+52.  Its later meaning is not yet decoded.
    static constexpr int kOffTerminationPass = 60;

    // Halfword tested by task termination.  A nonzero value makes it wait the
    // task (wait code 8), set status bit 0x80 and return before freeing the
    // task allocation or entering slot 4.  The producer/meaning of the count
    // is not yet decoded; the name is structural rather than a guess.
    static constexpr int kOffTerminationWait130 = 130;

    // Halfword tested by task termination.  Like +130, nonzero defers the
    // remainder of termination through a wait (code 8) and status bit 0x80.
    // Its producer/meaning remains unresolved.
    static constexpr int kOffTerminationWait150 = 150;

    // The MIC field - a halfword at tb+62, holding the DECIMAL MIC.
    // SA21-9436 3-109: "The message identification code (MIC) is stored in
    // the task block."  What lands here is the decimal form: the raw MIC is
    // divided by 1000/100/10 and the digits packed first, so MIC hex 1F is
    // stored - and displayed - as 0031.  The same halfword goes to guest
    // 0x0800.  SVC 22 also reads a 3-byte address out of tb+61..63, which
    // OVERLAPS this halfword, but only when tb+40 is non-zero; when that
    // address is non-zero the MIC goes to that block's +10 instead.  tb+62 is
    // where the MIC goes when there is no such block, which is the IPL task's
    // case.
    static constexpr int kOffMic = 62;

    // Priority bias: the requeue adds four times this to the requested
    // priority before capping at 236.
    static constexpr int kOffPriorityBias = 64;

    // 3 bytes: guest address of the task's request block.
    static constexpr int kOffRequestBlock = 65;

    // tb+69..71 - the base of the current task's SCRATCH AREA, recomputed on
    // every transfer and every exit, and copied into queue header 38 by the
    // dispatcher and the exit path.  It is not a queue head, which is why
    // nothing ever queued anything on 38.  #SVTUB reads nine bytes through
    // it - +0..2 a unit block, +3..5 a configuration record, +6..8 a 32-byte
    // block.
    static constexpr int kOffWorkBase = 69;

    // ---- the LAST-byte names, which are what the queue engine is passed ----

    // Queue 39's chain field as the requeue names it: byte 27.
    static constexpr int kChainLastQueue39 = kOffQueue39Link + 2;

    // Queue 40's chain field as the requeue names it: byte 35.
    static constexpr int kChainLastQueue40 = kOffQueue40Link + 2;

    // The complete queue's header as the event post and wait name it: tb+47,
    // the last of the three bytes at 45..47.
    static constexpr int kCompleteQueueLast = kOffCompleteQueue + 2;

    static bool isTaskBlock(machine::MachineState& m, int tb)
    {
        return tb != 0 && tb + 2 <= m.backingBytes() && m.readHalf(tb) == kEyecatcher;
    }

    // Is the task in a wait state?  Wait/post callers use this for policy,
    // but the task post itself has no such gate: once TB_STAT2 reaches zero
    // it executes its ready-state stores even for the currently-running task.
    static bool isWaiting(machine::MachineState& m, int tb)
    {
        return (m.readByte(tb + kOffState) & 0x80) != 0;
    }
};

static_assert(TaskBlock::kChainLastQueue39 == 27, "queue 39 chain field ends at tb+27");
static_assert(TaskBlock::kChainLastQueue40 == 35, "queue 40 chain field ends at tb+35");
static_assert(TaskBlock::kCompleteQueueLast == 47, "complete queue head ends at tb+47");

}  // namespace sim36::processors::controlstorage
