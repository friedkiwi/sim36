#include "Processors/ControlStorage/ActionControlElementQueue.h"

#include <fmt/format.h>

#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::processors::controlstorage {

ActionControlElementQueue::CheckpointState ActionControlElementQueue::captureCheckpoint() const
{
    CheckpointState s;
    for (const auto& kv : allocated_) s.allocated.push_back(kv.first);   // std::map iterates sorted
    return s;
}

bool ActionControlElementQueue::restoreCheckpoint(const CheckpointState& s, std::string& why)
{
    allocated_.clear();
    for (int a : s.allocated) {
        if (allocated_.count(a) != 0) {
            why = "duplicate allocated ACE in checkpoint";
            return false;
        }
        // The heap is restored from its own checkpoint, so the pool knows
        // which allocation each live element belongs to.
        allocated_[a] = heap_.liveAllocationAt(a, ActionControlElement::kSize);
    }
    why.clear();
    return true;
}

int ActionControlElementQueue::allocate()
{
    // 32 bytes out of the system queue space.  The native pool's allocation
    // does not return null, but this pool is smaller than the native one and
    // can refuse; report it rather than hand back an address the pool does
    // not own.
    int a = heap_.allocate(ActionControlElement::kSize, "nubldace ACE");
    if (a == 0) {
        trace_.ace("nubldace: the system queue space cannot assign 32 bytes for an ACE");
        return 0;
    }
    for (int i = 0; i < ActionControlElement::kSize; i++) m_.writeByte(a + i, 0);
    long long allocation = heap_.liveAllocationAt(a, ActionControlElement::kSize);
    auto it = allocated_.find(a);
    if (it != allocated_.end()) {
        // The pool cannot assign an area that is already assigned, so this
        // address is here twice only because the GUEST freed the earlier
        // element with SVC 07 and this allocator was still holding it.  The
        // record is stale, not a collision: hand it over to the new
        // allocation.  (An equal id would mean the pool assigned live
        // storage, which is the corruption case.)
        long long previous = it->second;
        if (previous == allocation) {
            trace_.ace("ACE allocator refused live element {:04X}; pool ownership is corrupt", a);
            return 0;
        }
        trace_.ace("ACE {:04X} was freed out of the system queue space by the guest "
                   "(allocation #{}); nubldace re-assigns it as #{}",
                   a, previous, allocation);
    }
    allocated_[a] = allocation;
    return a;
}

void ActionControlElementQueue::release(int ace)
{
    if (ace == 0) return;
    // A queued/completing ACE remains allocated until its one consumer
    // releases it.  Several synchronous controller paths can nest, so
    // admitting duplicate or foreign releases would let a live complete-queue
    // element be allocated and overwritten.  The real pool cannot do that;
    // enforce the same ownership invariant here.
    auto it = allocated_.find(ace);
    if (it == allocated_.end()) {
        trace_.ace("ACE release ignored for {:04X}: not owned or already released", ace);
        return;
    }
    long long allocation = it->second;
    allocated_.erase(it);

    // An element handed to the guest on a complete-event queue is the guest's
    // to free, and SSP does exactly that: a sign-on job dequeues the
    // work-station completion element and issues SVC 07 free of 32 bytes
    // against it.  When that has happened the pool no longer holds our
    // allocation, and freeing again would either be a double free or, worse,
    // hand back an area the pool has since re-assigned.  The allocation id,
    // not the address, is what tells the two apart.
    if (heap_.liveAllocationAt(ace, ActionControlElement::kSize) != allocation) {
        trace_.ace("ACE {:04X} was already freed out of the system queue space "
                   "(allocation #{} is gone); ownership dropped without a second free",
                   ace, allocation);
        return;
    }
    heap_.free(ace, ActionControlElement::kSize, "nubldace ACE");
}

int ActionControlElementQueue::buildAndQueue(int rb, int tb, uint8_t qByte, uint8_t headerNumber)
{
    int ace = allocate();
    if (ace == 0) return 0;

    ActionControlElement::build(m_, ace, rb, tb, qByte);
    trace_.ace("built {:04X} from rb={:04X} tb={:04X} q={:02X} xr1={:06X} xr2={:06X}",
               ace, rb, tb, qByte, RequestBlock::readXr1Field(m_, rb), RequestBlock::readXr2Field(m_, rb));

    // Q bit 3: put a different task block address in the ACE, from XR2.
    ActionControlElement::applyTaskAssociation(m_, ace, qByte);

    // Q bit 5: the event type the waiter will match arrives in WR6 and is
    // stored at +22 (nuidpost; nubldacn itself zeroes the halfword).  The
    // spool writer builds its general-post element this way with type 0010.
    if ((qByte & 0x04) != 0) {
        uint16_t type = RequestBlock::readWr(m_, rb, 6);
        m_.writeHalf(ace + ActionControlElement::kOffEventType, type);
        trace_.ace("  event type {:04X} from WR6 stored at +22 (Q bit 5)", type);
    }

    // Q bit 2: hand the address back through the event control mask and XR2.
    if ((qByte & 0x20) != 0) {
        int ecm = RequestBlock::readXr1Field(m_, rb);
        m_.writeAddr24(ecm + Ecm::kOffAceAddress, ace);
        RequestBlock::writeXr2(m_, rb, ace);
        trace_.ace("  address {:06X} stored at ecm {:06X}+2 and returned in XR2", ace, ecm);
    }

    enqueue(headerNumber, ace);
    return ace;
}

void ActionControlElementQueue::enqueue(uint8_t headerNumber, int ace)
{
    int header = headerAddress(headerNumber) - 2;   // the 3-byte value's first byte
    int head = m_.readAddr24(header);
    m_.writeAddr24(ace + ActionControlElement::kOffChainLink, 0);
    if (head == 0) {
        m_.writeAddr24(header, ace);
        trace_.ace("queued {:04X} to header {:02X} at {:04X} (was empty)", ace, headerNumber, header);
        return;
    }
    // Walk to the tail: FIFO, so a new element goes last.
    int at = head, guard = 0;
    while (guard++ < 4096) {
        int next = m_.readAddr24(at + ActionControlElement::kOffChainLink);
        if (next == 0) break;
        at = next;
    }
    m_.writeAddr24(at + ActionControlElement::kOffChainLink, ace);
    trace_.ace("queued {:04X} to header {:02X} behind {:04X}", ace, headerNumber, at);
}

std::vector<int> ActionControlElementQueue::elements(uint8_t headerNumber)
{
    std::vector<int> list;
    int at = m_.readAddr24(headerAddress(headerNumber) - 2);
    int guard = 0;
    while (at != 0 && guard++ < 4096) {
        list.push_back(at);
        at = m_.readAddr24(at + ActionControlElement::kOffChainLink);
    }
    return list;
}

void ActionControlElementQueue::post(int ace, int completionCode)
{
    // ace+13..15 is the RAW XR1 pair; the ECM address inside it is a
    // device-path address and follows the same rule as every other one -
    // real unless bit 0x800000, then task-translated.
    int ecmField = m_.readAddr24(ace + ActionControlElement::kOffXr1);
    int ecm;
    if (!m_.resolveGuest24(ecmField, true, ecm)) ecm = 0;
    if (ecm != 0) {
        Ecm::post(m_, ecm, completionCode);
        trace_.ace("posted {:04X} -> ecm {:06X}+6 = {:02X}", ace, ecm, 0x40 | (completionCode & 0x0F));
    } else {
        trace_.ace("posted {:04X} with no event control mask - request ignored", ace);
    }
}

void ActionControlElementQueue::dump(std::FILE* out, int ace)
{
    fmt::print(out, "ACE {:04X}\n", ace);
    fmt::print(out, "  +00 eyecatcher   {:04X} {}\n", m_.readHalf(ace),
               m_.readHalf(ace) == ActionControlElement::kEyecatcher ? "(AC)" : "(NOT AC - not an ACE?)");
    fmt::print(out, "  +02 chain link   {:06X}\n", m_.readAddr24(ace + ActionControlElement::kOffChainLink));
    fmt::print(out, "  +05 tb byte 7    {:02X}\n", m_.readByte(ace + ActionControlElement::kOffTbByte7));
    fmt::print(out, "  +06 rb counter   {:04X}\n", m_.readHalf(ace + ActionControlElement::kOffRbCounter));
    fmt::print(out, "  +08 queue header {:02X}\n", m_.readByte(ace + ActionControlElement::kOffInlineParm1));
    fmt::print(out, "  +13 XR1 / ecm    {:06X}\n", m_.readAddr24(ace + ActionControlElement::kOffXr1));
    fmt::print(out, "  +16 XR2          {:06X}\n", m_.readAddr24(ace + ActionControlElement::kOffXr2));
    fmt::print(out, "  +19 task block   {:06X}\n", m_.readAddr24(ace + ActionControlElement::kOffTaskBlock));
    fmt::print(out, "  +28 flags        {:02X}\n", m_.readByte(ace + ActionControlElement::kOffFlags));
}

}  // namespace sim36::processors::controlstorage
