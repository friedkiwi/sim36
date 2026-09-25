// The Advanced/36 control storage processor: task creation, the control
// block factory, user area pages, the task work area allocator, SMFC, the
// print buffer and the control storage transient bodies.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include "Devices/WorkStationController.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <ctime>

#include <fmt/format.h>

#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Processors/ControlStorage/WrkParameterList.h"
#include "Storage/Ebcdic.h"

namespace sim36::processors::controlstorage {

namespace {

// The host's local wall clock, broken down: the clock source for the
// time-of-day transients is emulator policy, the same policy SVC 2E uses.
// Case-insensitive comparison of two ASCII strings.
bool sameIgnoringCase(const std::string& a, const char* b)
{
    std::size_t n = 0;
    while (b[n] != 0) n++;
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::tm localNow()
{
    std::time_t t = std::time(nullptr);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

}  // namespace

// =====================================================================
// task creation: the asynchronous transfer's task builder
// =====================================================================

// Apply the state-changing half of native action 0x1A.  Sub-actions 1 and 5
// enter the size action, action 4 the stop action; start/retrieve own
// additional native objects and do not change this task-shape bit.
bool As36ControlStorageProcessor::applySystemMeasurementAction(uint8_t action, const std::string& call)
{
    bool guestOwnsState = (m_.readByte(0x08B4) & 0x80) != 0;
    switch (action) {
        case 1:
        case 5:
            if (!guestOwnsState) systemMeasurementEnabled_ = true;
            trace_.csp("{}: native $NUSMF action {} -> smfSize; guest 08B4.80 {}, NuEmul+03D5.80 {}", call, action,
                       guestOwnsState ? "set" : "clear", systemMeasurementEnabled_ ? "set" : "clear");
            return true;
        case 4:
            if (!guestOwnsState) systemMeasurementEnabled_ = false;
            trace_.csp("{}: native $NUSMF action 4 -> smfStop; guest 08B4.80 {}, NuEmul+03D5.80 {}", call,
                       guestOwnsState ? "set" : "clear", systemMeasurementEnabled_ ? "set" : "clear");
            return true;
        default:
            trace_.csp("{}: native $NUSMF action {} does not alter the nuptask measurement-block state", call, action);
            return action == 2 || action == 3;
    }
}

// Build a task: one allocation holding the task block, the return ACE
// (Q bit 7), the measurement block (SMF on) and the task's first request
// block; an identifier; a place on queue 39 and on the 0x0F1F chain; and
// the new task made current.  The new task is NOT made ready: it waits on
// TB_STAT2 bit 0x40 until the loader posts its program resident.
int As36ControlStorageProcessor::attachNewTask(int pb, int callerTb, int callerRb, uint8_t q, const std::string& call,
                                               int& rb)
{
    constexpr uint8_t kReturnToCaller = 0x01;   // Q bit 7

    rb = 0;

    int requestUnits = requestBlockUnits(pb, callerRb);
    int aceUnits = (q & kReturnToCaller) != 0 ? kReturnAceUnits : 0;
    int measurementUnits = systemMeasurementBlockUnits(call);
    int units = requestUnits + kTaskBlockUnits + aceUnits + measurementUnits;

    int block = heap_.allocate(units * GuestHeap::kGranularity, call + " nuptask TB/ACE/RB");
    if (block == 0) {
        trace_.csp("{}: nuptask asked nuasgncs for {} unit(s) - {} for the task block, {} for the request block, {} for a "
                   "return ACE, {} for a measurement block - and the system queue space has no room",
                   call, units, kTaskBlockUnits, requestUnits, aceUnits, measurementUnits);
        return 0;
    }

    // The whole allocation is cleared.
    for (int i = 0; i < units * GuestHeap::kGranularity; i++) m_.writeByte(block + i, 0);

    int tb = block;
    bool internalAttach = q == kInternalAttachQByte;

    // ---- the identifier, and the priority floor -----------------------
    // The internal-attach arm gives the task floor 0xF0 and never looks at
    // WR5; every other caller takes the floor from the CALLER's tb+20 and
    // the identifier from WR5 when the caller supplied one.
    int id;
    int requestedId = 0;
    if (internalAttach) {
        m_.writeByte(tb + TaskBlock::kOffPriorityFloor, kInternalAttachPriority);
        id = 0;
    } else {
        m_.writeByte(tb + TaskBlock::kOffPriorityFloor, m_.readByte(callerTb + TaskBlock::kOffPriorityFloor));
        id = RequestBlock::readWr(m_, callerRb, 5);
        requestedId = id;
        m_.writeHalf(tb + TaskBlock::kOffTaskId, static_cast<uint16_t>(id));
    }

    if (id == 0) {
        id = allocateTaskId(call);
        m_.writeHalf(tb + TaskBlock::kOffTaskId, static_cast<uint16_t>(id));
    }

    // The identifier goes back to the caller in WR5, and WR7, when the
    // caller set one, overrides the priority floor.
    if (!internalAttach) {
        RequestBlock::writeWr(m_, callerRb, 5, static_cast<uint16_t>(id));
        uint16_t wr7 = RequestBlock::readWr(m_, callerRb, 7);
        if (wr7 != 0) m_.writeByte(tb + TaskBlock::kOffPriorityFloor, static_cast<uint8_t>(wr7));
    }

    // ---- the task block's fixed fields -------------------------------
    uint8_t floor = m_.readByte(tb + TaskBlock::kOffPriorityFloor);
    m_.writeHalf(tb + TaskBlock::kOffState, kNewTaskState);
    m_.writeByte(tb + kTbLongWaitMark, 250);
    m_.writeByte(tb + TaskBlock::kOffPriority, floor);
    m_.writeByte(tb + TaskBlock::kOffStatus, kNewTaskStatus);
    m_.writeHalf(tb, TaskBlock::kEyecatcher);

    // A program block that is neither reusable nor whatever bit 0x20 marks
    // is claimed by the new task: the second writer of pb+37..39.
    uint8_t pbFlags = m_.readByte(pb + ProgramBlock::kOffFlags57);
    if ((pbFlags & 0x30) == 0) {
        m_.writeAddr24(pb + ProgramBlock::kOffOwningTask, tb);
        trace_.csp("{}: nuptask claims program block {:04X} for the new task - pb+57 = {:02X} has neither bit 20 nor bit "
                   "10, so pb+37..39 = {:04X} (c18a6074)",
                   call, pb, pbFlags, tb);
    }

    // ---- the cursor over the rest of the allocation -------------------
    int cursor = tb + kTaskBlockBytes;

    if (aceUnits != 0) {
        // The element describes the CALLER (the current blocks have not been
        // switched yet), so the element the caller will later wait on
        // carries the caller's registers and task block, and hangs off
        // tb+17..19.  An element built for a privileged requester (caller
        // rb+19 bit 0 clear) also carries flag 0x40.
        ActionControlElement::build(m_, cursor, callerRb, callerTb, q);
        if ((m_.readByte(callerRb + RequestBlock::kOffPrivilege) & 0x01) == 0)
            m_.writeByte(cursor + ActionControlElement::kOffFlags,
                         static_cast<uint8_t>(m_.readByte(cursor + ActionControlElement::kOffFlags) | 0x40));
        m_.writeAddr24(tb + TaskBlock::kOffReturnAce, cursor);
        trace_.csp("{}: Q bit 7 asks for control back, so nuptask builds an action control element at {:04X} and stores it "
                   "at tb+17..19 (c18a609c). nubldacn ace+28 includes privileged-requester bit 0x40 when caller rb+8 bit 0 "
                   "is clear (c18e4dc8)",
                   call, cursor);
        cursor += ActionControlElement::kSize;
    }

    if (measurementUnits != 0) {
        // The stored pointer is intentionally biased: the reader subtracts
        // two, reads the length at +4 (block+62), then subtracts that length
        // from block+64 to recover the record origin.
        int measurementBlock = cursor;
        m_.writeHalf(measurementBlock, 0xE2D4);   // EBCDIC "SM"
        m_.writeHalf(measurementBlock + 62, 64);
        m_.writeAddr24(tb + TaskBlock::kOffMeasurementBlock, measurementBlock + 61);
        trace_.csp("{}: nuptask builds SM block {:04X}-{:04X}, length 0040; tb+49..51 = {:04X} (block+61)", call,
                   measurementBlock, measurementBlock + 63, measurementBlock + 61);
        cursor += measurementUnits * GuestHeap::kGranularity;
    }

    // ---- the new task's first request block ---------------------------
    rb = cursor;
    buildRequestBlock(rb, requestUnits, pb, tb, callerRb);

    // The new task's root request block has no caller to return to: with
    // Q bit 7 the requester is handed back through the return ACE at
    // tb+17..19, not through the rb prev-chain, so rb+3..5 is zeroed.  This
    // is what sends the root exit to the task-termination arm instead of
    // resuming a dead frame.
    m_.writeAddr24(rb + RequestBlock::kOffPrevious, 0);

    // ---- the two queues ----------------------------------------------
    // Queue 39, chain 27, flags C7: the same priority insert the task post
    // and SVC 24 make on the task priority queue.
    queueOperation(GuestLowStorage::queueHeader(kTaskPriorityQueue), tb, TaskBlock::kChainLastQueue39, kTaskPriorityFlags);
    // A plain FIFO append onto the task block at guest 0x0F00's own
    // tb+29..31.
    queueOperation(GuestLowStorage::kTaskBlock + kTbChildChainLast - 2, tb, kTbChildChainLast, 0x00);

    // ---- and the new task becomes the current one ---------------------
    // No redispatch is requested: the machine's dispatcher re-runs at the
    // top of its own loop and selects from the ready list, which this task
    // is deliberately not on.
    requestBlockBeforeAttach_ = currentRequestBlock_;
    currentTaskBlock_ = tb;
    currentRequestBlock_ = rb;

    trace_.csp("{}: nuptask - task {:04X} id {:04X} (requested WR5 {:04X}), priority {:02X}, request block {:04X} ({} "
               "unit(s)); tb+4..5 = {:04X}, so it is WAITING on TB_STAT2 bit 40 until nucready posts it (c1897758). Queued "
               "on 39 and on the chain at guest {:04X}; NuEmul[0x5C0] keeps the outgoing request block {:04X}",
               call, tb, id, requestedId, floor, rb, requestUnits, kNewTaskState,
               GuestLowStorage::kTaskBlock + kTbChildChainLast, requestBlockBeforeAttach_);
    return tb;
}

// The identifier counter is stepped by TWO, a 16-bit value of zero is
// skipped, and the candidate is looked up on queue header 39 by the task
// identifier at tb+2..3; a hit means the identifier is in use.  The IPL
// task's identifier (0x0009, odd) is seeded, not allocated, and cannot
// collide with this counter.
int As36ControlStorageProcessor::allocateTaskId(const std::string& call)
{
    for (int tries = 0; tries < 0x10000; tries++) {
        taskIdCounter_ = (taskIdCounter_ + 2) & 0xFFFFFFF;
        int candidate = taskIdCounter_ & 0xFFFF;
        if (candidate == 0) continue;
        if (findTaskById(candidate, 0) != 0) continue;
        trace_.csp("{}: nuptask allocates task id {:04X} from the counter at NuEmul[0x454] (c18a5f54)", call, candidate);
        return candidate;
    }

    trace_.csp("{}: every one of the 32 767 even task identifiers is on queue 39 - nuptask's loop at c18a5fb0 does not "
               "terminate on the real machine either",
               call);
    return 0;
}

// Four units are added to a task's allocation when the system measurement
// facility is on.  Nothing on this machine starts the facility unless the
// native action does, so the bit is normally clear and no block is built.
int As36ControlStorageProcessor::systemMeasurementBlockUnits(const std::string& call)
{
    int units = systemMeasurementEnabled_ ? 4 : 0;
    trace_.csp("{}: nuptask reads NuEmul+03D5.80 {}; measurement allocation {} unit(s)", call,
               systemMeasurementEnabled_ ? "set" : "clear", units);
    return units;
}

// The request block builder both arms of a transfer share: +3..5 the
// CALLER's request block, +47 zero, +2 the length in 16-byte units, +40 no
// map table entries yet, +41..43 the program block, +49..51 FFFFFF, and the
// task relinked to it.  Which task block is relinked is the one thing that
// differs between the two callers.
void As36ControlStorageProcessor::buildRequestBlock(int rb, int units, int pb, int tb, int callerRb)
{
    m_.writeHalf(rb, GuestLowStorage::kEyeRequestBlock);
    m_.writeByte(rb + RequestBlock::kOffLengthUnits, static_cast<uint8_t>(units));
    m_.writeAddr24(rb + RequestBlock::kOffPrevious, callerRb);
    m_.writeByte(rb + kRequestBlockTransientMark, 0);
    m_.writeByte(rb + RequestBlock::kOffMapEntryCount, 0);
    m_.writeAddr24(rb + RequestBlock::kOffProgramBlock, pb);
    m_.writeAddr24(rb + RequestBlock::kOffSentinel, 0xFFFFFF);
    m_.writeAddr24(tb + TaskBlock::kOffRequestBlock, rb);
}

// =====================================================================
// SVC 31 ATASK, SVC 32 DTASK, and the control-block factory
// =====================================================================

// SVC 31, ATASK (SA21-9436 3-126): "assigns and formats a program block in
// the system queue space, and allocates a swap area in the task work area.
// A task work area allocate failure results in a return to the caller with
// the PSR set to high.  Otherwise, the PSR is returned set to equal."
// Q-byte bit 3 "allocate maximum swap area", WR6 "the region size in the
// high byte and the main storage size in the low byte", XR1 out.  The name
// is a false friend: ATASK assigns task-work-area-backed storage, it does
// not attach a task.
bool As36ControlStorageProcessor::attachTask(SvcRequest& req)
{
    constexpr uint8_t kMaximumSwapArea = 0x10;   // Q-byte bit 3
    constexpr uint8_t kWaitForSpace = 0x01;      // Q-byte bit 7

    int rb = req.requestBlock;
    int wr6 = RequestBlock::readWr(m_, rb, 6);
    int regionPages = (wr6 >> 8) & 0xFF;
    int mainStoragePages = wr6 & 0xFF;

    uint8_t flags = ControlBlock::kFlagSwapArea;
    if ((req.q & kMaximumSwapArea) != 0) flags |= ControlBlock::kFlagMaximumSwapArea;

    trace_.csp("SVC 31: ATASK, WR6 = {:04X} - region {} page(s), main storage {} page(s); nucbldsb flags {:02X}{}", wr6,
               regionPages, mainStoragePages, flags,
               (flags & ControlBlock::kFlagMaximumSwapArea) != 0 ? " (Q bit 3, maximum swap area)" : "");

    int failure;
    int pb = buildControlBlock(ControlBlock::kTypeProgramBlock, flags, regionPages, mainStoragePages, req.taskBlock, 0,
                               "SVC 31", failure);

    if (pb == 0) {
        if (failure == 0) {
            // The system queue space allocator has no failure return: an
            // exhausted pool is a machine stop, not a condition the caller
            // can see, so the call is refused rather than answered.
            trace_.csp("SVC 31: the system queue space has no room for a 64-byte program block. nuasgncs (c18e2c70) has "
                       "no failure return - it reaches nuerabt code 60, a machine stop - so there is no PSR condition "
                       "this can be reported as");
            return false;
        }
        return attachTaskFailed(req, failure, (req.q & kWaitForSpace) != 0);
    }

    // PB+18 is the initially resident count; PB+16 is the larger virtual
    // region backed by PB+24's task-work-area allocation.  Consume real MSP
    // frames only for the resident pages.  If SVC 12 later makes more pages
    // resident, ensureModuleStoragePages extends or relocates this extent
    // before nucratr publishes the new mappings.  BASIC's 28K/10K case is the
    // regression for that ordering: its four-page growth must not alias a
    // transient allocated after these initial ten pages.
    if (mainStoragePages != 0) {
        int storage = allocateModuleStorage(mainStoragePages, "SVC 31 ATASK resident pages");
        if (storage == 0) {
            deleteControlBlock(pb, "SVC 31 ATASK storage failure");
            trace_.csp("SVC 31: no real MSP page frames are available for the program block's {} initially resident "
                       "page(s) ({}-page virtual region)", mainStoragePages, regionPages);
            return false;
        }
        moduleStorage_[pb] = storage;
        trace_.csp("SVC 31: ATASK program block {:06X} has {} initially resident page(s) at real {:05X}; its "
                   "{}-page virtual region can acquire more backing through SVC 12",
                   pb, mainStoragePages, storage, regionPages);
    }

    // psr &= ~0x06; psr |= 0x01: "the PSR is returned set to equal".
    setCondition(req, kPsrEqual);

    // XR1 is written as a halfword and a prefix byte in the request block's
    // save area, which the dispatcher reloads on the way out.
    writeIndexRegister(rb, true, pb, true);

    // The block's first reference, and the ready page count squared with
    // the requested one.
    activateControlBlock(pb, "SVC 31");
    m_.writeHalf(pb + ProgramBlock::kOffPagesReady, static_cast<uint16_t>(m_.readHalf(pb + ProgramBlock::kOffPageCount)));

    trace_.csp("SVC 31: program block {:06X} formatted and returned in XR1 - PSR Equal, use count 1, pb+20 = pb+18 = {}", pb,
               m_.readHalf(pb + ProgramBlock::kOffPageCount));
    return true;
}

// The failure arm: Q bit 7 set takes a general wait unconditionally with
// the failure code as its mask; a waitable failure code without bit 7
// depends on a native word nothing on this machine sets, so the condition
// is returned.  The manual says High; the code writes Low, and the code is
// followed.
bool As36ControlStorageProcessor::attachTaskFailed(SvcRequest& req, int failure, bool waitRequested)
{
    constexpr int kWaitableFailureCode = 0x4000;   // (code & 0xFF00)

    if (waitRequested) {
        trace_.csp("SVC 31: the swap area could not be allocated and the Q-byte's bit 7 is set, which takes nuatask to "
                   "nugwaitc (c18bb5c4) with the mask {:04X}. SA21-9436 3-126 says bits 4-7 must be zero, so a guest that "
                   "reaches this is outside the architected call",
                   failure & 0xFFFF);
        return supervisorGeneralWait(req.taskBlock, failure & 0xFFFF, "SVC 31");
    }

    if ((failure & 0xFF00) == kWaitableFailureCode) {
        trace_.csp("SVC 31: nutwl failure code {:04X} is the one nuatask tests for at c18bb5a8, so whether it waits depends "
                   "on NuEmul[0xFF8] - the word NuTwaHeap::getHeap tests at c18b885c before it calls extendHeap. Nothing in "
                   "this machine extends the task work area, so the word is zero and c18bb5b8 returns the condition instead "
                   "of waiting",
                   failure & 0xFFFF);
    }

    setCondition(req, kPsrLow);
    trace_.csp("SVC 31: no swap area - PSR Low. NOTE: SA21-9436 3-126 says this condition is HIGH; nuatask sets psr &= ~0x05 "
               "| 0x02 at c18bb5e8, which is the same Low that SVC 2C's documented failure sets. The code is followed, the "
               "manual is recorded (docs/s36/svc-program-block-lifecycle.md)");
    return true;
}

// SVC 32, DTASK (SA21-9436 3-127): "issued whenever there has been a
// failure to attach a task and the ATASK is already completed ... dequeues
// and frees the program block and deallocates any associated swap area".
// The whole of DTASK is the deactivate: drop one reference and delete the
// block when the last one goes.  XR1 is used as a real address.
bool As36ControlStorageProcessor::detachTask(SvcRequest& req)
{
    int rb = req.requestBlock;
    int pb = RequestBlock::readXr1Field(m_, rb);

    if (req.q != 0) {
        trace_.csp("SVC 32: the Q-byte is {:02X} and SA21-9436 3-127 requires zero; nudtask does not test it and neither "
                   "does this",
                   req.q);
    }

    uint16_t eye = pb != 0 && pb + 2 <= m_.backingBytes() ? m_.readHalf(pb) : static_cast<uint16_t>(0);
    if (eye != GuestLowStorage::kEyeProgramBlock && eye != GuestLowStorage::kEyeSystemBlock) {
        // A GUARD, not a recovered rule: the machine accepts whatever it is
        // handed, and freeing an arbitrary address would corrupt guest
        // storage.
        trace_.csp("SVC 32: XR1 = {:06X} has eyecatcher {:04X}, which is neither PB ({:04X}) nor SB ({:04X}). nucdactv does "
                   "not check - this is a guard, not a recovered rule",
                   pb, eye, GuestLowStorage::kEyeProgramBlock, GuestLowStorage::kEyeSystemBlock);
        return false;
    }

    trace_.csp("SVC 32: DTASK on program block {:06X}", pb);
    deactivateControlBlock(pb, "SVC 32");
    return true;
}

// The factory behind SVC 31, SVC 10 and every work space in the machine.
// Arguments (type, flags, arg3, arg4, tb, entry): +8 the flags, +4 the
// type, +16 arg3, +0 the eyecatcher, +18 and +42 arg4, +22 0xFFFF,
// +24..26 the swap area reference + 1, +37..39 the task block's guest
// address, and for a program block built from a transfer table entry
// +48..50 the entry's sector and +52 its attribute.  The finished block is
// queued LIFO on the head its type selects.
int As36ControlStorageProcessor::buildControlBlock(uint8_t type, uint8_t flags, int arg3, int arg4, int taskBlock,
                                                   int entry, const std::string& call, int& failureCode)
{
    failureCode = 0;

    // Type 1 and type 0xCF get the 64-byte "PB"; anything else the 48-byte
    // "SB".
    bool programBlock = type == ControlBlock::kTypeProgramBlock || type == ControlBlock::kTypeProgramBlockAlt;
    int bytes = programBlock ? ProgramBlock::kBytes : StorageBlock::kBytes;
    uint16_t eyecatcher = programBlock ? GuestLowStorage::kEyeProgramBlock : GuestLowStorage::kEyeSystemBlock;

    int block = heap_.allocate(bytes, call + (programBlock ? " program block" : " storage block"));
    if (block == 0) return 0;
    for (int i = 0; i < bytes; i++) m_.writeByte(block + i, 0);

    // Flags bit 0x40 asks for a task work area block: (arg3 * 8) + 2
    // sectors, or a flat 256 + 2 when flags bit 0x10 is on.
    int swapArea = 0, swapSectors = 0;
    if ((flags & ControlBlock::kFlagSwapArea) != 0) {
        swapSectors = swapAreaSectors(flags, arg3);
        swapArea = twa_.allocate(swapSectors);
        if (swapArea < 0) {
            // The block is given back and the caller gets 0x800000 | the
            // allocator's code.
            heap_.free(block, bytes);
            failureCode = kTranslatedBit | ControlBlock::kTaskWorkAreaFullCode;
            trace_.csp("{}: no {} sector(s) of task work area for a swap area; nucbldsb frees the block and returns 0x800000 "
                       "| code = {:06X} (c18bc0cc)",
                       call, swapSectors, failureCode);
            return 0;
        }
        trace_.csp("{}: swap area of {} sector(s) at task work area relative sector {} (nutwl, c18bbfb8); pb+24..26 takes "
                   "reference + 1",
                   call, swapSectors, swapArea);
    }

    m_.writeByte(block + StorageBlock::kOffFlags, flags);
    m_.writeByte(block + StorageBlock::kOffType, type);
    m_.writeHalf(block + StorageBlock::kOffSizePages, static_cast<uint16_t>(arg3));
    m_.writeHalf(block + StorageBlock::kOffEyecatcher, eyecatcher);
    m_.writeHalf(block + StorageBlock::kOffPagesMapped, static_cast<uint16_t>(arg4));
    m_.writeHalf(block + ControlBlock::kOffPagesMappedCopy, static_cast<uint16_t>(arg4));
    m_.writeHalf(block + ControlBlock::kOffMinusOne, 0xFFFF);

    if ((flags & ControlBlock::kFlagSwapArea) != 0) m_.writeAddr24(block + StorageBlock::kOffDiskAddress, swapArea + 1);

    m_.writeAddr24(block + ProgramBlock::kOffOwningTask, taskBlock);

    if (programBlock && entry != 0) {
        m_.writeAddr24(block + ProgramBlock::kOffSector, m_.readAddr24(entry));
        m_.writeByte(block + ProgramBlock::kOffAttribute, m_.readByte(entry + 4));
    }

    int head = queueHeadFor(block, call);
    if (head != 0) {
        queueOperation(head - 2, block, ControlBlock::kChainLast, ControlBlock::kQueueLifo);
        trace_.csp("{}: control block {:06X} queued LIFO on the head at {:04X} (nucbldsb -> nuquecs, c18bc088)", call, block,
                   head);
    }

    trace_.csp("{}: {} at {:06X} - type {:02X}, flags {:02X}, +16 = {}, +18 = +42 = {}, +37..39 = task block {:06X}", call,
               programBlock ? "program block" : "storage block", block, type, flags, arg3, arg4, taskBlock);
    return block;
}

// =====================================================================
// SVC 12 Get Page, SVC 13 Maintain User Area Pages
// =====================================================================

// SVC 12: "expands the user's main storage size up to the region size",
// and returns "the last logical address plus 1" through a pointer the
// caller passes in its two inline parameters, resolved against the
// instruction fetch prefix.  The region grows only when the JCB's counter
// is clear and the program block carries both growable bits; the answer
// is a halfword whose high byte is (load page + pages) * 8.
bool As36ControlStorageProcessor::getPage(SvcRequest& req)
{
    int logical = (req.inline1 << 8) | req.inline2;
    uint8_t piar = m_.readByte(req.requestBlock + RequestBlock::kOffPiar);
    int answerAt;
    if (!m_.resolve(static_cast<uint16_t>(logical), piar, machine::MachineState::kAtrTaskGroup0, true, answerAt)) {
        trace_.csp("SVC 12: the answer address {:04X} under piar {:02X} does not resolve", logical, piar);
        return false;
    }

    int jcb = m_.readAddr24(req.taskBlock + JobControlBlock::kTaskBlockPointer);
    int pb = m_.readAddr24(req.requestBlock + RequestBlock::kOffProgramBlock);

    if (jcb == 0 || pb == 0) {
        // A dispatched task always has both on the machine; with a null JCB
        // the region fields would be read out of low storage.  Refusing
        // names the missing block instead of answering out of it.
        trace_.csp("SVC 12: task block {:04X} has JCB {:06X} and request block {:04X} has program block {:06X}; both are "
                   "needed and the region fields live in the JCB (docs/s36/guest-structures.md)",
                   req.taskBlock, jcb, req.requestBlock, pb);
        return false;
    }

    int loadPage = ProgramBlock::loadPage(m_, pb);
    int pages;

    bool grow = (m_.readHalf(jcb + JobControlBlock::kOffGrowthCounter) & 0x7F) == 0 &&
                (m_.readByte(pb + ProgramBlock::kOffFlags) & kGrowableFlags) == kGrowableFlags;

    if (grow) {
        pages = ProgramBlock::pageCount(m_, pb) & 0xFF;
        int region = JobControlBlock::regionPages(m_, jcb);
        if (pages < region) {
            // The smaller of the region size and 32 - load page is the only
            // reading consistent with a 32-entry translation window.
            pages = std::min(region, 32 - loadPage);
            if (!ensureModuleStoragePages(pb, pages, "SVC 12")) {
                trace_.csp("SVC 12: cannot grow program block {:06X} backing to {} page(s); the old page count and ATRs "
                           "remain in force",
                           pb, pages);
                return false;
            }
            m_.writeHalf(pb + ProgramBlock::kOffPageCount, static_cast<uint16_t>(pages));
        }
        JobControlBlock::setCurrentPages(m_, jcb, pages);
    } else {
        pages = JobControlBlock::currentPages(m_, jcb);
    }

    // Both arms end with the same two stores, and the high byte wraps on
    // the real machine exactly as it does here.
    int end = ((pages & 0x1F) + (loadPage & 0x1F)) * 8;
    m_.writeByte(answerAt, static_cast<uint8_t>(end));
    m_.writeByte(answerAt + 1, 0);

    int count = ProgramBlock::pageCount(m_, pb);
    // The non-grow arm can still observe a page count enlarged by the
    // region-maintenance path before this call.  Enforce the backing
    // invariant on both arms, not only where this routine itself changes
    // PB+18.
    if (!ensureModuleStoragePages(pb, count, "SVC 12")) {
        trace_.csp("SVC 12: cannot provide backing for program block {:06X}'s {} published page(s); ATRs are unchanged",
                   pb, count);
        return false;
    }
    m_.writeHalf(pb + ProgramBlock::kOffPagesReady, static_cast<uint16_t>(count));
    m_.writeHalf(pb + ProgramBlock::kOffPagesThird, static_cast<uint16_t>(count));

    buildTranslationRegisters(req.taskBlock);

    trace_.csp("SVC 12: {} region at JCB {:04X}, load page {}, {} page(s) -> last logical address + 1 = {:04X} stored at "
               "{:06X} (inline {:02X} {:02X}, piar {:02X})",
               grow ? "grew the" : "did not grow the", jcb, loadPage, pages, (end & 0xFF) << 8, answerAt, req.inline1,
               req.inline2, piar);
    return true;
}

// SVC 13, six functions in inline parameter 1.  On the Advanced/36
// pinning, unpinning, creating and deleting a swappable region are no-ops;
// only "change the region" leaves a trace, the new size in pages at
// JCB+137.  The manual agrees: "functions requested by hex 00, 02, and 05
// cannot fail".  Every function returns WR6 = the current number of pages
// in the user area, guest 0x0845.
bool As36ControlStorageProcessor::maintainUserAreaPages(SvcRequest& req)
{
    constexpr uint8_t kChangeRegion = 0x04;
    constexpr uint8_t kHighestFunction = 0x05;

    // The low THREE bits of the PSR are cleared before the function code is
    // even fetched.
    uint8_t psr = static_cast<uint8_t>(m_.readByte(req.requestBlock + RequestBlock::kOffPsr) & 0xF8);

    if (req.inline1 > kHighestFunction) {
        m_.writeByte(req.requestBlock + RequestBlock::kOffPsr, static_cast<uint8_t>(psr | kPsrLow));
        trace_.csp("SVC 13: function {:02X} is above the highest defined (05) - Low", req.inline1);
        return true;
    }

    int block = RequestBlock::readXr1Field(m_, req.requestBlock);

    if (req.inline1 == kChangeRegion) {
        if (block == 0) {
            // With XR1 zero the new size would land in low storage; refuse
            // instead.
            trace_.csp("SVC 13: function 04 changes the region of the JCB in XR1, which is zero; the new size would land in "
                       "low storage");
            return false;
        }
        m_.writeByte(block + JobControlBlock::kOffRegionPages, static_cast<uint8_t>(RequestBlock::readWr(m_, req.requestBlock, 6)));
    }

    m_.writeByte(req.requestBlock + RequestBlock::kOffPsr, static_cast<uint8_t>(psr | kPsrEqual));
    uint16_t userPages = m_.readHalf(GuestLowStorage::kUserAreaPages);
    RequestBlock::writeWr(m_, req.requestBlock, 6, userPages);

    trace_.csp("SVC 13: function {:02X} ({}), XR1 = {:06X} - Equal, WR6 = {} user area page(s) from guest {:04X}",
               req.inline1, userAreaFunction(req.inline1), block, userPages, GuestLowStorage::kUserAreaPages);
    return true;
}

const char* As36ControlStorageProcessor::userAreaFunction(uint8_t f)
{
    switch (f) {
        case 0x00: return "report page count";
        case 0x01: return "pin";
        case 0x02: return "unpin";
        case 0x03: return "create region";
        case 0x04: return "change region";
        case 0x05: return "delete region";
        default: return "?";
    }
}

// =====================================================================
// the task work area allocator: SVC 33 TWAL, SVC 34 DTWAL, SVC 35 WRK
// =====================================================================

// Walk the headers, best-fit over each one's free element chain, and
// carve the request off the END of the element that wins.  Three things
// are the machine's and copied because they are observable: the walk
// stops at the first header whose base identifier is 254 or more; the
// smallest run that fits wins; and an exact fit dequeues the element while
// a partial one shrinks it in place.  First-wins among equal sizes is
// emulator policy.
int As36ControlStorageProcessor::allocateTaskWorkArea(int sectors, std::string& why)
{
    ensureTaskWorkArea();
    why.clear();
    int at = m_.readAddr24(GuestLowStorage::queueHeader(TaskWorkAreaQueue::kAnchorHeader));
    int bestLink = 0, bestSize = 0x10000;

    for (int steps = 0; at != 0 && steps < 256; steps++) {
        if (m_.readByte(at + TaskWorkAreaQueue::kOffBaseIdentifier) >= TaskWorkAreaQueue::kFirstReservedBase) break;
        if ((m_.readByte(at + TaskWorkAreaQueue::kOffFlags) & TaskWorkAreaQueue::kFlagCheckExtent) != 0) {
            why = fmt::format("QH {:04X} has +6 bit 0x80 set, which makes findElement call NuTwaHeap::checkExtent (c18b8b98) "
                              "before it searches; checkExtent is not decoded",
                              at);
            return 0;
        }

        // The cursor starts at header + 5, so the chain head at +7..9 and
        // an element's +2..4 are the same field.
        int link = at + TaskWorkAreaElement::kHeaderAsElement;
        for (int hops = 0; hops < 4096; hops++) {
            int element = m_.readAddr24(link + TaskWorkAreaElement::kOffNext);
            if (element == 0) break;
            int size = m_.readHalf(element + TaskWorkAreaElement::kOffSectors);
            if (size >= sectors && size < bestSize) {
                bestSize = size;
                bestLink = link;
            }
            link = element;
        }
        if (bestLink != 0) break;   // the walk stops at the first header that answers
        at = m_.readAddr24(at + TaskWorkAreaQueue::kOffNext);
    }

    if (bestLink == 0) {
        if (why.empty())
            why = "no free element of that many sectors on any allocatable QH block. At IPL that is the machine's own "
                  "answer, not a gap: csipl builds only base FE and base FF, both of which getHeap's c18b8700 bound "
                  "excludes, and the ordinary extents are created later by NuTwaHeap::extendHeap - which attaches an SSP "
                  "task through nuptaskc (c18b8ab4), gated on guest 0x08AB bit 0x40 and 0x08B1 bit 0x80, and so needs the "
                  "task dispatcher";
        return 0;
    }

    int best = m_.readAddr24(bestLink + TaskWorkAreaElement::kOffNext);
    int remaining = bestSize - sectors;
    int answer = (m_.readByte(best + TaskWorkAreaElement::kOffBaseIdentifier) << 16) |
                 ((m_.readHalf(best + TaskWorkAreaElement::kOffDisplacement) + remaining) & 0xFFFF);

    if (remaining == 0)
        m_.writeAddr24(bestLink + TaskWorkAreaElement::kOffNext, m_.readAddr24(best + TaskWorkAreaElement::kOffNext));
    else
        m_.writeHalf(best + TaskWorkAreaElement::kOffSectors, static_cast<uint16_t>(remaining));

    return answer;
}

// Give a run of sectors back to the header its base identifier names,
// coalescing with a neighbour that abuts on either side.  The machine keeps
// the chain sorted and reuses the element it dequeues; this appends and
// only merges neighbours, so the free set is identical and the chain order
// is not (allocator policy).  A zero count and a base with no header both
// return without touching the chain.  The sectors are zero-filled before
// they are returned: SSP relies on a newly allocated work space being
// blank.
bool As36ControlStorageProcessor::freeTaskWorkArea(int relative, int sectors, std::string& why)
{
    int key = (relative >> 16) & 0xFF;
    int displacement = relative & 0xFFFF;
    why.clear();

    if (sectors == 0) {
        why = "a zero sector count - putHeap returns without touching the chain (c18b8e14, c18b8e30)";
        return false;
    }
    int header = taskWorkAreaHeader(key);
    if (header == 0) {
        why = fmt::format("no QH block carries base identifier {:02X}; putHeap returns without touching the chain when "
                          "findHeader answers null (c18b8e5c)",
                          key);
        return false;
    }

    if (!clearTaskWorkArea(relative, sectors, why)) return false;

    int link = header + TaskWorkAreaElement::kHeaderAsElement;
    int before = 0, after = 0;
    for (int hops = 0; hops < 4096; hops++) {
        int element = m_.readAddr24(link + TaskWorkAreaElement::kOffNext);
        if (element == 0) break;
        int at = m_.readHalf(element + TaskWorkAreaElement::kOffDisplacement);
        int len = m_.readHalf(element + TaskWorkAreaElement::kOffSectors);
        if (at + len == displacement) before = element;
        if (displacement + sectors == at) after = element;
        link = element;
    }

    if (before != 0) {
        int grown = m_.readHalf(before + TaskWorkAreaElement::kOffSectors) + sectors;
        if (after != 0) {
            grown += m_.readHalf(after + TaskWorkAreaElement::kOffSectors);
            unchainTaskWorkAreaElement(header, after);
        }
        m_.writeHalf(before + TaskWorkAreaElement::kOffSectors, static_cast<uint16_t>(grown));
        return true;
    }
    if (after != 0) {
        m_.writeHalf(after + TaskWorkAreaElement::kOffDisplacement, static_cast<uint16_t>(displacement));
        m_.writeHalf(after + TaskWorkAreaElement::kOffSectors,
                     static_cast<uint16_t>(m_.readHalf(after + TaskWorkAreaElement::kOffSectors) + sectors));
        return true;
    }

    int block = heap_.allocate(TaskWorkAreaElement::kBytes);
    if (block == 0) {
        why = "the heap has no room for a free element (16 bytes)";
        return false;
    }
    for (int i = 0; i < TaskWorkAreaElement::kBytes; i++) m_.writeByte(block + i, 0);
    m_.writeByte(block + TaskWorkAreaElement::kOffBaseIdentifier, static_cast<uint8_t>(key));
    m_.writeHalf(block + TaskWorkAreaElement::kOffDisplacement, static_cast<uint16_t>(displacement));
    m_.writeHalf(block + TaskWorkAreaElement::kOffSectors, static_cast<uint16_t>(sectors));
    m_.writeAddr24(block + TaskWorkAreaElement::kOffNext, m_.readAddr24(header + TaskWorkAreaQueue::kOffFreeChain));
    m_.writeAddr24(header + TaskWorkAreaQueue::kOffFreeChain, block);
    return true;
}

void As36ControlStorageProcessor::unchainTaskWorkAreaElement(int header, int element)
{
    int link = header + TaskWorkAreaElement::kHeaderAsElement;
    for (int hops = 0; hops < 4096; hops++) {
        int at = m_.readAddr24(link + TaskWorkAreaElement::kOffNext);
        if (at == 0) return;
        if (at == element) {
            m_.writeAddr24(link + TaskWorkAreaElement::kOffNext, m_.readAddr24(element + TaskWorkAreaElement::kOffNext));
            heap_.free(element, TaskWorkAreaElement::kBytes);
            return;
        }
        link = at;
    }
}

// SVC 33, TWAL: "allocates a task work area.  The value returned in XR2 is
// a relative disk address and not an actual disk address" (SA21-9436
// 3-128).  The sector count is WR6; the answer is split across the two
// halves of XR2 (the base identifier is XR2's PACT prefix); a failure is
// High, the manual's own "not available".  Q bit 7's wait is decoded and
// deliberately not taken: the only poster of its mask is the SSP task the
// task work area extender attaches, which this machine does not model, so
// the wait could never end.
bool As36ControlStorageProcessor::taskWorkAreaAllocate(SvcRequest& req)
{
    constexpr uint8_t kWaitForWorkArea = 0x01;   // Q bit 7

    int rb = req.requestBlock;
    int sectors = RequestBlock::readWr(m_, rb, 6);

    if (sectors == 0) {
        trace_.csp("SVC 33: WR6 is zero, and nutwal calls nuersvc with code 109 before it reaches the heap (c18926f8)");
        return false;
    }

    std::string why;
    int relative = allocateTaskWorkArea(sectors, why);
    if (relative == 0) {
        constexpr int kTaskWorkAreaFailureMask = 0x4000;

        setCondition(req, kPsrHigh);
        if ((req.q & kWaitForWorkArea) != 0)
            trace_.csp("SVC 33: {} sector(s) NOT allocated. Q bit 7 set takes nutwal's unconditional wait arm (c18927a4) - "
                       "nugwaitc with general-wait mask {:04X} - but the only poster is extendHeap's SSP task (NuEmul[0xFF8] "
                       "gate, c18b885c), which this emulator does not model, so the wait can never end. Answering High "
                       "(c18927f8), the manual's own \"not available\" (SA21-9436 3-128) and the closer approximation of the "
                       "real machine, which posts the wait and goes on. {}",
                       sectors, kTaskWorkAreaFailureMask, why);
        else
            trace_.csp("SVC 33: {} sector(s) NOT allocated - High. Q bit 7 clear and NuEmul[0xFF8] is zero, so nutwal's second "
                       "arm (c18927b8) returns the condition rather than waiting. {}",
                       sectors, why);
        return true;
    }

    RequestBlock::writeXr2(m_, rb, relative);
    setCondition(req, kPsrEqual);
    trace_.csp("SVC 33: allocated {} sector(s) -> XR2 = {:06X}, base identifier {:02X} displacement {:04X} (1-based sector {})",
               sectors, relative, (relative >> 16) & 0xFF, relative & 0xFFFF, taskWorkAreaSectorOf(relative));
    return true;
}

// SVC 34, DTWAL: "frees the task work area specified by the calling task"
// (SA21-9436 3-129).  XR2's prefix is the base identifier, XR2 the
// displacement, WR6 the sector count.  The call has no output and never
// touches the PSR; the free is asynchronous on the real machine and inline
// here.
bool As36ControlStorageProcessor::taskWorkAreaFree(SvcRequest& req)
{
    int rb = req.requestBlock;
    int relative = RequestBlock::readXr2Field(m_, rb);
    int sectors = RequestBlock::readWr(m_, rb, 6);

    std::string why;
    if (!freeTaskWorkArea(relative, sectors, why)) {
        trace_.csp("SVC 34: {} sector(s) at relative {:06X} NOT freed - {}", sectors, relative, why);
        return true;   // the machine reports nothing either way
    }

    // System event counter 27 is stepped, when the measurement block is
    // present and carries its eyecatcher.
    incrementEventCounter(req.taskBlock, 27);
    trace_.csp("SVC 34: freed {} sector(s) at relative {:06X} - base identifier {:02X} displacement {:04X}; system event "
               "counter 27 incremented (c1892690)",
               sectors, relative, (relative >> 16) & 0xFF, relative & 0xFFFF);
    return true;
}

// SVC 35, WRK: "provides work space maintenance.  A work space can be
// created or freed for the system or for a task" (SA21-9436 3-130).  "For
// creation the WRK SVC assigns and builds a storage block to describe the
// work space and allocates a swap area on disk from the task work area ...
// For deletion the SVC frees the swap area, storage block, and any main
// storage associated with the area."  XR1 names the list, resolved the way
// MAP resolves XR2.
bool As36ControlStorageProcessor::workSpaceMaintenance(SvcRequest& req)
{
    int rb = req.requestBlock;
    int xr1 = RequestBlock::readXr1Field(m_, rb);
    uint8_t list[WrkParameterList::kBytes] = {};
    if (!m_.readGuest24Range(xr1, list, sizeof list)) {
        return refuse("SVC 35: the {}-byte parameter list at {:06X} is not fully mapped (nucwrk resolves XR1 at "
                      "c18bb814 the same way nucmap resolves XR2)",
                      sizeof list, xr1);
    }

    const auto addr24 = [&](int at) { return (list[at] << 16) | (list[at + 1] << 8) | list[at + 2]; };
    uint8_t command = list[WrkParameterList::kOffCommand];
    uint8_t type = list[WrkParameterList::kOffType];
    int size = addr24(WrkParameterList::kOffSizeBytes);
    uint8_t flags = list[WrkParameterList::kOffFlags];
    int taskId = (list[WrkParameterList::kOffTaskId] << 8) | list[WrkParameterList::kOffTaskId + 1];
    bool task = type >= WrkParameterList::kTaskWorkSpaceFloor;

    trace_.csp("SVC 35: parameter list at {:06X}: command {} ({}), type {:02X} ({} work space), size {} byte(s) = {} page(s), "
               "flags {:02X}, task id {:04X}",
               xr1, command,
               command == WrkParameterList::kConditionalCreate     ? "conditional create"
               : command == WrkParameterList::kUnconditionalCreate ? "unconditional create"
               : command == WrkParameterList::kDelete              ? "delete"
                                                                   : "INVALID",
               type, task ? "task" : "system", size,
               (size + machine::MachineState::kPageBytes - 1) >> machine::MachineState::kPageShift, flags, taskId);

    if (command != WrkParameterList::kConditionalCreate && command != WrkParameterList::kUnconditionalCreate &&
        command != WrkParameterList::kDelete) {
        return refuse("SVC 35: command {} is not 1, 2 or 3 - nucwrk calls nuersvc with code 998 (c18bba1c); list={:06X}, "
                      "type={:02X}, size={:06X}, flags={:02X}, task-id={:04X}",
                      command, xr1, type, size, flags, taskId);
    }

    if (command == WrkParameterList::kDelete) {
        // Find, then the same teardown DTASK uses.  A referenced work space
        // is retained: its pin is cleared, it is dequeued and its owner
        // cleared; the final map-reference release deletes it.
        std::string delWhy;
        int victim = findWorkSpace(req.taskBlock, type, taskId, delWhy);
        if (victim == 0) {
            setCondition(req, kPsrLow);
            trace_.csp("SVC 35: delete found no {} work space of type {:02X} - Low ({})", task ? "task" : "system", type,
                       delWhy);
            return true;
        }
        uint8_t references = m_.readByte(victim + ControlBlock::kOffUseCount);
        if (references == 0) {
            deleteControlBlock(victim, "SVC 35");
        } else {
            uint8_t victimFlags = m_.readByte(victim + StorageBlock::kOffFlags);
            m_.writeByte(victim + StorageBlock::kOffFlags, static_cast<uint8_t>(victimFlags & ~ControlBlock::kFlagPinned));
            dequeueControlBlock(victim, "SVC 35 nucwdel");
            m_.writeAddr24(victim + ProgramBlock::kOffOwningTask, 0);
            trace_.csp("SVC 35: nucwdel detached referenced workspace {:06X} (+27={}), cleared pin 02 and owner +37..39; its "
                       "last map release will delete it (c18bbbec..c18bbc0c)",
                       victim, references);
        }
        setCondition(req, kPsrEqual);
        trace_.csp("SVC 35: deleted the {} work space of type {:02X} at {:04X} - Equal", task ? "task" : "system", type,
                   victim);
        return true;
    }

    if (command == WrkParameterList::kConditionalCreate) {
        // The anchor test masks off bit 0x800000: translated zero takes the
        // ordinary type search, like zero.  A non-zero candidate asks for a
        // membership scan that is not implemented.
        int anchor = addr24(WrkParameterList::kOffAnchor);
        int candidate = anchor & 0x7FFFFF;
        if (anchor != candidate && candidate == 0)
            trace_.csp("SVC 35: raw anchor {:06X} is translated zero; nucwrk masks bit 800000 at c18bb994 and uses the "
                       "ordinary nuquscs type search",
                       anchor);
        if (candidate != 0) {
            return refuse("SVC 35: conditional create asks whether candidate work space {:06X} (raw anchor {:06X}) is already "
                          "on the {} type-{:02X} chain; nucwrk calls nuqscan(11,11,3) at c18bb9b0 and this membership scan "
                          "is not implemented",
                          candidate, anchor, task ? "task" : "system", type);
        }
        std::string findWhy;
        int existing = findWorkSpace(req.taskBlock, type, taskId, findWhy);
        if (existing != 0) {
            // A found conditional create returns the EXISTING block in XR1
            // as well as High.
            RequestBlock::writeXr1(m_, rb, existing);
            setCondition(req, kPsrHigh);
            trace_.csp("SVC 35: a {} work space of type {:02X} already exists at {:04X} - returned in XR1 with High, nothing "
                       "built (nucwrk c18bbab0, common store c18bbb68..c18bbb70)",
                       task ? "task" : "system", type, existing);
            return true;
        }
        // Not found falls through to the create below.
    }

    // The page count is (size + 2047) >> 11, and the one builder is the
    // control-block factory, task work area allocation included.
    int pages = (size + machine::MachineState::kPageBytes - 1) >> machine::MachineState::kPageShift;
    if (pages == 0) {
        trace_.csp("SVC 35: a size of {} byte(s) rounds to zero pages, so there is nothing to build", size);
        setCondition(req, kPsrLow);
        return true;
    }

    // Flags 0x40 owns task-work-area backing and 0x02 pins the work space
    // while it remains on its chain; a system work space also gets 0x08.
    // Input bit 0x02 selects the unpinned form; input bit 0x01 makes the
    // whole newly created work space initially mapped.
    uint8_t storageFlags = ControlBlock::kFlagSwapArea;
    if ((flags & ControlBlock::kFlagPinned) == 0) storageFlags |= ControlBlock::kFlagPinned;
    if (!task) storageFlags |= 0x08;

    int failureCode;
    int initiallyMappedPages = (flags & 0x01) != 0 ? pages : 0;
    int sb = buildControlBlock(type, storageFlags, pages, initiallyMappedPages, req.taskBlock, 0, "SVC 35", failureCode);
    if (sb == 0) {
        setCondition(req, kPsrLow);
        trace_.csp("SVC 35: TWA allocate failure ({:06X}) - Low, the manual's own answer (3-131)", failureCode);
        return true;
    }

    // The finished block is queued LIFO on the head the type selects, the
    // same two heads a later conditional create searches.
    int anchorTask = task ? findTaskById(taskId, req.taskBlock) : 0;
    if (task && anchorTask == 0) {
        // No task with this id: queueing on the null anchor would write low
        // storage, so the block is built and returned but queued NOWHERE.
        trace_.csp("SVC 35: task id {:04X} names no task block on queue header 39 - the work space is built but queued "
                   "nowhere (nuidfind = 0; nucwrk's own null-anchor behaviour is unestablished)",
                   taskId);
    } else {
        int anchorFirst = task ? anchorTask + ControlBlock::kTaskWorkSpaceChain - 2
                               : GuestLowStorage::kQueueHeaderTable + 4 * ControlBlock::kSystemWorkSpaceQueue + 1;
        queueOperation(anchorFirst, sb, 11, ControlBlock::kQueueLifo);
    }

    RequestBlock::writeXr1(m_, rb, sb);
    setCondition(req, kPsrEqual);
    trace_.csp("SVC 35: created a {}-page {} work space of type {:02X} - storage block {:04X} in XR1; Equal", pages,
               task ? "task" : "system", type, sb);

    // The reference's experimental JCB+0x35 map-gate stand-in (its
    // SignonClssMapGate flag, off by default) is not ported.
    return true;
}

// =====================================================================
// SVC 36 SMFC
// =====================================================================

// SVC 36, SMFC (SA21-9436 3-132): the BSC communications interrupt handler
// tells the system measurement facility that an error occurred on the
// line.  A notification with no output; when the device has no measurement
// record the machine's own handler returns having done nothing, and
// nothing on this machine starts the facility for a line.
bool As36ControlStorageProcessor::smfc(SvcRequest& req)
{
    int rb = req.requestBlock;

    if ((req.q & ~kSmfcReceiveError) != 0) {
        trace_.csp("SVC 36: Q-byte {:02X} has bits the manual reserves; only bit 2 (0x20) is defined (3-132)", req.q);
    }

    int iob = RequestBlock::readXr1Field(m_, rb);
    bool receive = (req.q & kSmfcReceiveError) != 0;

    trace_.csp("SVC 36 SMFC: {} error reported on the communications line, IOB XR1 = {:06X}. NuSmf::commSmfcSvc returns at "
               "c18ad9f4 when the device has no SMF record, and nothing has started the system measurement facility on this "
               "machine, so there is nothing to collect into (docs/s36/svc-smfc.md)",
               receive ? "RECEIVE" : "TRANSMIT", iob);
    return true;
}

// =====================================================================
// SVC 26 Prepare Print Buffer
// =====================================================================

// One routine taking a (skip, space) pair.  The skip is a LINE NUMBER to
// reach: a form feed is emitted when the requested line is below the
// current one, and a skip past the end of the form is ignored outright.
// The space is a COUNT added to that base, itself ignored when it exceeds
// the forms length, and produces a form feed and a wrap when the sum runs
// off the page.
void As36ControlStorageProcessor::PrintWork::skipSpace(int skip, int space)
{
    lastCode = kScsCarriageReturn;   // primed with the carriage return
    int baseLine = line;

    if (skip != 0 && skip <= forms) {
        if (skip < baseLine) {
            lastCode = kScsFormFeed;
            formFeeds++;
            emit(kScsFormFeed);
        }
        baseLine = skip;
    }

    int newLine = baseLine;
    if (space <= forms) {
        int sum = (baseLine + space) & 0xFF;
        if (sum > forms) {
            lastCode = kScsFormFeed;
            formFeeds++;
            emit(kScsFormFeed);
            sum = (sum - forms) & 0xFF;
        }
        newLine = sum;
    }
    line = newLine;
}

// SVC 26, Prepare Print Buffer: ONE supervisor call, which SA21-9436 prints
// twice (3-113 not ideographic, 3-115 ideographic); the ideographic
// behaviour is selected by the SSP Ideographic feature indicator, guest
// 0x808 bit 0x80.  XR1 is the print IOB, reached as a real address; the
// Q-byte must be zero.
bool As36ControlStorageProcessor::preparePrintBuffer(SvcRequest& req)
{
    if (req.q != 0)
        trace_.csp("SVC 26: Q-byte {:02X} - the manual says it is not used and must be zero (3-113, 3-115)", req.q);

    int iob = RequestBlock::readXr1Field(m_, req.requestBlock);
    if (iob <= 0 || iob + kIobpFormFeedsAfter >= m_.backingBytes()) {
        trace_.csp("SVC 26: XR1 = {:06X} is not a usable print IOB address", iob);
        return false;
    }

    int buffer = m_.readAddr24(iob + kIobpBuffer);
    int length = m_.readHalf(iob + kIobpLength);
    uint8_t control = m_.readByte(iob + kIobpControl);
    uint8_t flags = m_.readByte(iob + kIobpFlags);
    int formsLength = m_.readByte(iob + kIobpFormsLength);
    int currentLine = m_.readByte(iob + kIobpCurrentLine);

    if (buffer <= 0 || buffer + kPrintBufferDataOffset + length > m_.backingBytes()) {
        trace_.csp("SVC 26: the print buffer at {:06X} plus {} bytes of data is not inside main storage", buffer, length);
        return false;
    }

    // The scan's own output bits are reset, and so is the before-count.
    m_.writeByte(iob + kIobpControl, static_cast<uint8_t>(control & ~kIobpCtlScanOutputs));
    control = static_cast<uint8_t>(control & ~kIobpCtlScanOutputs);
    m_.writeByte(iob + kIobpFormFeedsBefore, 0);

    bool ideographic = (m_.readByte(kScaDsspf) & kScaMkkkf) != 0;
    bool spooled = (flags & kIobpFlagSpooled) != 0;
    if ((control & kIobpCtl2ByteMode) != 0) {
        // The begin-scan indicator is set from the mode bit when the IOB
        // arrives already in 2-byte mode, whether or not the feature is
        // installed.
        control |= kIobpCtl2ByteBeginScan;
        m_.writeByte(iob + kIobpControl, control);
    }

    PrintWork work(m_, buffer, formsLength, currentLine);

    // ---- the skip and space BEFORE printing ---------------------------
    work.formFeeds = 0;
    work.skipSpace(m_.readByte(iob + kIobpSkipBefore), m_.readByte(iob + kIobpSpaceBefore));

    // If no form feed was emitted the last code is the carriage return it
    // was primed with, written out before the line is positioned; if one
    // was, the line is positioned only when it is not already line 1.
    if (work.lastCode != kScsFormFeed) {
        work.emit(work.lastCode);
        work.insertSkip();
    } else if (work.line != 1) {
        work.insertSkip();
    }

    m_.writeByte(iob + kIobpCurrentLine, static_cast<uint8_t>(work.line));
    m_.writeByte(iob + kIobpFormFeedsBefore, static_cast<uint8_t>(work.formFeeds));

    // ---- the data ------------------------------------------------------
    if ((control & kIobpCtlPrint) != 0) {
        if (!scanData(iob, work, length, ideographic, spooled, control)) return false;

        // ---- the skip and space AFTER printing -------------------------
        work.read = work.write;
        work.line = m_.readByte(iob + kIobpCurrentLine);
        work.formFeeds = 0;
        uint8_t skipAfter = m_.readByte(iob + kIobpSkipAfter);
        uint8_t spaceAfter = m_.readByte(iob + kIobpSpaceAfter);
        work.skipSpace(skipAfter, spaceAfter);
        if (work.lastCode != kScsFormFeed) {
            if (spaceAfter != 0 || skipAfter != 0) work.insertSkip();
            work.emitCarriageReturn();
        } else if (work.line != 1) {
            work.insertSkip();
        }
    } else if (work.lastCode != kScsFormFeed) {
        // No data, and no form feed, so the buffer still needs its
        // carriage return.
        work.emitCarriageReturn();
    }

    // ---- the update ------------------------------------------------------
    m_.writeByte(iob + kIobpCurrentLine, static_cast<uint8_t>(work.line));
    m_.writeByte(iob + kIobpFormFeedsAfter, static_cast<uint8_t>(work.formFeeds));
    int produced = work.write - buffer;
    m_.writeHalf(iob + kIobpLength, static_cast<uint16_t>(produced));

    if ((flags & kIobpFlagSpooled) == 0) {
        // 3-113: "If the data is to be routed directly to the printer
        // instead of being spooled, this instruction updates the forms
        // length and current line fields of the associated printer unit
        // block".  The unit block is guest storage.
        int pub = m_.readAddr24(iob + kIobpUnitBlock);
        if (pub > 0 && pub + kPubCurrentLine < m_.backingBytes()) {
            m_.writeByte(pub + kPubFormsLength, static_cast<uint8_t>(work.forms));
            m_.writeByte(pub + kPubCurrentLine, static_cast<uint8_t>(work.line));
            trace_.csp("SVC 26: direct to printer - unit block {:04X} forms length {}, current line {}", pub, work.forms,
                       work.line);
        } else {
            trace_.csp("SVC 26: the output is not spooled but IOB {:04X} names no printer unit block at +21", iob);
        }
    }

    trace_.csp("SVC 26: IOB {:04X} buffer {:06X} - {} byte(s) in, {} out; line {} of {}; {} form feed(s) after{}", iob, buffer,
               length, produced, work.line, work.forms, work.formFeeds,
               (control & kIobpCtlPrint) != 0 ? "" : "; no print operation requested");
    return true;
}

// The scan loop.  In single-byte mode it is 3-113's paragraph as code:
// compress runs of more than three blanks into a relative horizontal
// position code, replace anything below hex 40 with hex FF, and discard
// the blanks at the end of the record.  With the Ideographic feature
// installed, an SO switches into the 2-byte arm, which reads the data as
// ward/point PAIRS until an SI switches back; the point byte of a pair is
// copied verbatim, whereas a ward byte below hex 40 becomes hex FF.  The
// direct (not spooled) ideographic path stays refused.
bool As36ControlStorageProcessor::scanData(int iob, PrintWork& work, int length, bool ideographic, bool spooled,
                                           uint8_t& control)
{
    work.read = work.buffer + kPrintBufferDataOffset;
    work.blankRun = 0;

    bool twoByteMode = ideographic && (control & kIobpCtl2ByteMode) != 0;
    // After a ward byte is emitted while 2-byte mode is on, the loop reads
    // the POINT byte next.
    bool expectPoint = false;

    if (twoByteMode && !refuseDirectIdeographic(iob, spooled)) return false;

    for (int i = 0; i < length; i++) {
        uint8_t ch = m_.readByte(work.read++);

        if (expectPoint) {
            // The point byte is copied as-is.  A blank point byte is counted
            // only when a run is already open.
            if (ch == kScsBlank) {
                if (work.blankRun != 0) work.blankRun++;
                work.emit(ch);
            } else {
                work.blankRun = 0;
                work.emit(ch);
            }
            expectPoint = false;
            continue;
        }

        if (twoByteMode) {
            if (ch == kScsShiftIn) {
                // SI leaves 2-byte mode: flush, emit the SI, clear the mode
                // bit.  No point byte follows an SI.
                work.flushBlankRun();
                work.emit(ch);
                twoByteMode = false;
                control = static_cast<uint8_t>(control & ~kIobpCtl2ByteMode);
                m_.writeByte(iob + kIobpControl, control);
                continue;
            }
            if (ch > kScsBlank) work.flushBlankRun();
            if (ch == kScsBlank) work.blankRun++;
            if (ch < kScsBlank) {
                // SO while already in mode, or any other byte below hex 40,
                // is 3-115's "processed as a character less than hex 40".
                work.flushBlankRun();
                ch = kScsGraphicError;
            }
            work.emit(ch);
            expectPoint = true;
            continue;
        }

        // ---- single-byte mode -----------------------------------------
        if (ideographic && ch == kScsShiftOut) {
            // SO enters 2-byte mode: flush, emit the SO, set the mode bit.
            // The byte after an SO is a ward, not a point.
            if (!refuseDirectIdeographic(iob, spooled)) return false;
            work.flushBlankRun();
            work.emit(ch);
            twoByteMode = true;
            control = static_cast<uint8_t>(control | kIobpCtl2ByteMode);
            m_.writeByte(iob + kIobpControl, control);
            continue;
        }
        if (ch > kScsBlank) {
            work.flushBlankRun();
            work.emit(ch);
            continue;
        }
        if (ch == kScsBlank) {
            work.blankRun++;
            work.emit(ch);
            continue;
        }

        // The run is flushed and hex FF takes the character's place.  An SI
        // outside 2-byte mode lands here too.
        work.flushBlankRun();
        work.emit(kScsGraphicError);
    }

    // The trailing blank run is simply rewound over.
    work.write -= work.blankRun;
    work.blankRun = 0;
    return true;
}

// The direct (not spooled) ideographic path is refused: the machine would
// rewrite ward/point bytes to hex 40 from the control byte's printer
// ideographic-capability bit, whose semantics are not placed.  True when it
// is safe to proceed.
bool As36ControlStorageProcessor::refuseDirectIdeographic(int iob, bool spooled)
{
    if (spooled) return true;
    trace_.csp("SVC 26: IOB {:04X} carries ideographic 2-byte data with the output direct (not spooled). nuptckpt (c18df834) "
               "would replace bytes with hex 40 from $IOBPCTL bit 01, the printer ideographic capability, whose semantics "
               "this corpus does not place (docs/s36/svc-prepare-print-buffer.md)",
               iob);
    return false;
}

// =====================================================================
// the control storage transient bodies
// =====================================================================

// Run a control storage transient.  False when the identifier has no body
// here, which is the area's cue to trace it as unimplemented rather than
// to pretend it ran.  Control returns to the caller only after the
// transient has executed completely (SA21-9436 3-142), so a transient that
// does nothing is not a harmless stub.
bool As36ControlStorageProcessor::runTransient(uint8_t transientId, uint8_t inline2, uint8_t inline3, int xr1, int xr2,
                                               int taskBlock, int requestBlock)
{
    switch (transientId) {
        case 0x03: return transientLoadControl(xr2);
        case 0x05: return transientIplControl(requestBlock);
        case 0x09: return transientStorageCleanup(xr1, xr2, requestBlock);
        case 0x0A: return transientTimer(inline2, xr2, taskBlock, requestBlock);
        case 0x37: return transientTimeOfDay(xr1);
        case 0x3E: return transientTransferM36(inline2, inline3, xr1, requestBlock);
        default: return false;
    }
}

// Transient 3E, the hosted-M36 TFRM36 parameter-list service: function 01
// retrieves, 02 ends, selected from rb+17.  WR4's low byte identifies the
// work station and WR5 receives the status: 00FE no station/display, 00FF
// a display with no transfer plist, 0003 a plist present.  A configured
// display whose transfer has been bound answers 03 for both AUTOSIGNON
// settings and supplies the complete 80-byte guest copy of the list; a
// configured display that has not crossed that bind gets the exact no-list
// answer.
bool As36ControlStorageProcessor::transientTransferM36(uint8_t function, uint8_t subFunction, int xr1, int requestBlock)
{
    if (requestBlock == 0) return false;
    if (function != 0x01 && function != 0x02) {
        trace_.csp("transient 3E: function {:02X} is neither 01 (nuRtvTfrM36) nor 02 (nuEndTfrM36); native nucx calls "
                   "nuerio 79",
                   function);
        return true;
    }

    int unit = RequestBlock::readWr(m_, requestBlock, 4) & 0xFF;
    int tub = resolveConfiguredTubByUnit(unit);
    bool autoSignOn = false;
    bool displayPresent = tub != 0;
    bool plistPresent = false;
    if (displayPresent) {
        auto bound = transferredWorkStations_.find(tub);
        if (bound != transferredWorkStations_.end()) {
            plistPresent = true;
            autoSignOn = bound->second;
        }
    }
    uint16_t status = !displayPresent ? static_cast<uint16_t>(0x00FE)
                      : !plistPresent ? static_cast<uint16_t>(0x00FF)
                                      : static_cast<uint16_t>(0x0003);
    RequestBlock::writeWr(m_, requestBlock, 5, status);
    if (!plistPresent) {
        trace_.csp("transient 3E: {} station {:02X}, TU {:06X}, saved XR1 {:06X}, subfunction {:02X} -> WR5={:04X}; {}",
                   function == 0x01 ? "nuRtvTfrM36" : "nuEndTfrM36", unit, tub, xr1, subFunction, status,
                   displayPresent ? "display has no transfer plist" : "no station/display");
        return true;
    }

    if (function == 0x01 && subFunction == 0x02) {
        int destination;
        if (!resolveThreeByteAddress(xr1, destination)) return true;
        std::vector<uint8_t> payload = buildTfrm36GuestPayload(unit, autoSignOn);
        m_.write(destination, payload.data(), static_cast<int>(payload.size()));
        trace_.csp("transient 3E: nuRtvTfrM36 station {:02X}, TU {:06X}, subfunction 02 copied the verified 80-byte plist+60 "
                   "payload to saved XR1 {:06X}; WR5=0003, AUTOSIGNON(*{})",
                   unit, tub, xr1, autoSignOn ? "YES" : "NO");
        return true;
    }

    trace_.csp("transient 3E: {} station {:02X}, TU {:06X}, saved XR1 {:06X}, subfunction {:02X} -> WR5=0003; transfer plist "
               "present",
               function == 0x01 ? "nuRtvTfrM36" : "nuEndTfrM36", unit, tub, xr1, subFunction);
    return true;
}

// The 80-byte guest copy of the transfer plist: +0 the auto-sign-on byte,
// +1 the ideographic flag, +8 the S/36 user, +16 menu, +24 library, +32
// procedure, +40 the OS/400 user, all EBCDIC blank-filled, then thirty
// binary zeros.  The user name is the one the station's session announced.
std::vector<uint8_t> As36ControlStorageProcessor::buildTfrm36GuestPayload(int unit, bool autoSignOn)
{
    std::vector<uint8_t> payload(80, 0);
    payload[0] = autoSignOn ? 1 : 0;
    payload[1] = 1;
    for (int i = 8; i < 50; i++) payload[static_cast<std::size_t>(i)] = 0x40;

    devices::WorkStationSlot* slot = devices_.workStations().find(unit);
    std::string user = slot == nullptr || slot->isPrinter ? std::string() : slot->backend()->userName();
    std::string trimmed;
    for (char c : user)
        if (!std::isspace(static_cast<unsigned char>(c))) trimmed += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!trimmed.empty()) {
        std::vector<uint8_t> encoded = storage::Ebcdic::fromAscii(trimmed);
        for (std::size_t i = 0; i < std::min<std::size_t>(8, encoded.size()); i++) payload[8 + i] = encoded[i];
        for (std::size_t i = 0; i < std::min<std::size_t>(10, encoded.size()); i++) payload[40 + i] = encoded[i];
    }
    return payload;
}

// Transient 05, IPL control: it does not return, it reboots the machine.
// The request word is the IPL source the guest has just written through
// SVC 0F as direct area word 1074; bit 0x02 asks for the next IPL from
// disk, bit 0x10 from tape, and the body then performs a pseudo IPL.  This
// is the last instruction of an SSP generation.  The closest a control
// processor with no persistent machine object can get is to STOP; taking
// the restart is the operator's move.
bool As36ControlStorageProcessor::transientIplControl(int requestBlock)
{
    (void)requestBlock;
    int request = directArea_.read(DirectArea::kIplSource);

    if ((request & 0xFF) == 0) {
        trace_.csp("transient 05 (IPL control, c18d548c): request word {:04X} has a zero low byte, so the body frees the "
                   "action element and returns without doing anything. No IPL is being asked for",
                   request);
        return true;
    }

    const char* source = (request & 0x02) != 0 ? "disk" : (request & 0x10) != 0 ? "tape" : nullptr;
    const char* arm = (request & 0x02) != 0   ? "bit 02: setIplType(2) + allowIplFromDisk, so the next IPL is FROM DISK"
                      : (request & 0x10) != 0 ? "bit 10: findTape + setIplType(0), so the next IPL is FROM TAPE"
                                              : "no arm decoded for these bits";

    // The machine's IPL source becomes the requested one.  The machine
    // definition is immutable on this side, so the new source is recorded
    // here for the operator's `ipl` to consult; the name reported is the
    // one the reference would show.
    std::string iplSourceName = source != nullptr ? std::string(source) : cfg_.iplSourceName;
    if (source != nullptr) pseudoIplSource_ = source;
    pseudoIplRequested_ = true;

    msp_->halt(fmt::format("transient 05: the guest asked for a PSEUDO IPL (SVC 50 inline 05, request word {:04X}; {}). "
                           "NuEmul::nuips does copyInstallData + flushS36ToDasd and restarts, so control does not return "
                           "here - `$IPS Psuedo IPL` on the microcode volumes. The IPL source is now '{}' and the machine "
                           "is stopped at the point the restart would happen; `ipl` to take it. "
                           "docs/s36/msrel-program-check-2026-09-10.md",
                           request, arm, iplSourceName));
    trace_.csp("transient 05 (IPL control, c18d548c): PSEUDO IPL requested, request word {:04X} - {}. IPL source is now '{}'",
               request, arm, iplSourceName);
    return true;
}

// Transient 09, storage block cleanup, dispatched on the request block's
// byte 9.  Only the shared free-and-return tail is modelled: the 0x60 walk
// deletes guest storage blocks reached through a native table this
// emulator does not represent.
bool As36ControlStorageProcessor::transientStorageCleanup(int xr1, int xr2, int requestBlock)
{
    (void)xr1;
    (void)xr2;
    int function = requestBlock != 0 ? m_.readByte(requestBlock + 9) : -1;
    if (function == 0x60 || function == 0x50)
        trace_.csp("transient 09 (storage cleanup, c18d5704): rb+9 = {:02X}, which is the {} arm - it walks NuEmul+0x390's "
                   "table and calls nucdelsb, and neither the table nor the walk is modelled. Returning without deleting "
                   "anything; blocks it would have freed stay allocated",
                   function, function == 0x60 ? "block-chain delete" : "sub-coded");
    else
        trace_.csp("transient 09 (storage cleanup, c18d5704): rb+9 = {:02X} takes the body's shared tail - free the action "
                   "element and return, which is what this does",
                   function);
    return true;
}

// Transient 0A, the timer transient.  The timer request block is reached
// through saved XR2; only the high nibble of inline 2 selects the
// operation.  Operation 40 with representation 8 writes zoned time and
// date (MMDDYY, the default Advanced/36 ordering, emulator policy) at
// TRB+2..+13, synchronously, with no post, wait or scheduler effect.
bool As36ControlStorageProcessor::transientTimer(uint8_t inline2, int xr2, int taskBlock, int requestBlock)
{
    uint8_t operation = static_cast<uint8_t>(inline2 & 0xF0);

    int block;
    if (!resolveThreeByteAddress(xr2, block)) return true;

    if (operation == 0x20) return registerCptcTimer(block, taskBlock, requestBlock);

    if (operation == 0x30) return cancelCptcTimer(inline2, block, taskBlock);

    if (operation != 0x40) {
        trace_.csp("transient 0A (nutix): XR2 TRB {:06X}, operation {:02X} (inline {:02X}) is not implemented; block UNCHANGED",
                   block, operation, inline2);
        return false;
    }

    uint8_t representation = static_cast<uint8_t>(m_.readByte(block) & 0x0F);
    if (representation != 0x08) {
        trace_.csp("transient 0A (nutix): XR2 TRB {:06X}, operation {:02X}, representation {:X} is not implemented; block "
                   "UNCHANGED",
                   block, inline2, representation);
        return false;
    }

    std::tm now = localNow();
    writeZonedTwoDigits(block + 2, now.tm_hour);
    writeZonedTwoDigits(block + 4, now.tm_min);
    writeZonedTwoDigits(block + 6, now.tm_sec);

    writeZonedTwoDigits(block + 8, now.tm_mon + 1);
    writeZonedTwoDigits(block + 10, now.tm_mday);
    writeZonedTwoDigits(block + 12, (now.tm_year + 1900) % 100);

    trace_.csp("transient 0A (nutix): XR2 TRB {:06X}, operation {:02X}, representation 8 -> zoned {:02}:{:02}:{:02} "
               "{:02}/{:02}/{:02} at +2..+13 (host local clock; emulator date-order policy MMDDYY); no task post, wait, or "
               "scheduling side effect",
               block, inline2, now.tm_hour, now.tm_min, now.tm_sec, now.tm_mon + 1, now.tm_mday, (now.tm_year + 1900) % 100);
    return true;
}

// The narrow, proven registration arm: TRB 82 01 ... with a non-zero
// type-2 interval and operation 20.  The registration is retained so a
// repeated request replaces the same task/key entry; type-2 expiry
// advances the machine date timer by a day and re-arms.  The one guest
// post edge is the immediate task post when TRB+0 bit 80 is set.
bool As36ControlStorageProcessor::registerCptcTimer(int block, int taskBlock, int requestBlock)
{
    uint8_t control = m_.readByte(block);
    uint8_t key = m_.readByte(block + 1);
    uint32_t interval = (static_cast<uint32_t>(m_.readByte(block + 4)) << 24) |
                        (static_cast<uint32_t>(m_.readByte(block + 5)) << 16) |
                        (static_cast<uint32_t>(m_.readByte(block + 6)) << 8) | m_.readByte(block + 7);

    if (control != 0x82 || key != 0x01 || interval == 0) {
        trace_.csp("transient 0A (nutix): operation 20 TRB {:06X} shape {:02X} {:02X} interval {:08X} is outside the "
                   "implemented CPTC subset; block unchanged",
                   block, control, key, interval);
        return false;
    }

    std::string identity = fmt::format("{:06X}:{:02X}", taskBlock, key);
    bool replaced = nutixTimers_.find(identity) != nutixTimers_.end();
    long long registered = timerNow();
    NutixTimerRegistration registration;
    registration.taskBlock = taskBlock;
    registration.requestBlock = requestBlock;
    registration.trb = block;
    registration.key = key;
    registration.control = control;
    registration.interval = interval;
    registration.registeredTimestamp = registered;
    registration.dueTimestamp = registered + timerUnitsToTicks(interval);
    registration.expirationCount = 0;
    nutixTimers_[identity] = registration;

    trace_.csp("transient 0A (nutix): operation 20 registered CPTC timer TRB {:06X}, TB {:06X}, key {:02X}, interval {} "
               "unit(s) (8.192 ms), {}",
               block, taskBlock, key, interval, replaced ? "replaced prior registration" : "new registration");

    if ((control & 0x80) != 0 && TaskBlock::isTaskBlock(m_, taskBlock)) {
        trace_.csp("transient 0A (nutix): TRB {:06X} control {:02X} -> immediate nupotcb(TB {:06X}, 08) (c197d998..c197d9B0)",
                   block, control, taskBlock);
        postTaskConditionsFromTransient(taskBlock, 0x08, "transient 0A nutix immediate timer post");
    } else {
        trace_.csp("transient 0A (nutix): TRB {:06X} no immediate post; type-2 expiry remains unimplemented (no task wake "
                   "fabricated)",
                   block);
    }
    return true;
}

// The proven cancellation form: operation/option 38 with a type-8 result
// TRB against the key-01 registration.  The remaining interval is
// converted to whole seconds with the exact 128/15625 ratio and written as
// zoned HHMMSS at +2..+7.
bool As36ControlStorageProcessor::cancelCptcTimer(uint8_t inline2, int block, int taskBlock)
{
    uint8_t control = m_.readByte(block);
    uint8_t key = m_.readByte(block + 1);

    if (inline2 != 0x38 || control != 0x08 || key != 0x01) {
        trace_.csp("transient 0A (nutix): operation 30 TRB {:06X} inline {:02X} shape {:02X} {:02X} is outside the implemented "
                   "CPTC cancel subset; block unchanged",
                   block, inline2, control, key);
        return false;
    }

    std::string identity = fmt::format("{:06X}:{:02X}", taskBlock, key);
    uint32_t remaining = 0;
    auto it = nutixTimers_.find(identity);
    bool found = it != nutixTimers_.end();
    if (found) {
        remaining = timerRemainingInterval(it->second);
        nutixTimers_.erase(it);
    }

    uint32_t seconds = static_cast<uint32_t>((static_cast<unsigned long long>(remaining) * 128ULL) / 15625ULL);
    writeZonedTwoDigits(block + 2, static_cast<int>(seconds / 3600U));
    writeZonedTwoDigits(block + 4, static_cast<int>((seconds / 60U) % 60U));
    writeZonedTwoDigits(block + 6, static_cast<int>(seconds % 60U));

    trace_.csp("transient 0A (nutix): operation 38 cancelled CPTC timer TRB {:06X}, TB {:06X}, key {:02X}: {}, remaining {} "
               "unit(s) -> zoned {:02}:{:02}:{:02} at +2..+7; no guest task post or workstation event",
               block, taskBlock, key, found ? "matching registration removed" : "no matching registration", remaining,
               seconds / 3600U, (seconds / 60U) % 60U, seconds % 60U);
    return true;
}

// The host monotonic clock in nanoseconds.
long long As36ControlStorageProcessor::timerNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One timer unit is exactly 8192 microseconds; a native timer never fires
// before its requested unit.
long long As36ControlStorageProcessor::timerUnitsToTicks(uint32_t units)
{
    return std::max(1LL, static_cast<long long>(units) * 8192000LL);
}

// The machine subtracts two integral timer values; the host clock is finer
// grained, so a positive difference rounds up to the next complete unit.
uint32_t As36ControlStorageProcessor::timerRemainingIntervalAt(const NutixTimerRegistration& timer, long long now)
{
    long long ticks = timer.dueTimestamp - now;
    if (ticks <= 0) return 0;
    long long units = (ticks + 8192000LL - 1) / 8192000LL;
    if (units >= static_cast<long long>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(units);
}

uint32_t As36ControlStorageProcessor::timerRemainingInterval(const NutixTimerRegistration& registration)
{
    return timerRemainingIntervalAt(registration, timerNow());
}

int As36ControlStorageProcessor::millisecondsUntilNextNativeTimer() const
{
    if (nutixTimers_.empty()) return -1;
    long long now = timerNow();
    long long due = nutixTimers_.begin()->second.dueTimestamp;
    for (const auto& t : nutixTimers_) due = std::min(due, t.second.dueTimestamp);
    if (due <= now) return 0;
    long long milliseconds = (due - now + 999999LL) / 1000000LL;
    if (milliseconds >= static_cast<long long>(INT32_MAX)) return INT32_MAX;
    return static_cast<int>(std::max(1LL, milliseconds));
}

std::vector<As36ControlStorageProcessor::NativeTimerInfo> As36ControlStorageProcessor::nativeTimerState() const
{
    long long now = timerNow();
    std::vector<const NutixTimerRegistration*> ordered;
    for (const auto& t : nutixTimers_) ordered.push_back(&t.second);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const NutixTimerRegistration* a, const NutixTimerRegistration* b) { return a->dueTimestamp < b->dueTimestamp; });
    std::vector<NativeTimerInfo> result;
    for (const NutixTimerRegistration* timer : ordered) {
        long long delta = timer->dueTimestamp - now;
        int milliseconds;
        if (delta <= 0) {
            milliseconds = 0;
        } else {
            long long value = (delta + 999999LL) / 1000000LL;
            milliseconds = value >= static_cast<long long>(INT32_MAX) ? INT32_MAX : static_cast<int>(std::max(1LL, value));
        }
        uint8_t type = static_cast<uint8_t>(timer->control & 0x0F);
        NativeTimerInfo info;
        info.taskBlock = timer->taskBlock;
        info.requestBlock = timer->requestBlock;
        info.trb = timer->trb;
        info.key = timer->key;
        info.control = timer->control;
        info.type = type;
        info.originalInterval = timer->interval;
        info.remainingUnits = timerRemainingIntervalAt(*timer, now);
        info.millisecondsUntilDue = milliseconds;
        info.expirationCount = timer->expirationCount;
        info.expiryDisposition = type == 2 ? "nutimih: advance machine date timer and re-arm for one day; no guest post"
                                           : "unimplemented timer subtype";
        result.push_back(info);
    }
    return result;
}

// Run every due native timer through its expiry disposition.  A type-2
// entry advances the machine date timer by one day and re-arms; nothing
// is posted to a guest task.  Any other type has no implemented
// disposition and is removed rather than repeatedly fired.
bool As36ControlStorageProcessor::serviceDueNativeTimers(int& expired)
{
    expired = 0;
    bool guestTaskReadied = false;
    long long now = timerNow();

    for (int guard = 0; guard < 4096; guard++) {
        auto next = nutixTimers_.end();
        for (auto it = nutixTimers_.begin(); it != nutixTimers_.end(); ++it)
            if (it->second.dueTimestamp <= now && (next == nutixTimers_.end() || it->second.dueTimestamp < next->second.dueTimestamp))
                next = it;
        if (next == nutixTimers_.end()) break;

        NutixTimerRegistration& timer = next->second;
        uint8_t type = static_cast<uint8_t>(timer.control & 0x0F);
        expired++;

        if (type == 2) {
            timer.expirationCount++;
            long long dayTicks = timerUnitsToTicks(kNutixMachineDayUnits);
            timer.dueTimestamp = timer.dueTimestamp > LLONG_MAX - dayTicks ? LLONG_MAX : timer.dueTimestamp + dayTicks;
            trace_.csp("native timer expiry: TRB {:06X}, TB {:06X}, key {:02X}, control {:02X}/type 2 -> nutislih calls "
                       "nutimih; machine timer += 00A0EEBB unit(s) (24 hours), entry re-armed; no nupotcb, nupostac, SSP "
                       "event, or workstation record (expiry #{})",
                       timer.trb, timer.taskBlock, timer.key, timer.control, timer.expirationCount);
            continue;
        }

        int trb = timer.trb;
        uint8_t control = timer.control;
        nutixTimers_.erase(next);
        trace_.csp("native timer expiry: TRB {:06X}, control {:02X}/type {:X} has no implemented nutislih disposition; entry "
                   "removed without inventing a guest post",
                   trb, control, type);
    }
    return guestTaskReadied;
}

// Write a two-digit positive value as EBCDIC zoned decimal F0..F9.
void As36ControlStorageProcessor::writeZonedTwoDigits(int address, int value)
{
    m_.writeByte(address, static_cast<uint8_t>(0xF0 | ((value / 10) % 10)));
    m_.writeByte(address + 1, static_cast<uint8_t>(0xF0 | (value % 10)));
}

// Write a two-digit positive value as one packed-BCD byte.
void As36ControlStorageProcessor::writePackedTwoDigits(int address, int value)
{
    m_.writeByte(address, static_cast<uint8_t>((((value / 10) % 10) << 4) | (value % 10)));
}

// Transient 37, the time-of-day transient, addressed through saved XR1.
// Operation 05 places packed hour, minute, second, year, month and day at
// request+2..+7 and returns status zero (host local clock, the same policy
// as SVC 2E).  Operation 0A returns machine status: bit 02 is the manual
// key state, the one native flag this emulator models; the +8 native query
// result is left unchanged.
bool As36ControlStorageProcessor::transientTimeOfDay(int xr1)
{
    int block;
    if (!resolveThreeByteAddress(xr1, block)) return true;

    uint8_t operation = m_.readByte(block);
    if (operation < 1 || operation > 11) {
        m_.writeByte(block + 1, 0x80);
        trace_.csp("transient 37 (nutod): XR1 block {:06X}, operation {:02X} outside 01..0B -> +1 = 80 (c197d4c4..c197d510)",
                   block, operation);
        return true;
    }

    if (operation == 0x05) {
        std::tm now = localNow();
        m_.writeByte(block + 1, 0x00);
        writePackedTwoDigits(block + 2, now.tm_hour);
        writePackedTwoDigits(block + 3, now.tm_min);
        writePackedTwoDigits(block + 4, now.tm_sec);
        writePackedTwoDigits(block + 5, (now.tm_year + 1900) % 100);
        writePackedTwoDigits(block + 6, now.tm_mon + 1);
        writePackedTwoDigits(block + 7, now.tm_mday);
        trace_.csp("transient 37 (nutod): XR1 block {:06X}, operation 05 -> +1 = 00, packed {:02}:{:02}:{:02} "
                   "{:02}-{:02}-{:02} at +2..+7 (host local clock; c178b2ec..c178b3c0)",
                   block, now.tm_hour, now.tm_min, now.tm_sec, (now.tm_year + 1900) % 100, now.tm_mon + 1, now.tm_mday);
        return true;
    }

    if (operation != 0x0A) {
        trace_.csp("transient 37 (nutod): XR1 block {:06X}, operation {:02X} is valid but nutodp operation body is not "
                   "implemented; block UNCHANGED",
                   block, operation);
        return false;
    }

    bool manual = sameIgnoringCase(cfg_.iplType, "attend");
    uint8_t status = manual ? 0x02 : 0x00;
    m_.writeByte(block + 1, status);
    trace_.csp("transient 37 (nutod): XR1 block {:06X}, operation 0A -> +1 = {:02X}; native status bit 14/manual key is {}, "
               "native status bits 12, 13 and 31 are not represented and therefore clear. +8 native-query result remains "
               "unchanged (not decoded; #CPTS does not consume it)",
               block, status, manual ? "set (attend)" : "clear (unattend)");
    return true;
}

// Transient 03, the system disk area locator.  The caller points XR2 at a
// block, puts a two-byte area identifier in +2..3, and gets back the
// area's disk sector in +2..4 and a second value in +8; block+0 bit 0x08
// makes the block a chain of 90-byte entries and bit 0x10 asks for the
// entry to be resolved.
bool As36ControlStorageProcessor::transientLoadControl(int xr2)
{
    int block;
    if (!resolveThreeByteAddress(xr2, block)) return true;

    uint8_t control = m_.readByte(block + 0);

    // +1 is cleared before anything is decided.
    m_.writeByte(block + 1, 0);

    if ((control & kNulcChained) != 0) return loadControlChain(block, control);

    resolveArea(block, control);
    return true;
}

// The chained sub-form: a singly linked list of fixed 90-byte entries.  An
// entry with both 0x08 and 0x10 is a link the walk steps over (clearing the
// next entry's +1); the walk stops at the first entry with 0x08 clear,
// which is resolved, or with 0x08 set and 0x10 clear, the end of the chain
// with nothing resolved.  The header itself must carry 0x10.
bool As36ControlStorageProcessor::loadControlChain(int block, uint8_t control)
{
    if ((control & kNulcResolve) == 0) {
        trace_.csp("transient 03 (nulc): chained header at block {:06X} has +0 = {:02X} (bit 08 without bit 10) - chain not "
                   "walked (c18d8320)",
                   block, control);
        return true;
    }

    int entry = block;
    for (int step = 0; step < kNulcChainMaxEntries; step++) {
        m_.writeByte(entry + kNulcChainStride + 1, 0);
        entry += kNulcChainStride;
        uint8_t c = m_.readByte(entry + 0);

        if ((c & kNulcChained) == 0) {
            trace_.csp("transient 03 (nulc): chained walk resolves entry {:06X} (+0 = {:02X})", entry, c);
            resolveArea(entry, c);
            return true;
        }

        if ((c & kNulcResolve) == 0) {
            trace_.csp("transient 03 (nulc): chained walk ends at entry {:06X} (+0 = {:02X}, bit 08 without bit 10) - no entry "
                       "resolved (c18d8348)",
                       entry, c);
            return true;
        }
        // 0x18: a link; loop to the next entry.
    }

    // Not the machine's behaviour: it would walk a malformed all-links chain
    // until it ran off mapped storage.  The links are guest data, so the
    // walk is bounded and refused rather than spun.
    trace_.csp("transient 03 (nulc): chained walk from block {:06X} passed {} entries with no terminator - refusing (guard has "
               "no SLIC counterpart)",
               block, kNulcChainMaxEntries);
    return true;
}

// Resolve one area-locator entry in place.  The keyboard translate tables
// (FB10..FBEF, keyed by guest 0x097F) are at sector 592 but their INDEX
// comes from a computed jump through a runtime pointer, so they are
// answered not-found and the divergence is said.
void As36ControlStorageProcessor::resolveArea(int entry, uint8_t control)
{
    if ((control & kNulcResolve) == 0) {
        trace_.csp("transient 03 (nulc): entry {:06X} has +0 = {:02X}, neither bit 08 nor bit 10 - nothing to resolve", entry,
                   control);
        return;
    }

    int key = m_.readHalf(entry + 2);

    if (key >= kKeyboardTableFirst && key <= kKeyboardTableLast) {
        m_.writeAddr24(entry + 2, kNulcNotFound);
        trace_.csp("transient 03 (nulc): area {:04X} is a keyboard translate table (FBxx, this machine's is {:04X} from guest "
                   "097F = {:02X}) -> sector 592, but the table INDEX comes from a computed jump through TOC[0x358], a runtime "
                   "pointer. Answering not-found; DIVERGES from the machine, which would answer. "
                   "docs/s36/svc-transient-scheduler.md",
                   key, kKeyboardTableFirst + m_.readByte(kKeyboardTableSelector), m_.readByte(kKeyboardTableSelector));
        return;
    }

    int sector, plus8;
    if (!loadControlArea(key, sector, plus8)) {
        m_.writeAddr24(entry + 2, kNulcNotFound);
        trace_.csp("transient 03 (nulc): area {:04X} is not one of the six the table names (and the keyboard-table key at "
                   "guest 097F is not modelled) -> +2..4 = FF0000, not found",
                   key);
        return;
    }

    m_.writeAddr24(entry + 2, sector);
    m_.writeByte(entry + 8, static_cast<uint8_t>(plus8));
    trace_.csp("transient 03 (nulc): area {:04X} -> sector {}, +8 = {}", key, sector, plus8);
}

// The area table, in the order the machine tests it.  The sectors are
// literals in the instruction stream, properties of the machine rather
// than of the volume.
bool As36ControlStorageProcessor::loadControlArea(int key, int& sector, int& plus8)
{
    switch (key) {
        case 0xC104: sector = 27;   plus8 = 4; return true;
        case 0xF010: sector = 580;  plus8 = 1; return true;
        case 0xF020: sector = 580;  plus8 = 1; return true;
        case 0xFEFE: sector = 8191; plus8 = 0; return true;
        case 0x804E: sector = 581;  plus8 = 9; return true;
        case 0xFCF7: sector = 7217; plus8 = 0; return true;
        default: sector = 0; plus8 = 0; return false;
    }
}

// The machine's one rule for a three-byte guest address: below 0x800000
// it is real, at or above it the low sixteen bits are task-translated.
bool As36ControlStorageProcessor::resolveThreeByteAddress(int value, int& real)
{
    if ((value & kThreeByteTranslated) == 0) {
        real = value;
        return true;
    }
    machine::StorageProtection fault;
    if (m_.resolve(static_cast<uint16_t>(value), machine::MspRegisters::kPactTranslate, machine::MachineState::kAtrTaskGroup0,
                   false, real, &fault))
        return true;
    trace_.csp("transient 03 (nulc): {:06X} is translated and does not resolve - {}", value, fault.message());
    real = 0;
    return false;
}

}  // namespace sim36::processors::controlstorage
