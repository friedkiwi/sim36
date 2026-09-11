// Action control element allocation and queueing.
//
// Every delayed SVC - the contiguous range R 0x40 to 0x52, which is every
// device path plus the transient scheduler, task work area access and the
// loader - becomes an ACE queued to a system queue header.  So this is how
// all System/36 I/O is represented, not a corner of the design.
//
// The queues are singly-linked lists of 24-bit guest addresses.  Header n
// lives at guest 0xB03 + 4n; the chain link is ACE+2, which is why a freshly
// built element has it zeroed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/GuestHeap.h"

namespace sim36::processors::controlstorage {

class ActionControlElementQueue {
public:
    // Queue header n is at this address plus 4n.
    static constexpr int kQueueHeaderBase = 0xB03;
    static constexpr int kQueueHeaderStride = 4;

    // Checkpoint state: the private-arena fields Next/Free are vestigial (the
    // heap owns placement and reuse) and are kept so an older snapshot still
    // loads.
    struct CheckpointState {
        int next = 0;
        std::vector<int> free;
        std::vector<int> allocated;
    };

    // ACEs are allocated out of THE SYSTEM QUEUE SPACE: an ACE is an ordinary
    // 32-byte system queue space assignment whose guest address is handed to
    // the caller.  (A private arena would overlap the pool: a guest task that
    // dequeues a completion element and frees it with SVC 07 - the architected
    // thing to do with an area that came out of the system queue space -
    // would then put arena storage on the pool's free lists.)
    ActionControlElementQueue(machine::MachineState& m, GuestHeap& heap, monitor::Tracer& trace)
        : m_(m), heap_(heap), trace_(trace) {}

    CheckpointState captureCheckpoint() const;
    // False (with `why` set) when the checkpoint is invalid.
    bool restoreCheckpoint(const CheckpointState& s, std::string& why);

    // A new control-processor IPL rebuilds the system queue space, so every
    // element this allocator was holding is gone with it.
    void reset() { allocated_.clear(); }

    static int headerAddress(int headerNumber) { return kQueueHeaderBase + headerNumber * kQueueHeaderStride; }

    // Allocate a 32-byte element in guest storage, ZERO-FILLED before it is
    // handed back.  The pool reuses freed areas, so an uncleared reused
    // element would carry the previous element's fields - including its saved
    // XR2 at ace+16, which a partial producer that does not rewrite it would
    // then hand to a woken waiter.  Zero is the value a real producer leaves
    // there.  Returns 0 when the pool cannot supply one.
    int allocate();

    void release(int ace);

    // SVC 4C: build an element from the current task and queue it.  Q bit 2
    // asks for the address back - stored into the event control mask XR1
    // points at, and returned in XR2.  Returns 0 when no element could be
    // allocated.
    int buildAndQueue(int rb, int tb, uint8_t qByte, uint8_t headerNumber);

    // The two-arm FIFO insert.  A stored link is the successor's OWN address,
    // not its address plus the link offset: the queue scan, the queue search
    // and the IPL's own writes of the task block's plain address into headers
    // 37 and 39 all read links that way.
    void enqueue(uint8_t headerNumber, int ace);

    std::vector<int> elements(uint8_t headerNumber);

    // Complete a request: post through the event control mask the ACE names,
    // then unlink.  The mask's 7th byte becomes hex 4n.
    void post(int ace, int completionCode);

    void dump(std::FILE* out, int ace);

private:
    machine::MachineState& m_;
    GuestHeap& heap_;
    monitor::Tracer& trace_;
    // Live elements, and the pool allocation each one came from.  The id is
    // what lets release() tell an element the guest has already freed from
    // one the pool has since re-assigned.
    std::map<int, long long> allocated_;
};

}  // namespace sim36::processors::controlstorage
