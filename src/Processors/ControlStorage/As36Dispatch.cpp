// The Advanced/36 control storage processor: the task dispatcher and the
// supervisor calls that could not be built without it.
//
// SVC 00 General Wait, 17 Asynchronous Task Wait, 1E Task Wait, 25
// Asynchronous Task Ready Check, 30 QLOCK, and the blocking arms of 01, 02,
// 03/19/2B, 1D and 23; the queue scan (1B), test and set (23), task priority
// (24), the general post (01), the action controller (0B), the event
// counters (08), the trace log (1A), the resource family (20, 21) and the
// time of day (2E).
//
// TB_STAT2 is tb+5: the wait conditions live there, and tb+6 holds the
// deferred conditions an asynchronous wait folds in later.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>

#include <fmt/format.h>

#include "Devices/WorkStationIob.h"
#include "Processors/ControlStorage/AllocationQueueElement.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/SvcTable.h"
#include "Processors/ControlStorage/TaskBlock.h"

namespace sim36::processors::controlstorage {

// =============================================================================
// The two queue primitives
// =============================================================================

// The ready list, keyed on tb+7: system queue 40, chain field ending at
// tb+35, with the caller's flags byte (readyQueueInsert is in the M4 core).
// Remove is the same call with flags 0x60, system request plus dequeue:
// the whole of "stop running this task".
void As36ControlStorageProcessor::readyQueueRemove(int tb)
{
    queueOperation(GuestLowStorage::queueHeader(kTaskReadyQueue), tb, TaskBlock::kChainLastQueue40, 0x60);
}

// =============================================================================
// Put the calling task into a wait
// =============================================================================

// The routine every wait call tail-jumps to: tb+4 |= 0x80, tb+5 |= the
// wait code, and a deferred condition byte at tb+6 is folded in only for a
// task that is not the current one (the current task is never suspendable
// from outside).  Then the redispatch request is set and the task leaves
// the ready list.  A call that is not a general wait and carries Q bit 1
// also takes the long-wait priority arm.
void As36ControlStorageProcessor::taskWaitReturn(int tb, uint8_t waitCode, uint8_t rByte, uint8_t q,
                                                  const std::string& call)
{
    constexpr uint8_t kQLongWait = 0x40;   // Q bit 1

    uint8_t state = m_.readByte(tb + TaskBlock::kOffState);
    m_.writeByte(tb + TaskBlock::kOffState, static_cast<uint8_t>(state | kTbStateWaiting));

    uint8_t stat2 = static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffStat2) | waitCode);
    m_.writeByte(tb + TaskBlock::kOffStat2, stat2);

    trace_.csp("{}: nuwartn - task block {:04X} waits on TB_STAT2 (tb+5) = {:02X}, tb+4 = {:02X}", call, tb, stat2,
               m_.readByte(tb + TaskBlock::kOffState));

    uint8_t deferred = m_.readByte(tb + TaskBlock::kOffDeferredWait);
    if (deferred != 0 && tb != currentTaskBlock_) {
        // Only reachable from SVC 17, and only for a task the suspendability
        // gauntlet admits.  0xFF means terminate.
        m_.writeByte(tb + TaskBlock::kOffDeferredWait, 0);
        if (deferred == 0xFF) {
            trace_.csp("{}: task block {:04X} carries tb+6 = FF, which nuwartn turns into NuEmul::nupterm(tb, rb) at "
                       "c18b4dc0 - task termination is not modelled (docs/s36/task-dispatcher.md)",
                       call, tb);
        } else {
            m_.writeByte(tb + TaskBlock::kOffStat2, static_cast<uint8_t>(stat2 | deferred));
            trace_.csp("{}: task block {:04X} had deferred conditions {:02X} at tb+6; nuwartn folds them into TB_STAT2 "
                       "(c18b4de0-c18b4de8)",
                       call, tb, deferred);
        }
    } else if (deferred != 0) {
        trace_.csp("{}: task block {:04X} has tb+6 = {:02X}, but nuwasusp refuses the CURRENT task at c18b5100, so "
                   "nuwartn takes the ordinary path",
                   call, tb, deferred);
    }

    redispatch_ = true;
    readyQueueRemove(tb);

    // "R != 0" is literally "this is not a general wait": the general wait
    // stores 0 in the R-byte slot itself, and SVC 00 IS R-byte 00.
    if (rByte != 0x00) {
        if ((q & kQLongWait) == 0) {
            trace_.csp("{}: Q bit 1 is off, so nuwartn skips both nuprqvlw and nucsbprt (c18b4e9c -> c18b4ed0)", call);
            return;
        }
        m_.writeByte(tb + TaskBlock::kOffState, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffState) | kTbStateLongWait));
    }

    if ((m_.readByte(tb + TaskBlock::kOffState) & kTbStateLongWait) != 0) longWaitPriority(tb);

    // The routine then calls a control-storage block routine unconditionally
    // on this arm; nothing in this corpus says what it does.
    trace_.csp("{}: nuwartn would now call NuEmul::nucsbprt(tb, 0) at c18b4ec8, which is not decoded", call);
}

// =============================================================================
// Make a task ready again
// =============================================================================

// The tail of the task post after the mask clearing: tb+4 &= ~0x90, rb+47
// = 0xFF, a priority insert on the ready list, and the redispatch request.
// The priority arm (0xC7) is used rather than the LIFO-plus-priority arm the
// machine selects for one Q-byte value this corpus cannot explain; both
// SA21-9436 3-71 and the dispatcher's own walk require the list ordered.
void As36ControlStorageProcessor::readyTask(int tb, const std::string& call)
{
    constexpr uint8_t kReadyFlagsPriority = 0xC7;

    uint8_t state = m_.readByte(tb + TaskBlock::kOffState);
    m_.writeByte(tb + TaskBlock::kOffState, static_cast<uint8_t>(state & ~(kTbStateWaiting | kTbStateLongWait)));

    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (rb != 0) m_.writeByte(rb + kRbTransientMark, 0xFF);

    readyQueueInsert(tb, kReadyFlagsPriority);
    redispatch_ = true;

    trace_.csp("{}: nupotb readies task block {:04X} - tb+4 {:02X} -> {:02X}, rb+47 = FF, queued on the ready list "
               "(queue 40) by priority {:02X}",
               call, tb, state, m_.readByte(tb + TaskBlock::kOffState), m_.readByte(tb + TaskBlock::kOffPriority));
}

// =============================================================================
// Choose the next task and give it the processor
// =============================================================================

// Selection.  With dispatching disabled the machine keeps running the task
// queue header 37 names; enabled, the head of ready queue 40, ordered by
// tb+7 descending (SA21-9436 3-71, 3-73).  The anti-starvation rotation
// the machine applies from a host table is not modelled: taking the head
// is what the manual specifies.  The dispatching switch is PMR bit 0, the
// control SA21-9436 3-33 gives the guest; bit set means enabled, which
// makes a reset machine disabled and unable to switch until SSP asks.
int As36ControlStorageProcessor::selectNextTask(const std::string& call)
{
    if (!m_.msp.taskDispatchingEnabled()) {
        int held = m_.readAddr24(GuestLowStorage::queueHeader(kDispatchedTaskQueue));
        if (held == 0 || (m_.readByte(held + TaskBlock::kOffState) & kTbStateWaiting) != 0) {
            trace_.csp("{}: task dispatching is disabled (PMR bit 0) and queue header 37 names {:04X}, which is {}; "
                       "nudspchA has nothing to run (c180dc5c-c180dc64)",
                       call, held, held == 0 ? "empty" : "in a wait");
            return 0;
        }
        return held;
    }

    int head = m_.readAddr24(GuestLowStorage::queueHeader(kTaskReadyQueue));
    if (head == 0) {
        trace_.csp("{}: the ready list (system queue 40) is empty - nudspchA takes its no-task exit at c180e04c", call);
        return 0;
    }
    return phaseOrderJobTaskSelection(head, call);
}

// Console sign-on phase ordering.  When the phase-2 sign-on job task is
// about to run its job-control-block-dependent continuation while phase 1
// has not yet linked the console unit block's job pointer, hold it and
// dispatch another ready task instead; once the pointer is linked, re-apply
// the job task's copy of it with the live value and complete the deferred
// queue-112 enqueue.  A read-only walk of the ready list plus the one guest
// write the sign-on job entry itself makes.
int As36ControlStorageProcessor::phaseOrderJobTaskSelection(int head, const std::string& call)
{
    int jt = phase2SvatJobTask_;
    if (jt == 0) return head;

    // The job task is gone, or already holds a JCB: nothing left to order.
    if (!TaskBlock::isTaskBlock(m_, jt) || m_.readAddr24(jt + TaskBlock::kOffJobControlBlock) != 0) {
        phase2SvatJobTask_ = 0;
        phase2SvatDispatched_ = false;
        return head;
    }

    // The job entry has not run yet: it must, to post the event that drives
    // the IPL task onward.  Holding it here deadlocks.
    if (!phase2SvatDispatched_) return head;

    int jcb = consoleTubJobPointer();

    // The reference's SignonJcbArm experiment (synthesising a JCB here) is
    // off by default and is not ported.
    if (jcb != 0) {
        m_.writeAddr24(jt + TaskBlock::kOffJobControlBlock, jcb);
        // The deferred real operation that followed the copy: a FIFO enqueue
        // of the JCB on system queue 112 through the chain ending at JCB+0x11.
        // The queue engine rejects a duplicate, so this stays safe if the
        // entry itself enqueues it later.
        bool activationQueued = queueOperation(GuestLowStorage::queueHeader(GuestLowStorage::kWorkStationActivationQueue),
                                               jcb, 0x11, 0x40);
        trace_.csp("{}: phase-ordering - #CPON linked the console TUB job pointer {:06X}; re-applied #SVAT's 0x10EB copy "
                   "into job task {:04X} tb+0x15..0x17 and its 1366 QH112 enqueue ({}) "
                   "(docs/s36/msipl-wake-ordering.md)",
                   call, jcb, jt, activationQueued ? "queued" : "already queued/refused");
        phase2SvatJobTask_ = 0;
        phase2SvatDispatched_ = false;
        return head;
    }

    // Phase 1 has not linked the JCB yet.  Defer the job task in favour of
    // any other ready task; if it is the only thing ready, let it run.
    if (head != jt) return head;
    int other = firstReadyTaskOtherThan(jt);
    if (other == 0) return head;
    trace_.csp("{}: phase-ordering - holding phase-2 job task {:04X} (#SVAT copied a null console TUB job pointer) until "
               "phase-1 #CPON links it; dispatching {:04X} instead (docs/s36/msipl-wake-ordering.md)",
               call, jt, other);
    return other;
}

// The console unit block published at guest 0x092A, when it carries the
// "TU" eyecatcher.
int As36ControlStorageProcessor::consoleUnitBlock()
{
    int ub = m_.readAddr24(GuestLowStorage::kSystemConsoleUnitBlockPointer);
    if (ub != 0 && m_.readHalf(ub) == devices::WorkStationIob::kUnitBlockEyecatcher) return ub;
    return 0;
}

int As36ControlStorageProcessor::consoleTubJobPointer()
{
    int tub = consoleUnitBlock();
    return tub == 0 ? 0 : m_.readAddr24(tub + kConsoleTubJobPointerOffset);
}

// The first ready task (queue 40, priority order) that is not `skip`.
int As36ControlStorageProcessor::firstReadyTaskOtherThan(int skip)
{
    int at = m_.readAddr24(GuestLowStorage::queueHeader(kTaskReadyQueue));
    for (int steps = 0; at != 0 && steps < 64; steps++) {
        if (at != skip && TaskBlock::isTaskBlock(m_, at)) return at;
        at = m_.readAddr24(at + TaskBlock::kOffQueue40Link);
    }
    return 0;
}

// The whole of a dispatch: select, publish the choice in queue header 37,
// and, if the winner is not the task already running, switch to it.  The
// switch is three fields: the current task block, its request block from
// tb+65..67, and header 37.  The register save area is the request block
// and a task switch moves it: inside a supervisor call the entry spill and
// closing reload are the two halves; outside one, a preemptive switch
// spills the outgoing task's live registers itself and loads the incoming
// block's.  Then tb+4 decides how to enter the task: bit 0x08 clear (or
// 0x04 set) resumes the MSP; 0x08 with 0x02 is the error-control-storage
// arm (not decoded); 0x08 alone re-issues the supervisor call.
bool As36ControlStorageProcessor::dispatch(const std::string& call)
{
    redispatch_ = false;

    int next = selectNextTask(call);
    if (next == 0) return false;
    // A task is about to run, so the machine is no longer at nudspchA's
    // no-task exit.  Every dispatch path resets this, not only the wait
    // tails: a later stop of the MSP for another reason must be reported
    // as that reason rather than mistaken for the external-event idle.
    idleEventWait_ = false;

    if (next == phase2SvatJobTask_) phase2SvatDispatched_ = true;

    m_.writeAddr24(GuestLowStorage::queueHeader(kDispatchedTaskQueue), next);

    // The resume path publishes the SELECTED task's scratch base into queue
    // header 38 as well, from tb+69..71, on every dispatch: a resumed task's
    // work-base load must read ITS OWN scratch area.
    m_.writeAddr24(GuestLowStorage::queueHeader(kTaskWorkBaseQueue), m_.readAddr24(next + TaskBlock::kOffWorkBase));

    if (next == currentTaskBlock_) return true;

    int rb = m_.readAddr24(next + TaskBlock::kOffRequestBlock);
    if (rb == 0) {
        trace_.csp("{}: task block {:04X} has no request block at tb+65..67, so nudspchA has no register save area to "
                   "dispatch through (c180dde0)",
                   call, next);
        return false;
    }

    int from = currentTaskBlock_;

    bool switchLiveRegisters = !inSupervisorCall_ && msp_ != nullptr && !msp_->stopped();
    if (switchLiveRegisters) {
        saveRegisters(currentRequestBlock_);
        trace_.csp("{}: preemptive switch {:04X} -> {:04X} with the MSP running - spilled the outgoing task's registers "
                   "to request block {:04X} (nusvc's entry spill, c18e36b8) before repointing the ATR file",
                   call, from, next, currentRequestBlock_);
    }

    currentTaskBlock_ = next;
    currentRequestBlock_ = rb;

    // SA21-9436 1-29's "A5 PATR, fast task switch for ATRs": the incoming
    // task's registers are REPOINTED, never rebuilt.
    selectTranslationFile(rb, call);

    if (switchLiveRegisters) restoreRegisters(rb);

    uint8_t state = m_.readByte(next + TaskBlock::kOffState);
    trace_.csp("{}: dispatch {:04X} -> {:04X} (rb {:04X}, priority {:02X}, tb+4 {:02X})", call, from, next, rb,
               m_.readByte(next + TaskBlock::kOffPriority), state);

    if ((state & kTbStateInSvc) != 0 && (state & kTbStateSvcComplete) == 0) {
        if ((state & kTbStateEcs) != 0) {
            trace_.csp("{}: task block {:04X} has tb+4 bit 02, which sends nudspchA to NuEmul::nuecs (c180de70) rather "
                       "than to nudspchtb; nuecs is not decoded (docs/s36/task-dispatcher.md)",
                       call, next);
            return false;
        }
        return resumeSupervisorCall(next, rb, call);
    }
    return true;
}

// A resumed task re-executes its supervisor call: the instruction address
// is backed up by the call's length (a function of the R-byte) and the call
// is issued again.  tb+4 bit 0x08 is cleared unconditionally (the machine
// clears it only if bit 0x04 is on, and nothing located writes bit 0x04;
// otherwise a task that reached here once would re-issue for ever).
bool As36ControlStorageProcessor::resumeSupervisorCall(int tb, int rb, const std::string& call)
{
    if (dispatchDepth_ >= kDispatchDepthLimit) {
        trace_.csp("{}: {} nested supervisor call re-issues; refusing to recurse further (emulator policy - IBM's "
                   "nudspchA is a loop)",
                   call, dispatchDepth_);
        return false;
    }

    uint8_t r = m_.readByte(rb + RequestBlock::kOffRByte);
    int length = SvcTable::instructionLength(r);
    uint16_t iar = m_.readHalf(rb + RequestBlock::kOffIar);
    m_.writeHalf(rb + RequestBlock::kOffIar, static_cast<uint16_t>((iar - length) & 0xFFFF));

    m_.writeByte(tb + TaskBlock::kOffState, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffState) & ~kTbStateInSvc));

    SvcRequest again;
    again.r = r;
    again.q = m_.readByte(rb + RequestBlock::kOffQByte);
    again.inline1 = m_.readByte(rb + RequestBlock::kOffInline1);
    again.inline2 = m_.readByte(rb + RequestBlock::kOffInline2);
    again.inline3 = m_.readByte(rb + RequestBlock::kOffInline3);
    again.dispatch = SvcTable::classify(r);
    again.taskBlock = tb;
    again.requestBlock = rb;

    trace_.csp("{}: nudspchtb - task block {:04X} resumes by RE-ISSUING SVC {:02X}; rb+24 {:04X} -> {:04X} ({} bytes)",
               call, tb, r, iar, (iar - length) & 0xFFFF, length);

    dispatchDepth_++;
    bool ok = service(again);
    dispatchDepth_--;
    return ok;
}

// Children built by an asynchronous transfer are readied when the creating
// task yields, which is when the machine's make-ready action runs.
void As36ControlStorageProcessor::readyPendingAsyncChildren(const std::string& call)
{
    if (pendingAsyncChildren_.empty()) return;
    std::vector<int> children = pendingAsyncChildren_;
    pendingAsyncChildren_.clear();
    for (int child : children) {
        if (!TaskBlock::isTaskBlock(m_, child)) continue;
        if ((m_.readByte(child + TaskBlock::kOffStat2) & kWaitProgramNotReady) == 0) continue;
        postTaskBlock(child, kWaitProgramNotReady, "nucready c1897760 (deferred async transfer completion)");
    }
    (void)call;
}

// What a wait call does after the wait return: hand the processor to
// somebody else.  A wait that finds nothing runnable is not an error, it is
// the dispatcher's own no-task exit, but on a machine whose devices
// complete inside the supervisor call nothing can then post the waiter, so
// the MSP is stopped with that said in as many words.
bool As36ControlStorageProcessor::waitAndDispatch(int tb, const std::string& call)
{
    // The waiter has given the processor away: any printer record accepted
    // earlier has had its time on the wire, so its operation ends now and
    // its element reaches the owner's complete queue before the dispatcher
    // looks for work.
    completePendingPrinterOutput(call);
    readyPendingAsyncChildren(call);
    if (dispatch(call)) {
        idleEventWait_ = false;
        return true;
    }

    traceActionControllerAtNoTaskExit(call);
    msp_->halt(fmt::format("{}: task block {:04X} waits on TB_STAT2 {:02X} and no task is ready - nudspchA's no-task exit "
                           "(c180e04c){}",
                           call, tb, m_.readByte(tb + TaskBlock::kOffStat2), undischargedActionsSuffix()));
    idleEventWait_ = true;
    return true;
}

// =============================================================================
// Level 5: a main-storage-program storage-protection violation
// =============================================================================

// SA21-9436 1-30: a main storage program's access to a protected page is a
// level 5 interrupt to the control processor, the same level supervisor
// calls arrive on.  The registers are not demand-paged (a program's whole
// ATR file is built at transfer time), so there is nothing to fault in: the
// task is abnormally terminated.  Modelled: the abend bit, retirement from
// both scheduler queues, the releases, and a dispatch of the rest.  Not
// modelled: the specific MIC, the task dump and the 0AFB re-entry (3-109),
// none of which a decoded source supplies.
bool As36ControlStorageProcessor::raiseStorageProtection(uint16_t logical, bool forWrite)
{
    // The reference's SignonExperiment auto-map arm is off by default and is
    // not ported.
    int tb = currentTaskBlock_, rb = currentRequestBlock_;

    saveRegisters(rb);

    trace_.csp("LEVEL 5: main-storage-program storage-protection violation - logical {:04X} ({}); task block {:04X}, "
               "request block {:04X}, IAR {:04X}, PMR {:02X}, PACT dir/xr1/xr2/iar {:02X}/{:02X}/{:02X}/{:02X} "
               "(SA21-9436 1-30, level 5 to the control processor)",
               logical, forWrite ? "write" : "read", tb, rb, m_.msp.iar, m_.msp.pmr(), m_.msp.pactDir, m_.msp.pactXr1,
               m_.msp.pactXr2, m_.msp.pactIar);

    m_.reportCheck("storage protection (level 5)", 0x0000,
                   fmt::format("logical {:04X} ({}) at IAR {:04X}, task {:04X}", logical, forWrite ? "write" : "read",
                               m_.msp.iar, tb));

    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("LEVEL 5: the dispatched block {:04X} is not a task block (no TB eyecatcher), so there is no owner to "
                   "abnormally terminate - the machine stops",
                   tb);
        return false;
    }

    uint8_t status = m_.readByte(tb + TaskBlock::kOffStatus);
    m_.writeByte(tb + TaskBlock::kOffStatus, static_cast<uint8_t>(status | TaskBlock::kStatusAbnormalTermination));

    // Retire from both scheduler queues before releasing anything the task's
    // blocks point at.
    retireTerminatingTaskFromScheduler(tb, "LEVEL 5");
    releaseTaskResources(tb, "LEVEL 5");
    releaseTaskTerminationIoQueues(tb, "LEVEL 5");
    releaseTaskWorkSpaces(tb, "LEVEL 5");
    releaseTerminatingTaskProgramState(tb, "LEVEL 5");

    redispatch_ = true;

    trace_.csp("LEVEL 5: task block {:04X} abnormally terminated (tb+32 {:02X} -> {:02X}, nuab c18cc874) and removed from "
               "the ready list; the specific MIC, the task dump and the 0AFB end-of-job re-entry (SA21-9436 3-109) are "
               "NOT modelled",
               tb, status, m_.readByte(tb + TaskBlock::kOffStatus));

    enableDispatching("LEVEL 5", "the terminated task cannot remain dispatched");

    if (!dispatch("LEVEL 5")) {
        // The reference's CrustyInteractiveSession arm is off by default and
        // is not ported.
        trace_.csp("LEVEL 5: task block {:04X} was the last runnable task - nudspchA has nothing to dispatch after it "
                   "(c180e04c); the machine stops{}",
                   tb, undischargedActionsSuffix());
        traceActionControllerAtNoTaskExit("LEVEL 5");
        return false;
    }

    restoreRegisters(currentRequestBlock_);
    return true;
}

// Every enqueue of work reaching the emulator ends the guest dispatch loop
// at its next task boundary: one store sets the redispatch request.
void As36ControlStorageProcessor::signalNewWork(const std::string& call)
{
    if (!redispatch_) trace_.csp("{}: newWork sets NuEmul+1646 (redispatch/break)", call);
    redispatch_ = true;
}

// The preemption point SA21-9436 3-67 names: after every serviced call, a
// task readied by a post reaches the processor as soon as its priority
// entitles it to.  Nothing happens unless a wait or a post asked.
void As36ControlStorageProcessor::dispatchIfRequested(const std::string& call)
{
    if (!redispatch_) return;
    if (dispatchDepth_ >= kDispatchDepthLimit) {
        redispatch_ = false;
        return;
    }
    dispatchDepth_++;
    dispatch(call);
    dispatchDepth_--;
}

void As36ControlStorageProcessor::controlStorageTerminate() { trace_.csp("csterm"); }

// =============================================================================
// SVC 00: General Wait
// =============================================================================

// SA21-9436 3-68: the condition in the two inline parameters is ORed into
// the halfword at tb+8..9 and the task waits with code 0x20, the same value
// the general post posts with.  There is no no-wait form.
bool As36ControlStorageProcessor::generalWait(SvcRequest& req)
{
    int tb = req.taskBlock;
    int mask = tb + TaskBlock::kOffGeneralWaitMask;

    m_.writeByte(mask, static_cast<uint8_t>(m_.readByte(mask) | req.inline1));
    m_.writeByte(mask + 1, static_cast<uint8_t>(m_.readByte(mask + 1) | req.inline2));

    trace_.csp("SVC 00: general wait on {:02X}{:02X} - TB_WMASK at tb+8..9 is now {:04X}", req.inline1, req.inline2,
               m_.readHalf(mask));

    // The wait additionally enables dispatching, when the waiting task is
    // the dispatched one, which a general wait's always is.
    enableDispatching("SVC 00", "nuwatcb c18b4bd0");

    taskWaitReturn(tb, kWaitGeneral, req.r, req.q, "SVC 00");
    return waitAndDispatch(tb, "SVC 00");
}

// A general wait taken from inside the supervisor rather than from an SVC
// 00: tb+4 |= 0x0C (both bits together take the ordinary resume row, not
// the re-issue), then the same mask OR and wait code 0x20 with the R-byte
// slot zeroed.
bool As36ControlStorageProcessor::supervisorGeneralWait(int tb, int mask, const std::string& call)
{
    constexpr uint8_t kGwaitcState = 0x0C;

    m_.writeByte(tb + TaskBlock::kOffState, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffState) | kGwaitcState));

    int field = tb + TaskBlock::kOffGeneralWaitMask;
    m_.writeByte(field, static_cast<uint8_t>(m_.readByte(field) | ((mask >> 8) & 0xFF)));
    m_.writeByte(field + 1, static_cast<uint8_t>(m_.readByte(field + 1) | (mask & 0xFF)));

    trace_.csp("{}: nugwaitc - tb+4 |= 0C and TB_WMASK at tb+8..9 is now {:04X} (c18b4628, c18b4688); the wait code is "
               "nugw0020's 20, so SVC 01 general post is what ends it",
               call, m_.readHalf(field));

    enableDispatching(call, "nuwatcb c18b4bd0, via nugwaitc");

    taskWaitReturn(tb, kWaitGeneral, 0x00, 0x00, call);
    return waitAndDispatch(tb, call);
}

// =============================================================================
// SVC 1E: Task Wait
// =============================================================================

// SA21-9436 3-102: inline parameter 1 is read into TB_STAT2 (out of the
// request block, not the live registers), the task waits until a task post
// clears every bit, and dispatching is enabled unconditionally.
bool As36ControlStorageProcessor::taskWait(SvcRequest& req)
{
    enableDispatching("SVC 1E", "nuwatbms c18b4d0c, unconditional");
    taskWaitReturn(req.taskBlock, req.inline1, req.r, req.q, "SVC 1E");
    return waitAndDispatch(req.taskBlock, "SVC 1E");
}

// The dispatching switch is host state on the machine; this emulator's is
// PMR bit 0 (LPMR, SA21-9436 3-33).  Writing the PMR here is the rendering
// of the machine's store.
void As36ControlStorageProcessor::enableDispatching(const std::string& call, const std::string& site)
{
    if (m_.msp.taskDispatchingEnabled()) return;
    m_.msp.setPmr(static_cast<uint8_t>(m_.msp.pmr() | machine::MspRegisters::kPmrTaskDispatch));
    trace_.csp("{}: task dispatching was disabled and is now enabled ({})", call, site);
}

// =============================================================================
// SVC 17: Asynchronous Task Wait
// =============================================================================

// SA21-9436 3-95: put the task block in XR1 into the wait named by inline
// parameter 1; Equal if it was, not equal if it could not be.  A wait the
// caller could not have now is deferred into the target's tb+6 when the
// caller's task id ends in 09 (the IPL task's), and folded in the next time
// that task waits.
bool As36ControlStorageProcessor::asynchronousTaskWait(SvcRequest& req)
{
    int target = RequestBlock::readXr1Field(m_, req.requestBlock);
    if (!TaskBlock::isTaskBlock(m_, target)) {
        trace_.csp("SVC 17: XR1 = {:06X} is not a task block (no TB eyecatcher)", target);
        return false;
    }

    if (m_.msp.taskDispatchingEnabled())
        trace_.csp("SVC 17: SA21-9436 3-95 requires task switching to be DISABLED when this call is issued, and PMR bit 0 "
                   "says it is not; nutkwt does not check, so neither does this - the note is the diagnosis");

    if (!canSuspendAsynchronously(target, "SVC 17")) {
        setCondition(req, kPsrHigh);
        uint16_t callerId = m_.readHalf(req.taskBlock + TaskBlock::kOffTaskId);
        if ((callerId & 0xFF) == 0x09) {
            m_.writeByte(target + TaskBlock::kOffDeferredWait, req.inline1);
            trace_.csp("SVC 17: task block {:04X} cannot be suspended now; the caller's task ID is {:04X}, so nutkwt "
                       "defers the wait into tb+6 = {:02X} (c18b4c90-c18b4ca0)",
                       target, callerId, req.inline1);
        } else {
            trace_.csp("SVC 17: task block {:04X} cannot be suspended and the caller's task ID {:04X} does not end in "
                       "09, so nutkwt drops the request and returns PSR High",
                       target, callerId);
        }
        return true;
    }

    setCondition(req, kPsrEqual);
    taskWaitReturn(target, req.inline1, req.r, req.q, "SVC 17");

    // The CALLER keeps the processor; the redispatch is honoured at the
    // seam in svc().
    return true;
}

// The gauntlet that decides whether a task may be suspended from outside.
// The first eight gates are enforced; the last three (a JCB chain read
// through a dead computation, and a reusable/refreshable pair half of
// which is host state) are left permissive rather than half-built.
bool As36ControlStorageProcessor::canSuspendAsynchronously(int tb, const std::string& call)
{
    if (tb == currentTaskBlock_) {
        trace_.csp("{}: nuwasusp refuses the currently dispatched task block {:04X} (c18b5100)", call, tb);
        return false;
    }
    if (refuseField(call, tb, 10, m_.readByte(tb + 10) != 0, "tb+10 is non-zero", "c18b5108")) return false;
    if (refuseField(call, tb, 11, (m_.readByte(tb + TaskBlock::kOffLockByte1) & 0xFE) != 0,
                    "tb+11 holds a lock other than bit 7", "c18b5114"))
        return false;
    if (refuseField(call, tb, 14, m_.readByte(tb + 14) != 0, "the high byte of tb+14..15 is non-zero", "c18b5120"))
        return false;
    if (refuseField(call, tb, 57, m_.readAddr24(tb + TaskBlock::kOffTransferInterlock) != 0,
                    "a transfer is in flight (tb+57..59)", "c18b512c"))
        return false;
    if (refuseField(call, tb, 5, (m_.readByte(tb + TaskBlock::kOffStat2) & 0x08) != 0, "TB_STAT2 bit 08 is on", "c18b513c"))
        return false;
    if (refuseField(call, tb, 85, (m_.readByte(tb + 85) & 0x01) != 0, "tb+85 bit 7 is on", "c18b5148")) return false;
    if (refuseField(call, tb, 48, (m_.readByte(tb + 48) & 0x80) != 0, "tb+48 bit 0 is on", "c18b5154")) return false;
    return true;
}

bool As36ControlStorageProcessor::refuseField(const std::string& call, int tb, int field, bool refused,
                                              const std::string& why, const std::string& site)
{
    (void)field;
    if (!refused) return false;
    trace_.csp("{}: nuwasusp refuses task block {:04X} - {} ({})", call, tb, why, site);
    return true;
}

// =============================================================================
// SVC 25: Asynchronous Task Ready Check
// =============================================================================

// SA21-9436 3-112: if the task in XR1 is in an event wait and its complete
// queue holds an event that satisfies the wait, the task is posted.
bool As36ControlStorageProcessor::asynchronousTaskReadyCheck(SvcRequest& req)
{
    int tb = RequestBlock::readXr1Field(m_, req.requestBlock);
    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("SVC 25: XR1 = {:06X} is not a task block", tb);
        return false;
    }
    return postTaskCheckDispatch(tb, "SVC 25");
}

// A task in an event wait whose complete queue holds a matching element is
// readied with condition 0x80.
bool As36ControlStorageProcessor::postTaskCheckDispatch(int tb, const std::string& call, bool returnXr2OnPost,
                                                        bool deferDispatch)
{
    if ((m_.readByte(tb + TaskBlock::kOffStat2) & kWaitEvent) == 0) {
        trace_.csp("{}: task block {:04X} is not in an event wait (tb+5 bit 0 off) - nupotkck returns; the element stays "
                   "on tb+47 and the task's next SVC 02 will take it",
                   call, tb);
        return true;
    }

    if (!completedEventForTaskBlock(tb, call, returnXr2OnPost)) {
        trace_.csp("{}: task block {:04X} is in an event wait and nuevt finds nothing complete on its queue at tb+45..47",
                   call, tb);
        return true;
    }

    postTaskBlock(tb, kWaitEvent, call);
    if (!deferDispatch) dispatchIfRequested(call);
    return true;
}

// The event match for a task that is not the caller: the match arguments
// come out of the examined task's OWN request block (tb+65..67 -> rb, rb+21
// its Q-byte).  The post path passes returnXr2 = false: the XR2 store the
// machine performs is suppressed there as a labelled containment, because
// enabling it regressed the reference's guarded IPL milestones and the
// faithful ace+16 for a synthetic post cannot be grounded.
bool As36ControlStorageProcessor::completedEventForTaskBlock(int tb, const std::string& call, bool returnXr2OnPost)
{
    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (rb == 0) return false;
    uint8_t q = m_.readByte(rb + RequestBlock::kOffQByte);
    return completedEvent(tb, rb, q, returnXr2OnPost, call);
}

// =============================================================================
// SVC 30: QLOCK
// =============================================================================

// SA21-9436 3-124: scan the task priority queue (39, ordered by tb+7
// descending, so the first match is the highest-priority waiter) for the
// task whose tb+14..15 holds the qlock in WR6, clear "quick lock failure"
// (tb+9 bit 0x02) and post the general wait condition.
bool As36ControlStorageProcessor::quickLock(SvcRequest& req)
{
    constexpr int kQlockArgument = 14;
    constexpr uint8_t kQuickLockFailure = 0x02;

    uint16_t qlock = RequestBlock::readWr(m_, req.requestBlock, 6);
    if (qlock == 0) {
        trace_.csp("SVC 30: WR6 is zero; nuqlock calls NuEmul::nuerr with code 84 (c1894d94)");
        return false;
    }

    int header = GuestLowStorage::queueHeader(kTaskPriorityQueue);
    int at = m_.readAddr24(header);
    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, header)) return false;
        if (m_.readHalf(at + kQlockArgument) == qlock) {
            int low = at + TaskBlock::kOffGeneralWaitMask + 1;
            m_.writeByte(low, static_cast<uint8_t>(m_.readByte(low) & ~kQuickLockFailure));
            trace_.csp("SVC 30: qlock {:04X} - task block {:04X}, priority {:02X}, is the highest-priority waiter; tb+9 &= "
                       "~02 (quick lock failure)",
                       qlock, at, m_.readByte(at + TaskBlock::kOffPriority));
            postTaskBlock(at, kWaitGeneral, "SVC 30");
            dispatchIfRequested("SVC 30");
            return true;
        }
        at = m_.readAddr24(at + TaskBlock::kOffQueue39Link);
    }

    trace_.csp("SVC 30: no task block on the priority queue (39) has qlock {:04X} at tb+14..15", qlock);
    return true;
}

// =============================================================================
// SVC 1B: Scan System Queue
// =============================================================================

// Two arms on Q bit 1: set, is the block in XR1 on the queue (answer in the
// PSR only); clear, find the element whose Q & 7 byte field ending at
// element + inline 1 equals the low bytes of XR1, returned in XR2.  A zero
// Q returns the HEADER in XR2 before looking at the queue.
bool As36ControlStorageProcessor::scanSystemQueue(SvcRequest& req)
{
    constexpr uint8_t kScanChainField = 0x40;   // Q bit 1

    int header = RequestBlock::readXr2RealAddress(m_, req.requestBlock);
    int headerField = header - 2;
    int argument = RequestBlock::readXr1Field(m_, req.requestBlock);

    if ((req.q & kScanChainField) != 0) {
        bool onQueue = scanForBlock(headerField, argument, req.inline2);
        trace_.csp("SVC 1B: block {:06X} is {}on the queue at {:04X}", argument, onQueue ? "" : "NOT ", headerField);
        setCondition(req, onQueue ? kPsrEqual : kPsrHigh);
        return true;
    }

    if (req.q == 0) {
        RequestBlock::writeXr2(m_, req.requestBlock, header);
        trace_.csp("SVC 1B: Q is zero - XR2 = the queue header {:06X} itself (nuquescan c18e00c0; the manual says \"first "
                   "element\")",
                   header);
        return true;
    }

    int length = req.q == 1 ? 1 : req.q == 2 ? 2 : 3;
    int mask = length == 1 ? 0xFF : length == 2 ? 0xFFFF : 0xFFFFFF;
    int found = scanForField(headerField, argument & mask, req.inline1, length, req.inline2);

    RequestBlock::writeXr2(m_, req.requestBlock, found);
    trace_.csp("SVC 1B: queue header field {:06X} (head {:06X}); {}-byte field at element+{} = {:06X} -> XR2 = {:06X} "
               "({}), chain at element+{}",
               headerField, m_.readAddr24(headerField), length, req.inline1, argument & mask, found,
               found == 0 ? "not found" : "found", req.inline2);

    // The reference's SignonSessionContent experiment (populating a found
    // session block here) is off by default and is not ported.
    return true;
}

bool As36ControlStorageProcessor::scanForBlock(int headerField, int block, int chainLastByte)
{
    int at = m_.readAddr24(headerField);
    for (int steps = 0; at != 0; steps++) {
        if (at == block) return true;
        if (!chainStepValid(at, steps, headerField)) return false;
        at = m_.readAddr24(at + chainLastByte - 2);
    }
    return false;
}

int As36ControlStorageProcessor::scanForField(int headerField, int argument, int argumentLastByte, int length,
                                              int chainLastByte)
{
    int at = m_.readAddr24(headerField);
    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, headerField)) return 0;
        int value = 0;
        for (int i = length - 1; i >= 0; i--) value = (value << 8) | m_.readByte(at + argumentLastByte - i);
        if (value == argument) return at;
        at = m_.readAddr24(at + chainLastByte - 2);
    }
    return 0;
}

// =============================================================================
// SVC 23: Test and Set
// =============================================================================

// The byte is addressed through XR1 with the translate bit dropped; a bit
// already on answers Test False plus Equal, or, with Q bit 7, takes a
// general wait on condition 2000 ("test and set failure", SA21-9436 3-68).
bool As36ControlStorageProcessor::testAndSet(SvcRequest& req)
{
    constexpr uint8_t kSetLockIndicator = 0x20;   // Q bit 2
    constexpr uint8_t kWaitIfOn = 0x01;           // Q bit 7
    constexpr uint8_t kPsrTestFalse = 0x10;

    int at = RequestBlock::readXr1RealAddress(m_, req.requestBlock);
    uint8_t bit = req.inline1;
    uint8_t value = m_.readByte(at);

    if ((value & bit) != 0) {
        if ((req.q & kWaitIfOn) != 0) {
            trace_.csp("SVC 23: bit {:02X} at {:06X} is already on and Q bit 7 asks to wait; nugwaitc takes a general wait "
                       "on condition 2000",
                       bit, at);
            m_.writeByte(req.taskBlock + TaskBlock::kOffState,
                         static_cast<uint8_t>(m_.readByte(req.taskBlock + TaskBlock::kOffState) | 0x0C));
            SvcRequest gw = req;
            gw.inline1 = 0x20;
            gw.inline2 = 0x00;
            gw.r = 0x00;
            return generalWait(gw);
        }
        setCondition(req, static_cast<uint8_t>(kPsrTestFalse | kPsrEqual));
        trace_.csp("SVC 23: bit {:02X} at {:06X} was already on - PSR test false", bit, at);
        return true;
    }

    m_.writeByte(at, static_cast<uint8_t>(value | bit));
    if ((req.q & kSetLockIndicator) != 0) {
        int lock1 = req.taskBlock + TaskBlock::kOffLockByte1;
        uint8_t wr5 = static_cast<uint8_t>(RequestBlock::readWr(m_, req.requestBlock, 5));
        m_.writeByte(lock1, static_cast<uint8_t>(m_.readByte(lock1) | wr5));
        trace_.csp("SVC 23: task block lock byte 1 |= WR5 {:02X}", wr5);
    }
    trace_.csp("SVC 23: bit {:02X} at {:06X} was off and is now set", bit, at);
    return true;
}

// =============================================================================
// SVC 24: Task Block Priority Queue
// =============================================================================

// The requested priority goes to tb+20 and tb+28, the dispatching priority
// at tb+7 is min(requested + 4 * tb[64], 236), and the task is requeued on
// queues 39 and 40 (40 only when it is not waiting).
bool As36ControlStorageProcessor::taskBlockPriorityQueue(SvcRequest& req)
{
    int tb = RequestBlock::readXr1Field(m_, req.requestBlock);
    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("SVC 24: XR1 = {:06X} is not a task block (no TB eyecatcher)", tb);
        return false;
    }

    uint8_t requested = req.inline1;
    m_.writeByte(tb + TaskBlock::kOffPriorityFloor, requested);
    m_.writeByte(tb + TaskBlock::kOffPriorityRequested, requested);
    setDispatchingPriority(tb, requested);

    requeueByPriority(tb, TaskBlock::kChainLastQueue39, static_cast<uint8_t>(kTaskPriorityQueue));
    if (!TaskBlock::isWaiting(m_, tb))
        requeueByPriority(tb, TaskBlock::kChainLastQueue40, static_cast<uint8_t>(kTaskReadyQueue));
    else
        trace_.csp("SVC 24: task block {:04X} is waiting, so nuprqf10 leaves it off the ready queue", tb);
    return true;
}

void As36ControlStorageProcessor::setDispatchingPriority(int tb, uint8_t requested)
{
    int priority = requested + 4 * m_.readByte(tb + TaskBlock::kOffPriorityBias);
    if (priority > 236) priority = 236;
    m_.writeByte(tb + TaskBlock::kOffPriority, static_cast<uint8_t>(priority));
    trace_.csp("SVC 24: task block {:04X} priority {:02X} + 4*{} -> tb+7 = {:02X}", tb, requested,
               m_.readByte(tb + TaskBlock::kOffPriorityBias), priority);
}

// Dequeue, then insert by priority on tb+7.
void As36ControlStorageProcessor::requeueByPriority(int tb, int chainLastByte, uint8_t headerNumber)
{
    constexpr uint8_t kDequeue = 0x20, kSystemRequest = 0x40, kPriority = 0x80;
    int headerField = GuestLowStorage::queueHeader(headerNumber);
    queueOperation(headerField, tb, chainLastByte, kDequeue | kSystemRequest);
    queueOperation(headerField, tb, chainLastByte, static_cast<uint8_t>(kPriority | kSystemRequest | TaskBlock::kOffPriority));
}

// =============================================================================
// SVC 03 and 19: Event Post, Post Action Control Element
// =============================================================================

// The same operation reached two ways: SVC 03 is handed the event control
// mask and finds the element through it, SVC 19 is handed the element and
// finds the mask at ace+13.
bool As36ControlStorageProcessor::postActionControlElement(SvcRequest& req)
{
    int aceField = RequestBlock::readXr1Field(m_, req.requestBlock);
    int ace = 0;
    if (!m_.resolveGuest24(aceField, false, ace)) ace = 0;
    if (m_.readHalf(ace) != ActionControlElement::kEyecatcher) {
        trace_.csp("SVC 19: {:06X} (resolved {:06X}) is not an action control element - nuposta aborts through nuerabt "
                   "with code 93", aceField, ace);
        return false;
    }
    int ecm = 0;
    if (!aces_.ecmAddress(ace, ecm)) {
        trace_.csp("SVC 19: action control element {:06X} has no retained ECM translation", ace);
        return false;
    }
    return postEvent(ecm, req.inline1, req.inline2, "SVC 19");
}

bool As36ControlStorageProcessor::eventPost(SvcRequest& req)
{
    int ecmField = RequestBlock::readXr1Field(m_, req.requestBlock);
    int ecm = 0;
    if (ecmField != 0 && !m_.resolveGuest24(ecmField, false, ecm)) {
        trace_.csp("SVC 03: event control mask {:06X} cannot be resolved", ecmField);
        return false;
    }
    return postEvent(ecm, req.inline1, req.inline2, "SVC 03");
}

// Inline parameter 1 is the system queue header the element is waiting on;
// inline parameter 2 carries the completion code in its low nibble and the
// LIFO request in bit 1.  A zero element address means the post is ignored
// (SA21-9436 says so); the mask is un-posted, the element dequeued from the
// header, and with ace+28 bit 0 on it goes to the requester's complete
// event queue.
bool As36ControlStorageProcessor::postEvent(int ecm, uint8_t headerNumber, uint8_t indicators, const std::string& call)
{
    constexpr uint8_t kCompleteQueueEntry = 0x80;   // ace+28 bit 0

    if (ecm == 0 || ecm + Ecm::kSize > m_.backingBytes()) {
        trace_.csp("{}: event control mask address {:06X} is not addressable", call, ecm);
        return false;
    }

    int ace = m_.readAddr24(ecm + Ecm::kOffAceAddress);
    if (ace == 0) {
        trace_.csp("{}: mask {:06X} has no action control element address - the post is ignored, as SA21-9436 requires",
                   call, ecm);
        return true;
    }

    Ecm::post(m_, ecm, indicators & 0x0F);
    trace_.csp("{}: mask {:06X} posted {:02X}, element {:04X} off queue header {}", call, ecm, 0x40 | (indicators & 0x0F),
               ace, headerNumber);

    queueOperation(GuestLowStorage::queueHeader(headerNumber), ace, ActionControlElement::kChainLastByte, 0x60);

    if ((m_.readByte(ace + ActionControlElement::kOffFlags) & kCompleteQueueEntry) == 0) return true;
    return completeToTask(ace, indicators, call);
}

// The element is queued to the complete event queue of the task named at
// ace+19 (a header at tb+47, chain displacement 4, LIFO when inline 2 bit
// 1 is on) and the task is readied if it is in an event wait.  The ace+28
// bit 7 arm adjusts counters at tb+130 and tb+150 and can end in a free or
// a task termination, none of which is established.
bool As36ControlStorageProcessor::completeToTask(int ace, uint8_t indicators, const std::string& call, bool returnXr2OnPost,
                                                 bool deferDispatch)
{
    constexpr uint8_t kLifo = 0x40;              // inline 2 bit 1
    constexpr uint8_t kAccountedElement = 0x01;  // ace+28 bit 7

    if ((m_.readByte(ace + ActionControlElement::kOffFlags) & kAccountedElement) != 0) {
        trace_.csp("{}: element {:04X} has ace+28 bit 7 set; nupo0024 then adjusts tb+130 / tb+150 and may call nufree or "
                   "nupterm, none of which is established (docs/s36/ace-format.md)",
                   call, ace);
        return false;
    }

    int tb = m_.readAddr24(ace + ActionControlElement::kOffTaskBlock);
    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("{}: element {:04X} names {:06X} at +19, which is not a task block", call, ace, tb);
        return false;
    }

    queueOperation(tb + TaskBlock::kOffCompleteQueue, ace, ActionControlElement::kChainLastByte,
                   static_cast<uint8_t>((indicators & kLifo) != 0 ? 0x10 : 0x00));
    trace_.csp("{}: element {:04X} queued {} to the complete event queue of task block {:04X} at tb+47", call, ace,
               (indicators & kLifo) != 0 ? "LIFO" : "FIFO", tb);

    return postTaskCheck(tb, call, returnXr2OnPost, deferDispatch);
}

bool As36ControlStorageProcessor::postTaskCheck(int tb, const std::string& call, bool returnXr2OnPost, bool deferDispatch)
{
    return postTaskCheckDispatch(tb, call, returnXr2OnPost, deferDispatch);
}

// =============================================================================
// SVC 2B: Post Task by Task ID
// =============================================================================

// WR5 is a task ID: zero is this task, anything else a walk of queue 39 on
// tb+2.  The element is queued to that task's complete event queue as a
// completed event would be.
bool As36ControlStorageProcessor::postTaskByTaskId(SvcRequest& req)
{
    constexpr uint8_t kEventTypeGiven = 0x04;    // Q bit 5
    constexpr uint8_t kAsynchronousWait = 0x10;  // Q bit 3

    int id = RequestBlock::readWr(m_, req.requestBlock, 5);
    int tb = findTaskById(id, req.taskBlock);
    if (tb == 0) {
        trace_.csp("SVC 2B: no task block on queue 39 has task ID {:04X}; caller task {:04X} currently has ID {:04X}; "
                   "return IAR {:04X} from request block {:04X}",
                   id, req.taskBlock, m_.readHalf(req.taskBlock + TaskBlock::kOffTaskId),
                   m_.readHalf(req.requestBlock + RequestBlock::kOffIar), req.requestBlock);
        setCondition(req, kPsrHigh);
        return true;
    }
    setCondition(req, kPsrEqual);

    int ace = aces_.allocate();
    if (ace == 0) return false;

    ActionControlElement::build(m_, ace, req.requestBlock, req.taskBlock, req.q);
    m_.writeAddr24(ace + ActionControlElement::kOffTaskBlock, tb);
    m_.writeHalf(ace + ActionControlElement::kOffEventType,
                 (req.q & kEventTypeGiven) != 0 ? RequestBlock::readWr(m_, req.requestBlock, 6) : static_cast<uint16_t>(0));
    if ((req.q & kAsynchronousWait) != 0)
        m_.writeByte(ace + ActionControlElement::kOffFlags,
                     static_cast<uint8_t>(m_.readByte(ace + ActionControlElement::kOffFlags) | 0x10));

    trace_.csp("SVC 2B: task ID {:04X} -> task block {:04X}, element {:04X}", id, tb, ace);
    return completeToTask(ace, req.inline1, "SVC 2B");
}

// =============================================================================
// SVC 02: Event Wait
// =============================================================================

// Satisfied when an element on the caller's complete event queue answers
// the wait (consumed), or, for a specific wait, when the 7th byte of the
// mask XR1 names has bit 1 on.
// Q bit 7 off is "wait without wait": the PSR answers and the task never
// blocks; on, the task waits with the event-wait code 0x80.
bool As36ControlStorageProcessor::eventWait(SvcRequest& req)
{
    constexpr uint8_t kLongWait = 0x40;      // Q bit 1
    constexpr uint8_t kReturnXr2 = 0x20;     // Q bit 2
    constexpr uint8_t kMultipleWait = 0x08;  // Q bit 4
    constexpr uint8_t kWait = 0x01;          // Q bit 7

    bool satisfied = completedEventForTask(req);

    if (!satisfied && (req.q & kMultipleWait) == 0) {
        int xr1Field = RequestBlock::readXr1Field(m_, req.requestBlock);
        int xr1 = 0;
        if (!m_.resolveGuest24(xr1Field, false, xr1)) {
            trace_.csp("SVC 02: XR1 = {:06X} cannot be resolved, so the wait is not satisfied", xr1Field);
        } else if (xr1 != 0 && Ecm::isComplete(m_, xr1)) {
            satisfied = true;
            trace_.csp("SVC 02: mask {:06X} (resolved {:06X}) is complete ({:02X})", xr1Field, xr1,
                       m_.readByte(xr1 + Ecm::kOffCompletion));
        }
    }

    if (!satisfied) {
        if ((req.q & kWait) != 0) {
            trace_.csp("SVC 02: nothing is complete and Q bit 7 asks to wait; nuwait calls nuwasvtb with wait code 80 "
                       "(c18b4a90, c18b4b54)");
            if ((req.q & kLongWait) != 0) longWaitPriority(req.taskBlock);
            enableDispatching("SVC 02", "nuwatcb c18b4bd0, via nuwasvtb");
            taskWaitReturn(req.taskBlock, kWaitEvent, req.r, req.q, "SVC 02");
            return waitAndDispatch(req.taskBlock, "SVC 02");
        }
        setCondition(req, kPsrHigh);
        trace_.csp("SVC 02: wait without wait - not satisfied");
        return true;
    }

    // The PSR is only set on the no-wait form.
    if ((req.q & kWait) == 0) setCondition(req, kPsrEqual);
    if ((req.q & kLongWait) != 0) longWaitPriority(req.taskBlock);
    if ((req.q & kReturnXr2) != 0 && !satisfied) trace_.csp("SVC 02: Q bit 2 asked for XR2 but no element supplied one");
    return true;
}

// The event-type selector (nuevt c18b5e54..c18b5eb8): WR6 zero matches
// any; a high-byte overlap matches; with WR6 bit 0 on the low bytes must be
// equal; otherwise the type must be even and the two LOW BYTES must share a
// bit (c18b5e98 computes that AND and c18b5eac branches on it).  The
// reference transcribed the last arm as "WR6's low byte non-zero" and
// called the AND dead; the later verified reading in
// docs/s36/console-acquire-and-wake-2026-09-08.md is the one SSP's spool
// writer depends on: its type-0020 polls must not consume the type-0010
// general-post element it later waits for by key.
bool As36ControlStorageProcessor::waitEventTypeMatches(uint16_t wr6, uint16_t type)
{
    if (wr6 == 0) return true;
    if (((wr6 >> 8) & (type >> 8)) != 0) return true;
    if ((wr6 & 0x01) != 0) return (type & 0xFF) == (wr6 & 0xFF);
    if ((type & 0x01) != 0) return false;
    return ((wr6 & type) & 0xFF) != 0;
}

bool As36ControlStorageProcessor::completedEventForTask(SvcRequest& req)
{
    return completedEvent(req.taskBlock, req.requestBlock, req.q, true, "SVC 02");
}

// The one match-output model, shared by the wait path and the post path.
// Specific wait: ace+29..31 equals the examined task's XR1, or Q bit 3 and
// ace+28 bit 3 mark the element asynchronous.  Multiple wait: ace+28 bit 4
// from the action Q byte or mask+5 bit 0 marks a candidate.  The ECM
// attribute captured at submission survives a later unmap, while the live
// translation remains significant when SSP deliberately reuses the logical
// IOB address in a newly mapped work area.  With Q bit 5 the type at ace+22
// must match WR6 (which receives the matched type).  On a match XR1 =
// ace+29..31, XR2 = ace+16..18 when the examined task's Q bit 2 asks (and
// the caller allows), and the element is unlinked and freed.
bool As36ControlStorageProcessor::completedEvent(int tb, int rb, uint8_t q, bool returnXr2, const std::string& call)
{
    constexpr uint8_t kAsynchronousWait = 0x10;
    constexpr uint8_t kMultipleWait = 0x08, kEventTypeGiven = 0x04;
    constexpr uint8_t kReturnXr2 = 0x20;

    int headerField = tb + TaskBlock::kOffCompleteQueue;
    int xr1 = RequestBlock::readXr1Field(m_, rb);
    uint16_t wr6 = RequestBlock::readWr(m_, rb, 6);

    int prev = 0, at = m_.readAddr24(headerField);
    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, headerField)) return false;
        uint8_t flags = m_.readByte(at + ActionControlElement::kOffFlags);
        int ecm = m_.readAddr24(at + ActionControlElement::kOffXr1);
        int elementKey = m_.readAddr24(at + ActionControlElement::kOffXr1Copy);
        uint16_t elementType = m_.readHalf(at + ActionControlElement::kOffEventType);
        bool match;
        std::string mismatch;

        if ((q & kMultipleWait) == 0) {
            bool keyMatch = elementKey == xr1;
            bool asyncMatch = (q & kAsynchronousWait) != 0 && (flags & 0x10) != 0;
            match = keyMatch || asyncMatch;
            mismatch = fmt::format("specific key-match={}, async-match={}", keyMatch ? "True" : "False",
                                   asyncMatch ? "True" : "False");
        } else {
            bool flagsCandidate = (flags & ActionControlElement::kFlagsMultipleWait) != 0;
            int ecmReal = 0;
            bool retainedEcmCandidate = aces_.ecmMultipleWaitEligible(at);
            bool liveEcmCandidate = ecm != 0 && m_.resolveGuest24(ecm, false, ecmReal) && ecmReal != 0 &&
                                    (m_.readByte(ecmReal + Ecm::kOffMultiWait) & 0x80) != 0;
            bool ecmCandidate = retainedEcmCandidate || liveEcmCandidate;
            match = flagsCandidate || ecmCandidate;
            bool typeMatch = true;
            if (match && (q & kEventTypeGiven) != 0) {
                typeMatch = waitEventTypeMatches(wr6, elementType);
                match = typeMatch;
                if (match) RequestBlock::writeWr(m_, rb, 6, elementType);
            }
            mismatch = fmt::format("multiple flags-candidate={}, ecm-candidate={}, type-match={}",
                                   flagsCandidate ? "True" : "False", ecmCandidate ? "True" : "False",
                                   typeMatch ? "True" : "False");
        }

        if (match) {
            RequestBlock::writeXr1(m_, rb, m_.readAddr24(at + ActionControlElement::kOffXr1Copy));
            if (returnXr2 && (q & kReturnXr2) != 0)
                RequestBlock::writeXr2(m_, rb, m_.readAddr24(at + ActionControlElement::kOffXr2));
            m_.writeAddr24(prev == 0 ? headerField : prev + ActionControlElement::kOffChainLink,
                           m_.readAddr24(at + ActionControlElement::kOffChainLink));
            m_.writeAddr24(at + ActionControlElement::kOffChainLink, 0);
            aces_.release(at);
            trace_.csp("{}: nuevt - element {:04X} off task block {:04X}'s complete queue satisfies the wait (its Q-byte "
                       "{:02X} from rb+21); XR1 = {:06X}",
                       call, at, tb, q, RequestBlock::readXr1Field(m_, rb));
            return true;
        }
        trace_.csp("{}: nuevt skips element {:04X} for task {:04X}: {}; wait q={:02X} XR1={:06X} WR6={:04X}, element "
                   "flags={:02X} key={:06X} type={:04X} ECM={:06X}",
                   call, at, tb, mismatch, q, xr1, wr6, flags, elementKey, elementType, ecm);
        prev = at;
        at = m_.readAddr24(at + ActionControlElement::kOffChainLink);
    }
    return false;
}

// The long-wait arm every wait call shares: mark the task at tb+13 and
// drop its priority by four, never below the floor at tb+20 and never for
// a task at 240 or above.
void As36ControlStorageProcessor::longWaitPriority(int tb)
{
    m_.writeByte(tb + TaskBlock::kOffLongWaitMark, 250);
    uint8_t priority = m_.readByte(tb + TaskBlock::kOffPriority);
    if (priority >= 240) {
        trace_.csp("long wait: task block {:04X} is at priority {:02X}, which nuprqvlw leaves alone", tb, priority);
        return;
    }
    uint8_t lowered = static_cast<uint8_t>(priority - 4);
    uint8_t floor = m_.readByte(tb + TaskBlock::kOffPriorityFloor);
    if (lowered >= floor)
        setDispatchingPriority(tb, lowered);
    else
        m_.writeByte(tb + TaskBlock::kOffPriority, floor);

    requeueByPriority(tb, TaskBlock::kChainLastQueue39, static_cast<uint8_t>(kTaskPriorityQueue));
    if (!TaskBlock::isWaiting(m_, tb))
        requeueByPriority(tb, TaskBlock::kChainLastQueue40, static_cast<uint8_t>(kTaskReadyQueue));
}

// =============================================================================
// SVC 1D: Task Post
// =============================================================================

// The posted conditions are cleared from tb+6 and from TB_STAT2 at tb+5,
// and the task is readied only if tb+5 reaches zero.
bool As36ControlStorageProcessor::taskPost(SvcRequest& req)
{
    int tb = RequestBlock::readXr1Field(m_, req.requestBlock);
    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("SVC 1D: XR1 = {:06X} is not a task block; nupotb validates the TB eyecatcher and returns", tb);
        return false;
    }
    return postTaskBlock(tb, req.inline1, "SVC 1D");
}

bool As36ControlStorageProcessor::postTaskBlock(int tb, uint8_t conditions, const std::string& call)
{
    uint8_t deferred = static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffDeferredWait) & ~conditions);
    m_.writeByte(tb + TaskBlock::kOffDeferredWait, deferred);
    return postTaskConditionsDirect(tb, conditions, call);
}

// The task post entry proper: clear conditions from TB_STAT2 but not from
// tb+6.  There is no wait-state test before the ready tail; a post
// targeting the running task harmlessly reports "already there" on the
// ready list, but the state stores and redispatch request still occur.
bool As36ControlStorageProcessor::postTaskConditionsDirect(int tb, uint8_t conditions, const std::string& call)
{
    uint8_t waiting = static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffStat2) & ~conditions);
    m_.writeByte(tb + TaskBlock::kOffStat2, waiting);
    trace_.csp("{}: task block {:04X} posted {:02X} - tb+6 = {:02X}, TB_STAT2 (tb+5) = {:02X}", call, tb, conditions,
               m_.readByte(tb + TaskBlock::kOffDeferredWait), waiting);

    if (waiting != 0) return true;

    readyTask(tb, call);
    return true;
}

// The native timer transient reaches the task post directly, not through
// the SVC 1D wrapper: the timer post clears TB_STAT2 but not tb+6 first.
bool As36ControlStorageProcessor::postTaskConditionsFromTransient(int tb, uint8_t conditions, const std::string& call)
{
    if (!TaskBlock::isTaskBlock(m_, tb)) {
        trace_.csp("{}: transient nupotcb target {:06X} is not a task block; no post", call, tb);
        return false;
    }
    return postTaskConditionsDirect(tb, conditions, call);
}

// =============================================================================
// SVC 01: General Post
// =============================================================================

// The 16-bit condition in the two inline parameters is matched against the
// halfword at tb+8..9 of every task block on queue 39; a match zeroes the
// halfword, clears tb+4 bit 5 when bit 4 is on, and posts condition 0x20.
// The machine-action arm (inline 1 bit 2) is a no-op with queue 4 empty
// and refused otherwise.  Queue 30 holds ACEs whose ECM+7 condition mask is
// tested against the same 16-bit condition; matching elements take SLIC's
// nupostac(ace, 0, 30) path.
bool As36ControlStorageProcessor::generalPost(SvcRequest& req)
{
    constexpr uint8_t kClearLockIndicator = 0x20;   // Q bit 2
    constexpr uint8_t kMachineAction = 0x20;        // inline 1 bit 2

    if ((req.q & kClearLockIndicator) != 0) {
        int lock1 = req.taskBlock + TaskBlock::kOffLockByte1;
        uint8_t wr5 = static_cast<uint8_t>(RequestBlock::readWr(m_, req.requestBlock, 5));
        m_.writeByte(lock1, static_cast<uint8_t>(m_.readByte(lock1) & ~wr5));
        trace_.csp("SVC 01: task block lock byte 1 &= ~WR5 {:02X}", wr5);
    }

    if ((req.inline1 & kMachineAction) != 0) {
        int q4 = m_.readAddr24(GuestLowStorage::queueHeader(kMachineActionQueue));
        if (q4 != 0) {
            trace_.csp("SVC 01: condition {:02X}{:02X}, inline 1 bit 2, and system queue 4 is not empty ({:06X}) - nugpstcs "
                       "would dequeue each element and run a control storage action through nuset/nucx (the NuSetAction "
                       "controller), which is not modelled",
                       req.inline1, req.inline2, q4);
            return false;
        }
        trace_.csp("SVC 01: inline 1 bit 2 machine-action arm - system queue 4 empty, so nugpstcs's dequeue-and-nuset step "
                   "is a no-op; general post proceeds");
    }

    int header = GuestLowStorage::queueHeader(kTaskPriorityQueue);
    int at = m_.readAddr24(header), posted = 0;
    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, header)) return false;
        int next = m_.readAddr24(at + TaskBlock::kOffQueue39Link);
        uint8_t m1 = m_.readByte(at + TaskBlock::kOffGeneralWaitMask);
        uint8_t m2 = m_.readByte(at + TaskBlock::kOffGeneralWaitMask + 1);
        if ((m1 & req.inline1) != 0 || (m2 & req.inline2) != 0) {
            m_.writeHalf(at + TaskBlock::kOffGeneralWaitMask, 0);
            uint8_t state = m_.readByte(at + TaskBlock::kOffState);
            if ((state & 0x08) != 0) m_.writeByte(at + TaskBlock::kOffState, static_cast<uint8_t>(state & ~0x04));
            posted++;
            if (!postTaskBlock(at, kGeneralWaitCondition, "SVC 01")) return false;
        }
        at = next;
    }

    const uint16_t condition = static_cast<uint16_t>((req.inline1 << 8) | req.inline2);
    int queue30Header = GuestLowStorage::queueHeader(kGeneralPostElementQueue);
    at = m_.readAddr24(queue30Header);
    int elementsPosted = 0;
    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, queue30Header)) return false;
        if (at + ActionControlElement::kSize > m_.backingBytes() ||
            m_.readHalf(at + ActionControlElement::kOffEyecatcher) != ActionControlElement::kEyecatcher) {
            trace_.csp("SVC 01: queue {} element {:06X} is not an addressable action control element",
                       kGeneralPostElementQueue, at);
            return false;
        }

        // nugpstcs saves the next link before nupostac dequeues the current
        // ACE.  Preserve that order: the dequeue clears the current link.
        int next = m_.readAddr24(at + ActionControlElement::kOffChainLink);
        int ecmField = m_.readAddr24(at + ActionControlElement::kOffXr1);
        int ecm = 0;
        if (!aces_.ecmAddress(at, ecm) || ecm == 0 || ecm > m_.backingBytes() - Ecm::kOffGeneralPostMask - 2) {
            trace_.csp("SVC 01: queue {} element {:06X} names an unaddressable ECM {:06X}",
                       kGeneralPostElementQueue, at, ecmField);
            return false;
        }
        uint16_t mask = m_.readHalf(ecm + Ecm::kOffGeneralPostMask);
        if ((mask & condition) != 0) {
            trace_.csp("SVC 01: condition {:04X} matches queue {} element {:06X}, ECM {:06X} mask {:04X}; "
                       "nugpstcs posts it with completion 0",
                       condition, kGeneralPostElementQueue, at, ecm, mask);
            // We already have the ACE which nugpstcs matched.  Do not go
            // back through ECM+2 to find it: nubldace only fills that field
            // when Q bit 2 asks for the element address to be returned, and
            // CNFIGSSP deliberately builds its printer wait without that
            // option.  nupostac is passed the known ACE directly.
            Ecm::post(m_, ecm, 0);
            queueOperation(queue30Header, at, ActionControlElement::kChainLastByte, 0x60);
            if ((m_.readByte(at + ActionControlElement::kOffFlags) & ActionControlElement::kFlagsBase) != 0 &&
                !completeToTask(at, 0, "SVC 01 nugpstcs"))
                return false;
            elementsPosted++;
        }
        at = next;
    }

    trace_.csp("SVC 01: condition {:02X}{:02X} posted {} task(s) on queue 39 and {} element(s) on queue {}",
               req.inline1, req.inline2, posted, elementsPosted, kGeneralPostElementQueue);
    return true;
}

// =============================================================================
// SVC 0B: Post Action Controller Status Word
// =============================================================================

// Mask 0x79 sets bit 5 of guest 0x08B4; every other mask test-and-sets a
// host status bit and queues the named routine for the action scheduler's
// drain.  The SVC never fails: it is overlapped by definition.
bool As36ControlStorageProcessor::postActionControllerStatusWord(SvcRequest& req)
{
    constexpr uint8_t kGuestStatusMask = 0x79;
    constexpr int kActionControllerByte = 0x08B4;
    constexpr uint8_t kBit5 = 0x04;

    if (req.inline1 == kGuestStatusMask) {
        uint8_t v = m_.readByte(kActionControllerByte);
        m_.writeByte(kActionControllerByte, static_cast<uint8_t>(v | kBit5));
        trace_.csp("SVC 0B: mask 79 -> guest {:04X} |= {:02X} (now {:02X})", kActionControllerByte, kBit5, v | kBit5);
        ActionMaskCoverage& c = coverage(kGuestStatusMask);
        c.seen++;
        c.discharged++;
        c.lastIar = req.sourceIar;
        c.lastTaskBlock = req.taskBlock;
        c.lastMember = req.sourceMember;
        return true;
    }

    queueSetAction(req.inline1, req);
    return true;
}

// ---- the action controller -------------------------------------------------------

// The bit a mask selects within its word: the mask itself, or mask - 0x40.
// Only the coalescing of a second post of the same mask depends on it.
uint64_t As36ControlStorageProcessor::actionStatusBit(uint8_t mask)
{
    return 1ULL << (isMaskAboveWordSplit(mask) ? mask - 0x40 : mask);
}

As36ControlStorageProcessor::ActionMaskCoverage& As36ControlStorageProcessor::coverage(uint8_t mask)
{
    auto it = actionCoverage_.find(mask);
    if (it == actionCoverage_.end()) {
        ActionMaskCoverage c;
        c.mask = mask;
        it = actionCoverage_.emplace(mask, c).first;
    }
    return it->second;
}

// Test-and-set the mask's status bit and, only when it was clear, queue an
// action.  Returns to the caller either way.
void As36ControlStorageProcessor::queueSetAction(uint8_t mask, SvcRequest& req)
{
    uint64_t bit = actionStatusBit(mask);
    bool high = isMaskAboveWordSplit(mask);
    uint64_t word = high ? actionStatusHigh_ : actionStatusLow_;
    ActionMaskCoverage& c = coverage(mask);
    c.seen++;
    c.lastIar = req.sourceIar;
    c.lastTaskBlock = req.taskBlock;
    c.lastMember = req.sourceMember;

    if ((word & bit) != 0) {
        c.coalesced++;
        trace_.csp("SVC 0B: mask {:02X} already pending in NuEmul+{} - nuset coalesces the post (c18d17a4); {} is still "
                   "queued once",
                   mask, high ? 1440 : 1436, actionSchedulerRoutineName(mask));
        return;
    }
    if (high)
        actionStatusHigh_ |= bit;
    else
        actionStatusLow_ |= bit;
    PendingSetAction a;
    a.mask = mask;
    a.iar = req.sourceIar;
    a.taskBlock = req.taskBlock;
    a.member = req.sourceMember;
    actionQueue_.push_back(a);
    trace_.csp("SVC 0B: mask {:02X} -> nuset sets NuEmul+{} bit and queues NuSetAction({:02X}) via newWork (c18ab520) for "
               "{}; SVC returns, the routine runs when the action scheduler drains",
               mask, high ? 1440 : 1436, mask, actionSchedulerRoutineName(mask));
}

// For every queued action, in order: clear the status bit, then run the
// routine the code names.  Called from svc() in the slot the emulator uses
// for the host scheduler's turn.
void As36ControlStorageProcessor::drainActionScheduler(const std::string& call)
{
    while (!actionQueue_.empty()) {
        PendingSetAction a = actionQueue_.front();
        actionQueue_.pop_front();
        uint64_t bit = actionStatusBit(a.mask);
        if (isMaskAboveWordSplit(a.mask))
            actionStatusHigh_ &= ~bit;
        else
            actionStatusLow_ &= ~bit;
        executeSetAction(a, call);
    }
}

void As36ControlStorageProcessor::executeSetAction(const PendingSetAction& a, const std::string& call)
{
    ActionMaskCoverage& c = coverage(a.mask);
    switch (a.mask) {
        case 0x11:
        case 0x65:
            // The task dispatcher: the post-SVC dispatch honours it.
            trace_.csp("SVC 0B: mask {:02X} posts nudspchA (task dispatcher); the post-SVC DispatchIfRequested honours it "
                       "(executeRequest c18ab2f0)",
                       a.mask);
            c.discharged++;
            return;

        case kR3WorkStationDeviceStatusMask:
            trace_.csp("SVC 0B: mask 29 is SSP R3's equate of 7.5's 2D (work-station device status, #CPTS instruction for "
                       "instruction); the A/36 SLIC would take executeRequest's default arm and ABEND the task via "
                       "nuerio/nuerr/nuab. EMULATOR DECISION beyond SLIC: dispatched as 2D "
                       "(docs/s36/svc0b-mask-29-is-r3-device-status-2026-09-10.md)");
            [[fallthrough]];
        case 0x2D:
            // The work-station device-status scan: a latch and a descriptor
            // in the controller, whose consumer is not implemented.  No guest
            // state is fabricated; the survey below is read-only.
            trace_.csp("SVC 0B: mask 2D schedules NuActiveCtl::wsdvcsr via nuset/NuSetAction (c18ab474); SVC returns "
                       "asynchronously. Native wsdvcsr ensures the NuActiveCtl+0xB8 return-WSCF object and stores a "
                       "32-byte scan descriptor at NuActiveCtl+0x158; its wsentry consumer/callback is not implemented. No "
                       "guest ACE or session is fabricated (docs/s36/action-2d-wsentry-contract-2026-09-07.md)");
            wsEntryScanSurvey("SVC 0B mask 2D", [this](const std::string& m) { trace_.csp("{}", m); });
            c.discharged++;
            return;

        default:
            // Host action-scheduler routines this emulator does not model.
            // Recorded, not stopped: the guest never sees a failure here.
            c.undischarged++;
            trace_.csp("SVC 0B: mask {:02X} names {}, a host action-scheduler routine this emulator does not model; the "
                       "post is recorded as UNDISCHARGED (issued at IAR {:04X}{}, task {:04X}, {}; {} seen, {} undischarged)",
                       a.mask, actionSchedulerRoutineName(a.mask), a.iar, a.member.empty() ? "" : " " + a.member,
                       a.taskBlock, call, c.seen, c.undischarged);
            return;
    }
}

std::string As36ControlStorageProcessor::actionSchedulerRoutineName(uint8_t mask)
{
    switch (mask) {
        case 0x01: return "NutiTimer::nutislih (timer SLIH)";
        case 0x11: case 0x65: return "NuEmul::nudspchA (dispatcher)";
        case 0x17: case 0x19: return "NuActiveCtl::wsentry (work-station entry)";
        case 0x1B: return "NuEmul::nuerio(0x4F) (error I/O)";
        case kR3WorkStationDeviceStatusMask: return "2D (R3 equate) NuActiveCtl::wsdvcsr (work-station device status)";
        case 0x2D: return "NuActiveCtl::wsdvcsr (work-station device status)";
        case 0x6B: return "NuEmul::nucready (make control ready)";
        case 0x79: return "guest 0x08B4 bit 5 (inline in nusvc c18e3848, no NuSetAction)";
        default: return "NuEmul::nuerio (error I/O, executeRequest default arm)";
    }
}

std::string As36ControlStorageProcessor::actionSchedulerDisposition(uint8_t mask)
{
    switch (mask) {
        case 0x79: return "guest bit, no drain arm";
        case 0x11: case 0x65: return "modelled: post-SVC DispatchIfRequested";
        case 0x2D: return "modelled: wsdvcsr survey, no guest state";
        case kR3WorkStationDeviceStatusMask: return "EMULATOR DECISION: dispatched as 2D (SLIC would ABEND)";
        default: return "unmodelled host routine: recorded, not run";
    }
}

bool As36ControlStorageProcessor::hasUndischargedActions() const
{
    for (const auto& kv : actionCoverage_)
        if (kv.second.undischarged != 0) return true;
    return false;
}

// One-line list of undischarged posts for a stop message, or "" when every
// post ran.
std::string As36ControlStorageProcessor::undischargedActionsSuffix() const
{
    if (!hasUndischargedActions()) return "";
    std::string sb = "; undischarged SVC 0B action posts:";
    for (const auto& kv : actionCoverage_) {
        const ActionMaskCoverage& c = kv.second;
        if (c.undischarged == 0) continue;
        sb += fmt::format(" {:02X} {} x{} (last IAR {:04X}{} task {:04X})", c.mask, actionSchedulerRoutineName(c.mask),
                          c.undischarged, c.lastIar, c.lastMember.empty() ? "" : " " + c.lastMember, c.lastTaskBlock);
    }
    return sb;
}

// The coverage report: the `actions` monitor command and the trace at the
// no-task exit.
void As36ControlStorageProcessor::actionControllerReport(const std::function<void(const std::string&)>& sink) const
{
    sink(fmt::format("action controller status words: NuEmul+1436 (mask < 40) = {:016X}, NuEmul+1440 (mask >= 40) = "
                     "{:016X}; {} NuSetAction(s) queued",
                     actionStatusLow_, actionStatusHigh_, actionQueue_.size()));
    if (actionCoverage_.empty()) {
        sink("no SVC 0B post has been issued");
        return;
    }
    sink("  mask seen coalesced discharged undischarged last-IAR last-task  routine / disposition");
    for (const auto& kv : actionCoverage_) {
        const ActionMaskCoverage& c = kv.second;
        sink(fmt::format("  {:02X}   {:>4} {:>9} {:>10} {:>12}     {:04X}      {:04X}  {}; {}{}", c.mask, c.seen, c.coalesced,
                         c.discharged, c.undischarged, c.lastIar, c.lastTaskBlock, actionSchedulerRoutineName(c.mask),
                         actionSchedulerDisposition(c.mask), c.lastMember.empty() ? "" : " [" + c.lastMember + "]"));
    }
    for (const PendingSetAction& a : actionQueue_)
        sink(fmt::format("  queued: {:02X} from IAR {:04X} task {:04X}", a.mask, a.iar, a.taskBlock));
}

void As36ControlStorageProcessor::traceActionControllerAtNoTaskExit(const std::string& call)
{
    if (actionCoverage_.empty()) return;
    trace_.csp("{}: no task is ready; SVC 0B action-controller coverage at the no-task exit:", call);
    actionControllerReport([this](const std::string& m) { trace_.csp("  {}", m); });
}

// =============================================================================
// The work-station entry scan survey (read-only, no guest state)
// =============================================================================

bool As36ControlStorageProcessor::wsSurveyReadable(int addr, int len) const
{
    return addr > 0 && addr + len <= m_.backingBytes();
}

// The queue header byte is 0x0AFD + halfword DUB[0x28] and its 24-bit head
// pointer the following three bytes.  Elements chain at +0x02; each
// element's +0x0D names a block whose +0x15 must equal this DUB's address.
int As36ControlStorageProcessor::wsIobGet(int dub, int& queueHead)
{
    queueHead = 0;
    if (!wsSurveyReadable(dub, 0x2A)) return 0;
    queueHead = kWsIobQueueSelectorBase + m_.readHalf(dub + 0x28) + 1;
    if (!wsSurveyReadable(queueHead, 3)) {
        queueHead = 0;
        return 0;
    }
    int e = m_.readAddr24(queueHead);
    std::set<int> seen;
    for (int guard = 0; e != 0 && seen.insert(e).second && guard++ < 4096;) {
        if (!wsSurveyReadable(e, 0x10)) return 0;
        int p = m_.readAddr24(e + 0x0D);
        if (wsSurveyReadable(p, 0x18) && m_.readAddr24(p + 0x15) == dub) return e;
        e = m_.readAddr24(e + 0x02);
    }
    return 0;
}

// The station search for one DUB, reporting which successor state it
// selects.  Only two native predicates cannot be evaluated from guest
// storage; they are named in the verdict where reached.
std::string As36ControlStorageProcessor::wsSearchVerdict(const WsScanEntry& r)
{
    if ((r.cls & 0x40) != 0x40) return "wsfstdub70/wsfstdub0 skip: +0A & 40 clear";
    if (r.cls == 0xE0) return "wsfstdub70/wsfstdub0 skip: +0A == E0 (alternate)";
    if (r.eyecatcher != 0xE3E4 && r.eyecatcher != 0xD7E4) return "wsfstdub70 stop: eyecatcher is neither TU nor PU";

    if (r.at27 >= 0xE0) {
        if ((r.at06 & 0x40) != 0) return "wssearch(+27>=E0): +06 & 40 -> wsfchdb1";
        if ((r.at2A & 0x80) != 0) return "wssearch(+27>=E0): +2A & 80 -> wsfchdb1";
        if ((r.at2A & 0x0F) != 0) return "wssearch(+27>=E0): +2A & 0F -> wsfchdb1";
        if (r.iob == 0) return "wssearch(+27>=E0): wsiobget found no queued IOB -> wsfchdb1";
        return "wssearch(+27>=E0): IOB queued -> command dispatch";
    }

    if ((r.at07 & 0x08) != 0 && (r.at2A & 0x80) != 0) return "wssearch(+27<E0): +07&08 and +2A&80 -> wsfchdb1";
    if ((r.at06 & 0x80) != 0) return "wssearch(+27<E0): +06 & 80 -> wsfchdb1";
    if ((r.at7C & 0xC0) != 0)
        return fmt::format("wssearch(+27<E0): +7C & C0 = {:02X} -> wsissu05 (needs NuActiveCtl+0x150 non-null and its "
                           "+0x328 zero){}",
                           r.at7C & 0xC0,
                           (r.at07 & 0x01) == 0
                               ? "; wsissu05 c18c6064 would then call NuEmul::nuerr(114) because +07 & 01 is clear"
                               : "");
    if ((r.at2A & 0x0F) != 0) return "wssearch(+27<E0): +2A & 0F -> wsfchdb1";
    if ((r.at06 & 0x40) != 0) return "wssearch(+27<E0): +06 & 40 -> wsfchdb1";
    if (r.cls >= 0xC1) return "wssearch(+27<E0): +0A >= C1 -> c18c589c arm (undecoded)";
    return "wssearch(+27<E0): -> wsissue";
}

As36ControlStorageProcessor::WsScanEntry As36ControlStorageProcessor::wsScanRead(int a)
{
    WsScanEntry r;
    r.dub = a;
    r.eyecatcher = m_.readHalf(a);
    r.at06 = m_.readByte(a + 0x06);
    r.at07 = m_.readByte(a + 0x07);
    r.cls = m_.readByte(a + devices::WorkStationIob::kOffClass);
    r.unit = m_.readByte(a + devices::WorkStationIob::kOffUnitAddress);
    r.at27 = m_.readByte(a + 0x27);
    r.queueSel = m_.readHalf(a + 0x28);
    r.at2A = m_.readByte(a + 0x2A);
    r.at7C = m_.readByte(a + 0x7C);
    int head;
    r.iob = wsIobGet(a, head);
    r.verdict = wsSearchVerdict(r);
    return r;
}

// Walk one guest queue of unit blocks by its own link displacement.
std::vector<As36ControlStorageProcessor::WsScanEntry> As36ControlStorageProcessor::wsScanQueue(int queue, int linkOffset)
{
    std::vector<WsScanEntry> rows;
    int head = GuestLowStorage::queueHeader(queue);
    if (!wsSurveyReadable(head, 3)) return rows;
    int a = m_.readAddr24(head);
    std::set<int> seen;
    while (a != 0 && seen.insert(a).second && rows.size() < 256) {
        if (!wsSurveyReadable(a, 0x90)) break;
        rows.push_back(wsScanRead(a));
        a = m_.readAddr24(a + linkOffset);
    }
    return rows;
}

void As36ControlStorageProcessor::wsEntryScanSurvey(const std::string& call,
                                                    const std::function<void(const std::string&)>& sink)
{
    uint8_t lockByte = m_.readByte(kWsControllerLockByte);
    bool locked = (lockByte & kWsControllerLockBit) != 0;
    sink(fmt::format("{}: wsentry scan survey (docs/s36/action-2d-wsentry-contract-2026-09-07.md)", call));
    sink(fmt::format("  wsquelck lock guest {:04X} = {:02X}: {}", kWsControllerLockByte, lockByte,
                     locked ? "HELD -> wsentry sets NuActiveCtl+0x138 = 1 and defers to action 17/19"
                            : "free -> wsentry takes it and scans"));
    sink("  request WSCF NuActiveCtl+0xD0[1] is FF until a command sets it, so wsentry takes its scan-all arm: wsfchdub -> "
         "wsfstdub0");

    int q51 = GuestLowStorage::queueHeader(GuestLowStorage::kPrinterUnitBlockQueue);
    int h51 = wsSurveyReadable(q51, 3) ? m_.readAddr24(q51) : 0;
    sink(fmt::format("  wsfstdub0 stage 1: QH{} printer units, head {:04X} = {:06X}{}", GuestLowStorage::kPrinterUnitBlockQueue,
                     q51, h51, h51 == 0 ? "  (empty)" : ""));
    for (const WsScanEntry& r : wsScanQueue(GuestLowStorage::kPrinterUnitBlockQueue, 0x50)) wsScanPrint(sink, r);

    int q2 = GuestLowStorage::queueHeader(kWsIoRequestQueue);
    int h2 = wsSurveyReadable(q2, 3) ? m_.readAddr24(q2) : 0;
    sink(fmt::format("  wsfstdub0 stage 2: QH{} work-station I/O requests, head {:04X} = {:06X}{}", kWsIoRequestQueue, q2, h2,
                     h2 == 0 ? "  (empty)" : ""));
    std::set<int> seen2;
    for (int e = h2, n = 0; e != 0 && seen2.insert(e).second && n < 64; n++) {
        if (!wsSurveyReadable(e, 0x10)) break;
        int dub = m_.readAddr24(e + 0x0D);
        sink(fmt::format("    request {:06X} names DUB {:06X}", e, dub));
        if (wsSurveyReadable(dub, 0x90)) wsScanPrint(sink, wsScanRead(dub));
        e = m_.readAddr24(e + 0x02);
    }

    if (h51 == 0 && h2 == 0)
        sink("  => wsfstdub0 leaves NuActiveCtl+0x120 = 0; wsfchdb2 goes to wsissu90 and the scan ends with no "
             "guest-visible effect");

    sink(fmt::format("  reference - wsfstdub70's QH{} walk (the arm a non-FF request WSCF unit would take):",
                     GuestLowStorage::kSharedUnitBlockQueue));
    for (const WsScanEntry& r : wsScanQueue(GuestLowStorage::kSharedUnitBlockQueue, 0x4B)) wsScanPrint(sink, r);
}

void As36ControlStorageProcessor::wsScanPrint(const std::function<void(const std::string&)>& sink, const WsScanEntry& r)
{
    sink(fmt::format("    DUB {:06X} eye {:04X} unit {:02X} +06 {:02X} +07 {:02X} +0A {:02X} +27 {:02X} +28 {:04X} +2A "
                     "{:02X} +7C {:02X} iob {:06X}",
                     r.dub, r.eyecatcher, r.unit, r.at06, r.at07, r.cls, r.at27, r.queueSel, r.at2A, r.at7C, r.iob));
    sink("        " + r.verdict);
}

// =============================================================================
// SVC 08: Increment System Event Counters
// =============================================================================

// Counter 0-23; the block hangs off tb+49..51, carries "SM" at -61 and holds
// halfword counters from block - 29, saturating at FFFF.
bool As36ControlStorageProcessor::incrementSystemEventCounters(SvcRequest& req)
{
    if ((req.inline1 & 0x80) != 0) {
        trace_.csp("SVC 08: counter {} is outside the 0-23 range NuMiscSvc accepts", req.inline1);
        return true;
    }

    int block = m_.readAddr24(req.taskBlock + TaskBlock::kOffMeasurementBlock);
    if (block == 0) {
        trace_.csp("SVC 08: counter {} - task block +49..51 is zero, so there is no system measurement block and "
                   "NuMiscSvc returns",
                   req.inline1);
        return true;
    }

    if (m_.readHalf(block - 61) != kMeasurementBlockEyecatcher) {
        trace_.csp("SVC 08: {:06X} does not carry the SM eyecatcher at -61", block);
        return true;
    }

    int counter = block - 29 + 2 * req.inline1;
    uint16_t value = m_.readHalf(counter);
    if (value != 0xFFFF) m_.writeHalf(counter, static_cast<uint16_t>(value + 1));
    trace_.csp("SVC 08: counter {} at {:06X} = {}", req.inline1, counter, value + 1);
    return true;
}

// The same structure, for a counter number the SVC 08 entry does not cover.
// Nothing is written unless all three conditions hold.
void As36ControlStorageProcessor::incrementEventCounter(int taskBlock, int counter)
{
    int block = m_.readAddr24(taskBlock + TaskBlock::kOffMeasurementBlock);
    if (block == 0 || m_.readHalf(block - 61) != kMeasurementBlockEyecatcher) return;
    int at = block - 29 + 2 * counter;
    uint16_t value = m_.readHalf(at);
    if (value != 0xFFFF) m_.writeHalf(at, static_cast<uint16_t>(value + 1));
}

// =============================================================================
// SVC 1A: Log Trace Information
// =============================================================================

// The gate is the function trace ID table at guest hex 0250, one bit per
// function ID.  With the bit off the call does nothing; with it on the
// trace buffer's address and entry format are not in this corpus.
bool As36ControlStorageProcessor::logTraceInformation(SvcRequest& req)
{
    constexpr int kFunctionTraceIdTable = 0x0250;

    uint8_t gate = m_.readByte(kFunctionTraceIdTable + (req.inline1 >> 3));
    uint8_t bit = static_cast<uint8_t>(0x80 >> (req.inline1 & 7));
    int length = std::min(static_cast<int>(req.inline3), 30);

    if ((gate & bit) == 0) {
        trace_.csp("SVC 1A: function {:02X} subfunction {:02X} is not enabled in the trace ID table at {:04X} ({:02X} & "
                   "{:02X}) - nothing logged",
                   req.inline1, req.inline2, kFunctionTraceIdTable, gate, bit);
        return true;
    }

    trace_.csp("SVC 1A: function {:02X} IS enabled, so nulogsv4 would write {} byte(s) from XR2 = {:06X} into the resident "
               "trace buffer, whose address and entry format are not established (docs/s36/svc-task-event-queue.md)",
               req.inline1, length, RequestBlock::readXr2Field(m_, req.requestBlock));
    return false;
}

// =============================================================================
// SVC 2E: Time of Day
// =============================================================================

// SA21-9436 3-123: the time of day in timer units, XR2 the low halfword
// and XR1 the high.  One timer unit is 8.192 ms (SC21-7908-3, LY21-0590-04)
// on a 24-hour clock; the clock itself is the host's local time.
bool As36ControlStorageProcessor::timeOfDay(SvcRequest& req)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    auto sinceSecond = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    int ms = static_cast<int>(sinceSecond.count());
    if (ms < 0) ms += 1000;
    double totalMs = ((local.tm_hour * 60.0 + local.tm_min) * 60.0 + local.tm_sec) * 1000.0 + ms;
    long long units = static_cast<long long>(totalMs / kTimerUnitMilliseconds);
    if (units >= kTimerUnitsPerDay) units = kTimerUnitsPerDay - 1;
    int value = static_cast<int>(units);

    int rb = req.requestBlock;
    m_.writeHalf(rb + RequestBlock::kOffXr1Low, static_cast<uint16_t>(value >> 16));
    m_.writeHalf(rb + RequestBlock::kOffXr2Low, static_cast<uint16_t>(value));

    trace_.csp("SVC 2E: time of day {:02}:{:02}:{:02}.{:03} = {} timer unit(s) of 8.192 ms (SC21-7908-3, LY21-0590-04) -> "
               "XR1 = {:04X}, XR2 = {:04X}. The unit and the 24-hour origin are sourced; the CLOCK is the host's local "
               "time, which is emulator policy",
               local.tm_hour, local.tm_min, local.tm_sec, ms, value, (value >> 16) & 0xFFFF, value & 0xFFFF);
    return true;
}

// =============================================================================
// SVC 20: Specific Resource Dequeue
// =============================================================================

// SA21-9436 3-103: rebuild the active share levels of the job's elements
// on one resource after a nested element was dequeued; the last element
// on the job's chain becomes the owner.  No condition is set.
bool As36ControlStorageProcessor::specificResourceDequeue(SvcRequest& req)
{
    int jcb = RequestBlock::readXr1Field(m_, req.requestBlock);
    int resq = RequestBlock::readXr2Field(m_, req.requestBlock);

    trace_.csp("SVC 20: nursdeqsp - rebuild the active share levels of JCB {:06X} on resource queue {:06X}", jcb, resq);

    uint8_t running = 0;
    int last = 0, matched = 0;
    int at = m_.readAddr24(jcb + kJcbAqeQueue);

    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, jcb + kJcbAqeQueue)) return false;

        if (m_.readAddr24(at + AllocationQueueElement::kOffResourceQueue) == resq) {
            running = resourceShareCombine(m_.readByte(at + AllocationQueueElement::kOffRequested), running);

            // The NEP bit is taken from the element's own ACTIVE byte, not
            // from the combination.
            uint8_t active = m_.readByte(at + AllocationQueueElement::kOffActive);
            running = (active & AllocationQueueElement::kNeverEndingProgram) != 0
                          ? static_cast<uint8_t>(running | AllocationQueueElement::kNeverEndingProgram)
                          : static_cast<uint8_t>(running & ~AllocationQueueElement::kNeverEndingProgram);

            m_.writeByte(at + AllocationQueueElement::kOffActive, running);
            trace_.csp("SVC 20: AQE {:06X} active level {:02X} -> {:02X}", at, active, running);
            last = at;
            matched++;
        }

        at = m_.readAddr24(at + AllocationQueueElement::kOffOwnerChain);
    }

    if (last == 0) {
        trace_.csp("SVC 20: JCB {:06X} holds no allocation queue element for resource queue {:06X}. nursdeqsp calls "
                   "NuEmul::nuersvc(90, 0) here (c18928f8) - the supervisor call error path, which this emulator does not "
                   "model (docs/s36/svc-resource-allocation.md)",
                   jcb, resq);
        return false;
    }

    uint8_t owner = static_cast<uint8_t>(m_.readByte(last + AllocationQueueElement::kOffActive) | AllocationQueueElement::kOwner);
    m_.writeByte(last + AllocationQueueElement::kOffActive, owner);
    trace_.csp("SVC 20: {} element(s) rebuilt; AQE {:06X} is now the owner ({:02X})", matched, last, owner);
    return true;
}

// Combine two share bytes into the one that describes holding both: the
// higher level, flags from `a`, and two same-level requests that disagree
// about the extended-level-1 bit escalate to stored level 3.
uint8_t As36ControlStorageProcessor::resourceShareCombine(uint8_t a, uint8_t b)
{
    int la = a & AllocationQueueElement::kLevelMask;
    int lb = b & AllocationQueueElement::kLevelMask;

    if (la > lb) return a;
    if (la < lb) return static_cast<uint8_t>(lb | (a & 0xF0));
    if ((b & 0x02) == 0) return a;
    if (((a ^ b) & 0x0F) == 0) return a;
    return static_cast<uint8_t>(0x03 | (a & 0xF0));
}

// True when the two CANNOT share: levels a and b (stored as level + 1)
// share when their sum is at most 4, and at exactly 4 only when they agree
// on the extended-level-1 bit.
bool As36ControlStorageProcessor::resourceShareConflicts(uint8_t a, uint8_t b)
{
    int sum = (a & AllocationQueueElement::kLevelMask) + (b & AllocationQueueElement::kLevelMask);
    if (sum > 4) return true;
    if (sum < 4) return false;
    return ((a ^ b) & AllocationQueueElement::kExtendedLevel1) != 0;
}

// The task block an element belongs to: the owner, or, for an element
// queued by JCB, the JCB's current task, falling back to the dispatched one.
int As36ControlStorageProcessor::resourceOwnerTaskBlock(int aqe)
{
    int owner = m_.readAddr24(aqe + AllocationQueueElement::kOffOwner);
    if ((m_.readByte(aqe + AllocationQueueElement::kOffActive) & AllocationQueueElement::kQueuedByJcb) == 0) return owner;

    int tb = m_.readAddr24(owner + kJcbCurrentTaskBlock);
    return tb != 0 ? tb : currentTaskBlock_;
}

// While a task holds a critical system resource its dispatching priority
// is raised to the 240 ceiling, the prior priority saved at tb+28.
void As36ControlStorageProcessor::raiseHolderPriority(int tb)
{
    if (!TaskBlock::isTaskBlock(m_, tb)) return;
    uint8_t current = m_.readByte(tb + TaskBlock::kOffPriority);
    if (current >= kCriticalResourceCeiling) return;
    m_.writeByte(tb + TaskBlock::kOffPriorityRequested, current);
    setDispatchingPriority(tb, kCriticalResourceCeiling);
    requeueByPriority(tb, TaskBlock::kChainLastQueue39, static_cast<uint8_t>(kTaskPriorityQueue));
    if (!TaskBlock::isWaiting(m_, tb))
        requeueByPriority(tb, TaskBlock::kChainLastQueue40, static_cast<uint8_t>(kTaskReadyQueue));
    trace_.csp("SVC 21: nuprtup - holder {:04X} raised {:02X} -> F0 (critical-resource ceiling); prior priority saved at "
               "tb+28",
               tb, current);
}

void As36ControlStorageProcessor::restoreHolderPriority(int tb)
{
    if (!TaskBlock::isTaskBlock(m_, tb)) return;
    if (m_.readByte(tb + TaskBlock::kOffPriority) < kCriticalResourceCeiling) return;
    uint8_t saved = m_.readByte(tb + TaskBlock::kOffPriorityRequested);
    setDispatchingPriority(tb, saved);
    requeueByPriority(tb, TaskBlock::kChainLastQueue39, static_cast<uint8_t>(kTaskPriorityQueue));
    if (!TaskBlock::isWaiting(m_, tb))
        requeueByPriority(tb, TaskBlock::kChainLastQueue40, static_cast<uint8_t>(kTaskReadyQueue));
    trace_.csp("SVC 21: nuprtdn - holder {:04X} priority restored to {:02X}", tb, saved);
}

// Fold one owning element into the two running levels: every owner into
// the first, and into the second only those the caller can never outlast
// (never-ending, in a suspend wait, or status bit 0x08), which is what
// makes a conflict Low rather than High (SA21-9436 3-105).
void As36ControlStorageProcessor::resourceShareAccumulate(uint8_t& all, uint8_t& blocking, int aqe, int tb)
{
    uint8_t active = m_.readByte(aqe + AllocationQueueElement::kOffActive);

    bool neverReleased = (active & AllocationQueueElement::kNeverEndingProgram) != 0 ||
                         (m_.readByte(tb + TaskBlock::kOffStat2) & kWaitSuspend) != 0 ||
                         (m_.readByte(tb + TaskBlock::kOffStatus) & 0x08) != 0;

    if (neverReleased) blocking = resourceShareCombine(blocking, active);
    all = resourceShareCombine(all, active);
}

// =============================================================================
// SVC 21: Resource Enqueue/Dequeue
// =============================================================================

// SA21-9436 3-105 to 3-108.  Decide the owner (task block, or the JCB from
// XR1 or tb+21..23); walk the resource queue once, remembering the caller's
// element and accumulating the owners' levels; enqueue or dequeue; walk it
// again granting ownership to everyone who can share and posting the ones
// already waiting; then the condition: Equal if the caller owns the
// resource or chose to wait, Low if the conflict is with an owner that will
// not release, High otherwise.  The device arm on a "PU"/"DA" header is the
// host device manager and is refused.
bool As36ControlStorageProcessor::resourceEnqueueDequeue(SvcRequest& req)
{
    int rb = req.requestBlock;
    int resq = RequestBlock::readXr2Field(m_, rb) & 0x7FFFFF;
    uint8_t inline1 = req.inline1;

    if ((inline1 & kRenqDeviceResource) != 0) {
        uint16_t eye = m_.readHalf(resq);
        if (eye == 0xD7E4 || eye == 0xC4C1) {
            trace_.csp("SVC 21: inline parameter 1 = {:02X} has the device bit (0x04) and the halfword at XR2 {:06X} is "
                       "{:04X} (\"{}\"): nursenqc {} - the Advanced/36's host device manager (NuWsMap::findWs / "
                       "NuFSMEventDevAlloc*), not a System/36 control block "
                       "(docs/s36/svc-resource-allocation.md#the-device-arm)",
                       inline1, resq, eye, eye == 0xD7E4 ? "PU" : "DA",
                       eye == 0xD7E4 ? std::string("c18929c0 calls NuWsMap::findWs")
                                     : fmt::format("c18929e4 switches on DA type {:02X}", m_.readByte(resq + 2)));
            return false;
        }
        trace_.csp("SVC 21: inline parameter 1 = {:02X} has the device bit (0x04) but the halfword at XR2 {:06X} is {:04X}, "
                   "neither PU (D7E4) nor DA (C4C1): nursenqc c18929ac -> nursenqc2 c1892e38, which strips the bit "
                   "(c1892ed0-c1892edc, device class 0 at c1892f04) and runs the architected enqueue on that header",
                   inline1, resq, eye);
        inline1 = static_cast<uint8_t>(inline1 & ~kRenqDeviceResource);
    }

    bool criticalResource = (req.q & kRenqCriticalSystemResource) != 0;

    // The byte the element carries is inline parameter 1 PLUS ONE, which
    // puts the share level in the level+1 form the share test requires.
    uint8_t request = static_cast<uint8_t>(inline1 + 1);
    bool enqueue = (request & kRenqEnqueue) != 0;
    bool byJcb = (request & AllocationQueueElement::kQueuedByJcb) != 0;
    bool nested = (request & AllocationQueueElement::kNested) != 0;

    int tb = req.taskBlock;
    int owner, ownerQueue;
    if (byJcb) {
        owner = (req.q & kRenqJcbInXr1) != 0 ? RequestBlock::readXr1Field(m_, rb)
                                             : m_.readAddr24(tb + TaskBlock::kOffJobControlBlock);
        ownerQueue = owner + kJcbAqeQueue;
    } else {
        owner = tb;
        ownerQueue = owner + TaskBlock::kOffAqeQueue;
    }

    trace_.csp("SVC 21: {} resource queue {:06X} - inline 1 {:02X} ({}level {}{}{}), Q {:02X}, owner {} {:06X}",
               enqueue ? "enqueue on" : "dequeue from", resq, inline1,
               (inline1 & AllocationQueueElement::kNeverEndingProgram) != 0 ? "NEP, " : "",
               inline1 & AllocationQueueElement::kLevelMask,
               (inline1 & AllocationQueueElement::kExtendedLevel1) != 0 ? " extended" : "", nested ? ", nested" : "", req.q,
               byJcb ? "JCB" : "task block", owner);

    if (owner == 0) {
        trace_.csp("SVC 21: the owner block is zero - a queue-by-JCB request whose task block has no JCB at tb+21..23 and "
                   "whose Q-byte did not pass one in XR1");
        return false;
    }

    int headerField = resq - 2;

    // ---- pass one: find the caller's element, accumulate the owners ----
    uint8_t levelAll = 0, levelBlocking = 0;
    int mine = 0;

    int at = m_.readAddr24(headerField);

    // First element only: a never-ending element in front of a task whose
    // id ends 0x09 (the IPL task) re-points the whole operation at that
    // task block.
    if (at != 0 &&
        (m_.readByte(at + AllocationQueueElement::kOffRequested) & AllocationQueueElement::kNeverEndingProgram) != 0 &&
        m_.readByte(tb + TaskBlock::kOffTaskId + 1) == 0x09) {
        owner = GuestLowStorage::kTaskBlock;
        ownerQueue = owner + TaskBlock::kOffAqeQueue;
        trace_.csp("SVC 21: nursenqc2 c1893050 - the queue is headed by a never-ending element and the caller's task id "
                   "ends 09, so the owner becomes task block {:04X} queued by TB",
                   owner);
    }

    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, headerField)) return false;

        if (m_.readAddr24(at + AllocationQueueElement::kOffOwner) == owner) {
            mine = at;
        } else if ((m_.readByte(at + AllocationQueueElement::kOffActive) & AllocationQueueElement::kOwner) != 0) {
            resourceShareAccumulate(levelAll, levelBlocking, at, resourceOwnerTaskBlock(at));
            if (criticalResource && enqueue) raiseHolderPriority(resourceOwnerTaskBlock(at));
        }

        at = m_.readAddr24(at + AllocationQueueElement::kOffResourceChain);
    }

    int aqe;
    if (enqueue) {
        if (!resourceEnqueue(resq, headerField, ownerQueue, owner, request, nested, mine, aqe)) return false;
    } else {
        aqe = 0;
        if (mine != 0) resourceDequeueElement(mine, ownerQueue, request);
        if (criticalResource) restoreHolderPriority(owner);
    }

    // ---- pass two: hand the resource to everyone who can now share ----
    bool low = resourceEvaluateQueue(headerField, levelAll, levelBlocking, (req.q & kRenqWait) != 0);

    uint8_t condition;
    if (!enqueue) {
        condition = mine != 0 ? kPsrEqual : kPsrLow;
        trace_.csp("SVC 21: dequeue {} - condition {:02X}",
                   mine != 0 ? "removed the caller's element" : "found no element for this caller", condition);
    } else if ((m_.readByte(aqe + AllocationQueueElement::kOffActive) & AllocationQueueElement::kOwner) != 0) {
        condition = kPsrEqual;
        trace_.csp("SVC 21: AQE {:06X} owns resource queue {:06X} - Equal", aqe, resq);
    } else if ((req.q & kRenqWait) != 0) {
        // "Control will always return with equal program status condition.
        // The allocation queue element stays enqueued to the queue and
        // ownership is given when the caller shares with all AQEs above
        // him." (3-105)
        setCondition(req, kPsrEqual);
        if ((req.q & kRenqReturnAqeInXr2) != 0) writeIndexRegister(rb, false, aqe, true);

        trace_.csp("SVC 21: AQE {:06X} cannot share - the caller waits on TB_STAT2 {:02X}, resource enqueue (nuwatcb(tb, 16) "
                   "at c1893840)",
                   aqe, kWaitResourceEnqueue);
        enableDispatching("SVC 21", "nuwatcb c18b4bd0");
        taskWaitReturn(tb, kWaitResourceEnqueue, req.r, req.q, "SVC 21");
        return waitAndDispatch(tb, "SVC 21");
    } else {
        // No wait: the element is taken straight back off both queues and
        // freed, and the caller is told why it failed.
        resourceDequeueElement(aqe, ownerQueue, request);
        condition = low ? kPsrLow : kPsrHigh;
        aqe = 0;
        trace_.csp("SVC 21: the resource is held at an incompatible level and the caller did not ask to wait - {}",
                   low ? "Low, the owner is never ending or suspended" : "High");
    }

    setCondition(req, condition);

    if ((req.q & kRenqReturnAqeInXr2) != 0 && (condition & kPsrEqual) != 0 && aqe != 0) {
        writeIndexRegister(rb, false, aqe, true);
        trace_.csp("SVC 21: Q bit 5 - XR2 returns the AQE address {:06X}", aqe);
    }

    dispatchIfRequested("SVC 21");
    return true;
}

// The enqueue half.  Not nested and the caller already holds an element:
// its level is replaced in place.  Nested over an existing element: the old
// one loses ownership and the new one takes the head, remembering the old
// at +17..19.  Otherwise a new element, FIFO.  Everything new also goes
// FIFO on the owner's own AQE queue.
bool As36ControlStorageProcessor::resourceEnqueue(int resq, int headerField, int ownerQueue, int owner, uint8_t request,
                                                  bool nested, int mine, int& aqe)
{
    bool merge = false;

    if (!nested && mine != 0) {
        aqe = mine;
        trace_.csp("SVC 21: the caller already holds AQE {:06X} on this resource - its level is replaced in place (c18931b8)",
                   aqe);
    } else {
        int bytes = nested ? AllocationQueueElement::kNestedBytes : AllocationQueueElement::kBytes;
        aqe = heap_.allocate(bytes);
        if (aqe == 0) {
            trace_.csp("SVC 21: no room for a {}-byte allocation queue element - nuasgncs at {} returned nothing", bytes,
                       nested ? "c18930f0" : "c189317c");
            return false;
        }
        for (int i = 0; i < bytes; i++) m_.writeByte(aqe + i, 0);

        if (nested && mine != 0) {
            queueOperation(headerField, mine, AllocationQueueElement::kChainLastResource, kQueueDequeueFlag);
            uint8_t was = m_.readByte(mine + AllocationQueueElement::kOffActive);
            m_.writeByte(mine + AllocationQueueElement::kOffActive, static_cast<uint8_t>(was & ~AllocationQueueElement::kOwner));
            queueOperation(headerField, aqe, AllocationQueueElement::kChainLastResource, kQueueLifoFlag);
            merge = true;
            trace_.csp("SVC 21: nested - AQE {:06X} steps aside ({:02X} -> {:02X}) and AQE {:06X} takes the head of the queue",
                       mine, was, m_.readByte(mine + AllocationQueueElement::kOffActive), aqe);
        } else {
            queueOperation(headerField, aqe, AllocationQueueElement::kChainLastResource, kQueueFifo);
        }

        queueOperation(ownerQueue, aqe, AllocationQueueElement::kChainLastOwner, kQueueFifo);
    }

    m_.writeHalf(aqe, AllocationQueueElement::kEyecatcher);
    m_.writeAddr24(aqe + AllocationQueueElement::kOffOwner, owner);

    uint8_t active = request;
    if (merge) {
        active = resourceShareCombine(request, m_.readByte(mine + AllocationQueueElement::kOffActive));
        if ((request & AllocationQueueElement::kNeverEndingProgram) != 0) active |= AllocationQueueElement::kNeverEndingProgram;
    }
    m_.writeByte(aqe + AllocationQueueElement::kOffActive, static_cast<uint8_t>(active & ~AllocationQueueElement::kOwner));

    // On the non-device path the REQUESTED byte keeps the level, the
    // extended bit and the nesting bit, and loses 0x80, 0x40 and 0x20.
    m_.writeByte(aqe + AllocationQueueElement::kOffRequested,
                 static_cast<uint8_t>(request & ~(AllocationQueueElement::kOwner | AllocationQueueElement::kNeverEndingProgram |
                                                  AllocationQueueElement::kQueuedByJcb)));

    m_.writeAddr24(aqe + AllocationQueueElement::kOffResourceQueue, resq);
    if (nested) m_.writeAddr24(aqe + AllocationQueueElement::kOffNestedPrevious, mine);

    trace_.csp("SVC 21: AQE {:06X} - owner {:06X}, active {:02X}, requested {:02X}, resource {:06X}", aqe, owner,
               m_.readByte(aqe + AllocationQueueElement::kOffActive), m_.readByte(aqe + AllocationQueueElement::kOffRequested),
               resq);
    return true;
}

// Take one element off both queues and free it.  If it was nested, the
// element it nested over goes back on the resource queue in its place
// (3-106: the caller keeps ownership at its previous level).
void As36ControlStorageProcessor::resourceDequeueElement(int aqe, int ownerQueue, uint8_t request)
{
    (void)request;
    int resq = m_.readAddr24(aqe + AllocationQueueElement::kOffResourceQueue);
    queueOperation(resq - 2, aqe, AllocationQueueElement::kChainLastResource, kQueueDequeueFlag);

    bool nested = (m_.readByte(aqe + AllocationQueueElement::kOffRequested) & AllocationQueueElement::kNested) != 0;
    if (nested) {
        int previous = m_.readAddr24(aqe + AllocationQueueElement::kOffNestedPrevious);
        if (previous != 0) {
            queueOperation(resq - 2, previous, AllocationQueueElement::kChainLastResource, kQueueLifoFlag);
            trace_.csp("SVC 21: AQE {:06X} was nested over {:06X}, which goes back on the head of resource queue {:06X}", aqe,
                       previous, resq);
        }
    }

    queueOperation(ownerQueue, aqe, AllocationQueueElement::kChainLastOwner, kQueueDequeueFlag);

    int bytes = nested ? AllocationQueueElement::kNestedBytes : AllocationQueueElement::kBytes;
    int allocationBytes = GuestHeap::roundedSize(bytes);
    if (heap_.contains(aqe)) heap_.free(aqe, allocationBytes);
    trace_.csp("SVC 21: AQE {:06X} dequeued from resource queue {:06X} and owner queue {:06X}, {} bytes freed{}", aqe, resq,
               ownerQueue, allocationBytes,
               allocationBytes == bytes ? std::string() : fmt::format(" ({}-byte nested element)", bytes));
}

// Release every task-owned allocation queue element during task
// termination: the ordinary dequeue path, repeated from the task's AQE
// queue head until it is empty, each followed by the second pass so a
// waiting task can be posted.
int As36ControlStorageProcessor::releaseTaskResources(int tb, const std::string& call)
{
    int ownerQueue = tb + TaskBlock::kOffAqeQueue;
    int released = 0;

    for (int steps = 0;; steps++) {
        int aqe = m_.readAddr24(ownerQueue);
        if (aqe == 0) break;
        if (!chainStepValid(aqe, steps, ownerQueue)) break;

        if (m_.readHalf(aqe) != AllocationQueueElement::kEyecatcher) {
            trace_.csp("{}: nupterm resource cleanup stops: task {:04X} AQE head {:06X} has eyecatcher {:04X}, expected {:04X}",
                       call, tb, aqe, m_.readHalf(aqe), AllocationQueueElement::kEyecatcher);
            break;
        }
        if (m_.readAddr24(aqe + AllocationQueueElement::kOffOwner) != tb) {
            trace_.csp("{}: nupterm resource cleanup stops: task {:04X} AQE head {:06X} names owner {:06X}", call, tb, aqe,
                       m_.readAddr24(aqe + AllocationQueueElement::kOffOwner));
            break;
        }

        int resq = m_.readAddr24(aqe + AllocationQueueElement::kOffResourceQueue);
        if (resq < 2) {
            trace_.csp("{}: nupterm resource cleanup stops: AQE {:06X} has invalid resource queue {:06X}", call, aqe, resq);
            break;
        }

        uint8_t requested = m_.readByte(aqe + AllocationQueueElement::kOffRequested);
        trace_.csp("{}: nupterm releases task {:04X} AQE {:06X} from resource queue {:06X} (c18a481c-c18a48a4)", call, tb,
                   aqe, resq);
        resourceDequeueElement(aqe, ownerQueue, requested);
        released++;

        uint8_t levelAll = 0, levelBlocking = 0;
        int headerField = resq - 2;
        int at = m_.readAddr24(headerField);
        for (int resourceSteps = 0; at != 0; resourceSteps++) {
            if (!chainStepValid(at, resourceSteps, headerField)) break;
            if ((m_.readByte(at + AllocationQueueElement::kOffActive) & AllocationQueueElement::kOwner) != 0)
                resourceShareAccumulate(levelAll, levelBlocking, at, resourceOwnerTaskBlock(at));
            at = m_.readAddr24(at + AllocationQueueElement::kOffResourceChain);
        }
        resourceEvaluateQueue(headerField, levelAll, levelBlocking, false);
    }

    trace_.csp("{}: nupterm resource cleanup released {} AQE(s) for task {:04X}; tb+53..55={:06X}", call, released, tb,
               m_.readAddr24(ownerQueue));
    return released;
}

// The second walk: every element that does not yet own the resource is
// offered it; if it can share with the running level of everything already
// granted it becomes an owner, and one that has been round before (its
// caller no longer inside its own supervisor call) is POSTED.  Returns
// whether the caller's conflict is with an owner that will not release.
bool As36ControlStorageProcessor::resourceEvaluateQueue(int headerField, uint8_t& levelAll, uint8_t& levelBlocking,
                                                        bool wait)
{
    bool low = false;
    int at = m_.readAddr24(headerField);

    for (int steps = 0; at != 0; steps++) {
        if (!chainStepValid(at, steps, headerField)) break;
        int next = m_.readAddr24(at + AllocationQueueElement::kOffResourceChain);

        uint8_t active = m_.readByte(at + AllocationQueueElement::kOffActive);
        if ((active & AllocationQueueElement::kOwner) != 0) {
            at = next;
            continue;
        }

        int tb = resourceOwnerTaskBlock(at);
        uint8_t requested = m_.readByte(at + AllocationQueueElement::kOffRequested);
        bool seen = (requested & AllocationQueueElement::kEvaluated) != 0;

        if (!resourceShareConflicts(levelAll, active)) {
            m_.writeByte(at + AllocationQueueElement::kOffActive, static_cast<uint8_t>(active | AllocationQueueElement::kOwner));
            if (seen) {
                trace_.csp("SVC 21: AQE {:06X} can share now - task block {:04X} is posted for resource enqueue (nupotcb(tb, "
                           "16) c18936e4)",
                           at, tb);
                postTaskBlock(tb, kWaitResourceEnqueue, "SVC 21");
            } else {
                m_.writeByte(at + AllocationQueueElement::kOffRequested,
                             static_cast<uint8_t>(requested | AllocationQueueElement::kEvaluated));
                trace_.csp("SVC 21: AQE {:06X} takes ownership at level {} (active {:02X})", at,
                           (active & AllocationQueueElement::kLevelMask) - 1, active);
            }
            resourceShareAccumulate(levelAll, levelBlocking, at, tb);
        } else if (!seen) {
            m_.writeByte(at + AllocationQueueElement::kOffRequested,
                         static_cast<uint8_t>(requested | AllocationQueueElement::kEvaluated));
            if (resourceShareConflicts(levelBlocking, active)) {
                low = true;
                trace_.csp("SVC 21: AQE {:06X} conflicts with an owner that is never ending or suspended - Low (c1893744)", at);
            }
            if (wait) resourceShareAccumulate(levelAll, levelBlocking, at, tb);
        } else {
            resourceShareAccumulate(levelAll, levelBlocking, at, tb);
        }

        at = next;
    }

    return low;
}

}  // namespace sim36::processors::controlstorage
