// The system queue space: the pool SVC 06 Assign and SVC 07 Free Assigned
// Areas hand out of, and the pool every control block on the transfer path
// comes from.  SA21-9436 3-78: "the assign supervisor call instruction
// allocates the storage out of the system queue space. Storage is assigned
// in multiples of 16 bytes on 16-byte boundaries."
//
// On the Advanced/36 it is a binary buddy allocator over a pool that lies
// inside guest main storage, so every address it hands back is addressable
// by the MSP.  Everything below is transcribed from the native machine's
// behaviour unless it is marked EMULATOR POLICY.
//
// The pool: 64 KB initially, growing one 64 KB segment at a time up to a
// ceiling.  The first 112 bytes of the initial segment are not allocatable
// (the free-list head table lives there), and every later segment reserves
// its first 16 bytes.
//
// Size classes: twelve, powers of two from 16 to 32768.  The class is the
// smallest power of two that holds the request; a request bigger than 32768
// fails outright, which is the architecture's "requests for large amounts of
// free space are more likely to fail since the area assigned must be
// contiguous space" (3-78).  When the class is 256 bytes or more larger than
// the request the allocation is rounded to a 256-byte multiple instead; the
// class still selects the free list, only the bytes carved out change.
//
// Allocation: take the head of the list, no search.  Lists are kept in
// ascending address order, so the head is the lowest address in the class.
// A longer element's tail goes straight back to the free path.  The search
// walks class indexes upward from the request's own class to the limit.
//
// Free: absorb forward while the running length is not a power of two and
// the block immediately above is free ("this area freed is either merged to
// one of the current free areas (if adjacent) or queued to the free chain",
// 3-80); then, if the length is a power of two, merge with the buddy (at
// pool offset A XOR S) while the buddy is free and the same length; else
// decompose descending into powers of two, each filed in its own list.
//
// Free's validity checks, each an abort (code 52) on the real machine and a
// traced refusal here: zero length; address not 16-byte aligned; address
// below 8192; low halfword of the address zero; area crossing a 64 KB
// boundary.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"

namespace sim36::processors::controlstorage {

class GuestHeap {
public:
    // "Storage is assigned in multiples of 16 bytes on 16-byte boundaries"
    // (SA21-9436 3-78), and the smallest size class.
    static constexpr int kGranularity = 16;
    // Twelve size classes, 16 through 32768.
    static constexpr int kClasses = 12;
    // 32768: the largest class, and so the largest area the pool can assign.
    static constexpr int kLargestClass = kGranularity << (kClasses - 1);
    // One committed heap segment is a literal 64 KB.
    static constexpr int kIbmPoolBytes = 0x10000;
    // The free-list head table: the first 112 bytes are never allocatable.
    static constexpr int kHeaderBytes = 112;
    // An address below 8192 cannot be freed.
    static constexpr int kLowestFreeableAddress = 8192;

    struct AllocationExtent {
        int address, length, requested;
        long long allocation;
        std::string owner;
    };
    // One free element: the address and length that on the native machine
    // are the element's own "FQ" eyecatcher and length halfword.
    struct FreeElement {
        int address, length;
    };

    // Where the initial pool sits and where growth stops are EMULATOR
    // POLICY; the reserved prefixes and how each segment is carved are
    // transcribed.  The initial segment is the established 0x2000-0x10000
    // region, ending on a 64 KB boundary, because a relocated module's
    // stored addresses are 16 bits and this emulator does not carry a PACT
    // prefix through them; growth is nevertheless required.
    //
    // On an allocation miss the heap clears the next 64 KB of guest storage,
    // reserves its first 16 bytes, enqueues the other 65520 bytes, advances
    // the limit and retries the original search once.
    //
    // An invalid geometry (see `valid()`) leaves an empty heap that refuses
    // every request; the guest core does not throw.
    GuestHeap(machine::MachineState& m, int low, int bytes, int maximumHigh, monitor::Tracer& trace);

    bool valid() const { return valid_; }
    // Why construction failed, or empty.
    const std::string& constructionError() const { return constructionError_; }

    // Recreate the constructor state for a new control-processor IPL: the
    // initial segment is cleared and reseeded; extensions from an earlier run
    // are not committed in the new heap.
    void reset();

    int low() const { return low_; }
    int high() const { return high_; }
    // Bytes currently assigned, header excluded.
    int used() const { return used_; }
    // The high-water mark of used().
    int peak() const { return peak_; }
    // Assignable bytes in the committed span: the initial 112-byte header and
    // each extension segment's 16-byte prefix are excluded.
    int capacity() const { return bytes_ - kHeaderBytes - extensionPages_ * kGranularity; }
    // Bytes on the free lists.
    int available() const;

    // A stable copy of the live ownership journal, in address order.
    std::vector<AllocationExtent> allocatedExtents() const;
    // A stable copy of every current free-list extent, in address order.
    std::vector<FreeElement> freeExtents() const;
    std::vector<std::string> invariantFailures() const { return invariantFailures_; }
    long long operationSequence() const { return operationSequence_; }

    // Check all three representations against one another.  Read-only, and
    // independent of guest storage.
    bool checkInvariants(std::vector<std::string>& failures) const;

    std::vector<int> captureCheckpoint() const;
    // False when the checkpoint is malformed; the heap is then left reset.
    bool restoreCheckpoint(const std::vector<int>& v);

    // The ownership-journal id of the live allocation that begins at `at` and
    // is exactly `bytes` long, or zero.  Diagnostic host bookkeeping: an area
    // this emulator assigned on the guest's behalf may be freed by the guest
    // with its own SVC 07 first, and comparing the id distinguishes "still
    // ours" from "freed and re-assigned to somebody else".
    long long liveAllocationAt(int at, int bytes) const;

    // Did this pool hand out that address?  The initial program and request
    // block pair are the IPL's, not an allocation, so a transfer that discards
    // the caller's frame must not try to return them here.
    bool contains(int at) const { return at >= low_ && at < high_; }

    // The class index: the smallest power of two from 16 up that holds
    // `bytes`.  May equal kClasses, which is how a request larger than 32768
    // fails.
    static int classIndex(int bytes);
    // How many bytes an assign of `bytes` actually carves out.
    static int roundedSize(int bytes);

    // Assign, returning a GUEST address, or 0 when the pool cannot answer.
    // Zero is the architected failure value: SVC 06 returns "zero if no space
    // is assigned and the no wait option of the Q-byte is specified" (3-79).
    int allocate(int bytes) { return allocate(bytes, std::string()); }
    int allocate(int bytes, const std::string& owner);
    // Why the last allocate() returned zero, or empty.
    const std::string& lastRefusal() const { return lastRefusal_; }

    // SVC 07 Free Assigned Areas, and every internal free.  The caller's raw
    // length is what is returned, not the allocation's size class: freeing a
    // 272-byte tail of a combined task/request block allocation must return
    // exactly that tail.
    void free(int at, int bytes) { free(at, bytes, std::string()); }
    void free(int at, int bytes, const std::string& owner);

private:
    struct LiveExtent {
        int off, length, requested;
        long long allocation;
        std::string owner;
    };

    int allocateFromLists(int bytes, const std::string& owner, long long operation, int first, int size);
    bool extendHeap();
    void clearGuest(int at, int length);
    void rebuildOpaqueOwnership(const std::string& owner);
    void subtractOpaqueRange(int off, int length);
    void traceNewInvariantFailures(long long operation, const std::string& action);
    void reportInvariant(long long operation, const std::string& failure);
    void sortAllocated();
    int head(int k) const;
    void dequeue(int k, int off);
    void insert(int off, int size);
    void enqueue(int off, int size);
    static bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

    machine::MachineState& m_;
    monitor::Tracer& trace_;
    bool valid_ = false;
    std::string constructionError_;
    int low_ = 0, initialBytes_ = 0, maximumHigh_ = 0;
    int high_ = 0, bytes_ = 0, extensionPages_ = 0;
    // The free lists, one per size class, in ascending address order.
    // Offsets are pool-relative.
    std::set<int> class_[kClasses];
    // Pool offset to length, for every free element.
    std::map<int, int> free_;
    // Host-side ownership journal for diagnostics; never supplies guest bytes
    // and never changes placement.
    std::vector<LiveExtent> allocated_;
    std::vector<std::string> invariantFailures_;
    long long operationSequence_ = 0, allocationSequence_ = 0;
    int used_ = 0, peak_ = 0;
    std::string lastRefusal_;
};

// The allocator behind SVC 2C Translated Assign and SVC 2D Translated Free:
// space handed out within one work space, as displacements from its start.
//
// What the architecture fixes: "Storage is assigned in 64 byte multiples on
// 64 byte boundaries. The length assigned must not be zero and not exceed
// FFC0 hex bytes" (SA21-9436 3-119); the arena is the work space the storage
// block describes, sb[16] * 2048 bytes, and a longer request is refused
// before anything is searched; displacement zero is a valid answer (the
// manual's own example returns "hex 800000"); "Partial free areas of the
// originally assigned area may be requested if each area to free begins on
// a 64-byte boundary" (3-121), so free takes an arbitrary sub-range.
//
// Which free block a request gets is EMULATOR POLICY: the native machine
// keeps a bit per 64-byte element inside the storage block, in the part
// SA21-9436 defers to System Data Areas; this keeps the same information
// beside the block instead, as first fit over a free list.
class WorkSpaceHeap {
public:
    // "64 byte multiples on 64 byte boundaries" (SA21-9436 3-119).
    static constexpr int kGranularity = 64;
    // "not exceed FFC0 hex bytes": 64 KB less one element.
    static constexpr int kMaximumLength = 0xFFC0;

    explicit WorkSpaceHeap(int bytes) : capacity_(bytes > 0 ? bytes : 0) { free_.push_back({0, capacity_}); }

    static int round(int bytes) { return (bytes + kGranularity - 1) & ~(kGranularity - 1); }

    // First fit.  Returns the displacement, or -1 when the work space has no
    // run that long, which is the PSR Low the manual specifies.
    int allocate(int bytes);
    // Give a range back, coalescing with its neighbours: "merged to one of
    // the current free areas (if adjacent)".
    void free(int at, int bytes);
    int available() const;
    int capacity() const { return capacity_; }

    std::vector<int> captureCheckpoint() const;
    bool restoreCheckpoint(const std::vector<int>& v);

private:
    struct Range {
        int at, length;
    };
    int capacity_ = 0;
    std::vector<Range> free_;
};

}  // namespace sim36::processors::controlstorage
