// The allocation queue element (AQE) - the block SVC 20 and SVC 21 exist to
// make, chain and unmake.
//
// SA21-9436 names the block, names five of its fields in the worked example
// on 3-104 (AQECHAIN, AQETB@, AQECBQ, AQENESTQ, AQELEN) and gives not one
// displacement - the layout is the one it defers to System Data Areas.  Every
// offset below was recovered from the four routines that build and walk the
// block; the manual's five names are attached where the walk shows which
// field is which.
//
// Offsets are DECIMAL, and three-byte fields are named by their FIRST byte -
// the same convention TaskBlock uses, and two lower than the last-byte name
// the queue engine and the manual use.
#pragma once

#include <cstdint>

namespace sim36::processors::controlstorage {

struct AllocationQueueElement {
    // EBCDIC "AQ", stamped into every AQE the enqueue builds.  The same shape
    // as the ACE's "AC" and the task block's "TB".
    static constexpr uint16_t kEyecatcher = 0xC1D8;

    // 3 bytes ending at 4 - AQECHAIN, the chain on the RESOURCE queue.  The
    // enqueue's evaluation loop steps through it, and every queue-engine call
    // that names the resource queue passes chain displacement 4.
    static constexpr int kOffResourceChain = 2;

    // 3 bytes ending at 7 - AQETB@, the address of the block that owns this
    // element: the TASK BLOCK normally, the JOB CONTROL BLOCK when the request
    // said "queue by JCB".  The enqueue writes it and matches against it to
    // find the caller's own element; the owner lookup reads it to get from an
    // AQE back to a task.
    //
    // The manual's own name for it is AQETB@, and 3-104's scan passes it as
    // the argument displacement of a 3-byte compare - SVC SVCSCAN,03 - while
    // loading a JCB address into XR1 as the argument.  One field, two kinds of
    // owner.
    static constexpr int kOffOwner = 5;

    // The ACTIVE share byte.  (level + 1) in bits 0x07, the request's flags
    // above it, and bit 0x80 meaning this element owns the resource.
    //
    // The +1 is corroborated: the enqueue stores inline parameter 1 + 1, and
    // the share test decides two elements CANNOT share when
    // (a & 7) + (b & 7) > 4 - which is the manual's own table (3-107: 0 shares
    // with 0,1,2; 1 with 0,1; 2 with 0; 3 with none, i.e. La + Lb <= 2) if and
    // only if the stored value is L + 1.  The System/34 System Data Areas
    // manual prints the same encoding for its own AQEFLAGS (LY21-0049 figure
    // 2-24: 01 Level 0, 02 Level 1, 03 Level 2, 04 Level 3), and the same 80
    // "Owner of resource" and 40 "Never ending program owner/requestor".
    static constexpr int kOffActive = 8;

    // 3 bytes ending at 11 - AQECBQ, the chain on the OWNER's AQE queue (the
    // task block's or the job control block's).  The enqueue queues on it with
    // chain displacement 11, the dequeue removes with the same, and the
    // specific dequeue walks it.
    static constexpr int kOffOwnerChain = 9;

    // The REQUESTED share byte - the same encoding as kOffActive, holding what
    // this requester asked for rather than what the queue has resolved.  The
    // specific dequeue feeds it back to rebuild the active levels after a
    // dequeue.
    //
    // Two flag bits are read out of this byte rather than the active one:
    // 0x10 nested queueing active, which decides whether the element is 16 or
    // 20 bytes, and 0x80, which the enqueue uses as "this element has already
    // been evaluated once" - set the first time the walk reaches it, and the
    // thing that distinguishes "grant it now, its caller is still inside its
    // own supervisor call" from "grant it and POST the task that is waiting
    // for it".
    static constexpr int kOffRequested = 12;

    // 3 bytes ending at 15: the address of the RESOURCE QUEUE HEADER this
    // element is enqueued on - the value the caller passed in XR2.  The
    // dequeue reads it back to find the queue to dequeue from, and the
    // specific dequeue matches on it to pick this resource's elements out of
    // the owner's chain.
    static constexpr int kOffResourceQueue = 13;

    // 3 bytes ending at 19, present only in a NESTED element: the element
    // this one nests over.  The dequeue puts that element back on the
    // resource queue when this one is freed.
    static constexpr int kOffNestedPrevious = 17;

    // AQELEN.  The enqueue allocates 16 or, for a nested element, 20; the
    // dequeue frees the same two lengths off aqe+12 bit 0x10.
    static constexpr int kBytes = 16, kNestedBytes = 20;

    // ---- the LAST-byte names, which are what the queue engine is passed ----

    // AQECHAIN as the queue engine is passed it: byte 4.
    static constexpr int kChainLastResource = kOffResourceChain + 2;

    // AQECBQ as the queue engine is passed it: byte 11.
    static constexpr int kChainLastOwner = kOffOwnerChain + 2;

    // ---- the bits of the two share bytes ----

    // 0x80 in kOffActive: owner of the resource.  LY21-0049 figure 2-24 names
    // it in exactly those words for the S/34's AQEFLAGS; the enqueue sets it
    // when the element can share with everything above it, the specific
    // dequeue sets it on the last surviving element, and the enqueue clears it
    // off a superseded element.
    static constexpr uint8_t kOwner = 0x80;

    // 0x40: never-ending program owner or requester.  Manual 3-107 inline
    // parameter 1 bit 1, S/34 AQEFLAGS 40, and the bit tested first when
    // deciding whether a conflict is Low rather than High.
    static constexpr uint8_t kNeverEndingProgram = 0x40;

    // 0x20: the element is queued by JCB rather than by task block.  Manual
    // 3-107 inline parameter 1 bit 2; the owner lookup reads it out of
    // kOffActive to decide whether kOffOwner is a task block or a job control
    // block.
    static constexpr uint8_t kQueuedByJcb = 0x20;

    // 0x10: nested queueing active.  Manual 3-107 inline parameter 1 bit 3,
    // read out of kOffRequested by the dequeue.
    static constexpr uint8_t kNested = 0x10;

    // 0x08: extended level 1.  Manual 3-107 inline parameter 1 bit 4, "shares
    // with extended level 1 and 0".  The share test consults it only when the
    // two level + 1 values sum to exactly 4 - the level-1/level-1 and
    // level-0/level-2 cases - and calls it a conflict when the two elements
    // disagree about the bit.
    static constexpr uint8_t kExtendedLevel1 = 0x08;

    // 0x07: the share level, stored as level + 1.
    static constexpr uint8_t kLevelMask = 0x07;

    // 0x80 in kOffRequested: this element has been evaluated once already, so
    // its requester is no longer inside the supervisor call that created it
    // and a grant has to POST it.
    static constexpr uint8_t kEvaluated = 0x80;
};

static_assert(AllocationQueueElement::kOffResourceQueue + 3 == AllocationQueueElement::kBytes, "a plain AQE is 16 bytes");
static_assert(AllocationQueueElement::kOffNestedPrevious + 3 == AllocationQueueElement::kNestedBytes, "a nested AQE is 20 bytes");
static_assert(AllocationQueueElement::kChainLastResource == 4 && AllocationQueueElement::kChainLastOwner == 11, "last-byte chain names");

}  // namespace sim36::processors::controlstorage
