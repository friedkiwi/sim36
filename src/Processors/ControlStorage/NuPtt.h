// The ATR file of ONE request block, and the pool of them.
//
// A request block does not merely use an ATR file, it owns one.  Every site
// that changes which request block is current - a transfer in, a return, a
// task dispatch, filling the file - resolves the handle at rb+56..58 against
// the pool, checks that the file's owner is the block asked about, and only
// then takes its array.  The live ATR file pointer is REPOINTED at those
// sites rather than rebuilt: that is SA21-9436 1-29's "A5 PATR - Fast task
// switch for ATRs" expressed in software.
//
// This emulator keeps a real 128-register hardware file in MachineState,
// because SY31-9035 describes real registers and address resolution needs
// one choke point.  So the request block owns the IMAGE, and making a block
// current loads its image into the file.  Same invariant, one copy added.
//
// The slots hold the architected 16-bit page frame, not a host bias, and
// the object lives beside guest storage rather than in it - only the HANDLE
// at rb+56..58 is a guest field.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace sim36::processors::controlstorage {

class NuPtt {
public:
    // 32 program-level ATRs.
    static constexpr int kAtrCount = 32;
    // The array's offset inside the native object, immediately after its base object.
    static constexpr int kOffAtrArray = 40;
    // The owning request block, immediately after the array: 40 + 32 * 8.
    static constexpr int kOffOwner = 0x128;
    // The free-list link.
    static constexpr int kOffFreeLink = 0x130;
    // The object's size: 312 bytes.
    static constexpr int kBytes = 0x138;

    static_assert(kOffAtrArray + kAtrCount * 8 == kOffOwner, "the owner word follows the 32 slots");

    explicit NuPtt(int handle) : handle_(handle) {}

    // The value of rb+56..58: this object's byte offset from the pool base.
    int handle() const { return handle_; }
    // Zero when the object is on the free list.
    int owner = 0;
    // The image: sixteen-bit page frames.
    uint16_t atr[kAtrCount] = {};

private:
    int handle_;
};

// The pool: a base and a free-list head.  Both allocation sites - the one a
// transfer runs for every request block it creates, and the one the IPL runs
// for the initial block - pop the free list or construct a 312-byte object,
// stamp the owner, and store the handle into rb+56..58.  So EVERY request
// block in existence has its own file; there is no shared one and no
// unowned block.  Freeing is the exact inverse: clear the owner, push onto
// the free list, then free the request block itself.
class NuPttPool {
public:
    // How many objects have ever been constructed; the rest is free-list reuse.
    int constructed() const { return static_cast<int>(all_.size()); }
    int freeCount() const { return static_cast<int>(free_.size()); }
    // Every object the pool has ever constructed, in handle order, for the
    // monitor.  Nothing in the machine reads it.
    const std::vector<std::unique_ptr<NuPtt>>& files() const { return all_; }

    // A power-on IPL constructs a fresh control processor, so the pool comes
    // up empty and no file outlives the machine.
    void reset();

    std::vector<int> captureCheckpoint() const;
    // False (pool left empty) when the checkpoint is malformed.
    bool restoreCheckpoint(const std::vector<int>& v);

    // Give this request block its own ATR file and return it; its handle goes
    // to rb+56..58.
    NuPtt* allocate(int rb);
    // Clear the ownership word and push the object onto the free list.  Called
    // with the handle out of rb+56..58, so a block whose handle names someone
    // else's file cannot free it.
    bool free(int handle, int rb);
    // A resident module occasionally has to move when SVC 12 grows its task
    // region. Repoint every saved request-block image that still names one
    // of the old real frames.
    void rebaseFrames(uint16_t oldFrame, uint16_t newFrame, int pages);
    // The ownership check itself: resolve the handle against the pool and hand
    // back the object only when its owner is the request block asked about.
    NuPtt* owned(int handle, int rb);

private:
    std::vector<std::unique_ptr<NuPtt>> all_;
    std::map<int, NuPtt*> byHandle_;
    std::vector<NuPtt*> free_;   // back() is the head
};

}  // namespace sim36::processors::controlstorage
