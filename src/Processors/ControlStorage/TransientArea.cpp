#include "Processors/ControlStorage/TransientArea.h"

namespace sim36::processors::controlstorage {

void TransientArea::schedule(uint8_t id, uint8_t p2, uint8_t p3, int xr1, int xr2, int taskBlock, int requestBlock)
{
    if (id > kMaxId) {
        trace_.csp("transient {:02X}: error 55 (out of range)", id);
        return;
    }
    Request r{id, p2, p3, xr1, xr2, taskBlock, requestBlock};
    if (busy_) {
        queue_.push_back(r);
        trace_.csp("transient {:02X} queued (depth {})", id, queue_.size());
        return;
    }
    run(r);
}

void TransientArea::run(const Request& r)
{
    busy_ = true;
    if (isErrorStub(r.id))
        trace_.csp("transient {:02X}: not implemented on this model", r.id);
    else {
        trace_.csp("transient {:02X} ({})", r.id, name(r.id));
        // SA21-9436 3-142: "control is passed to the transient... Control is
        // returned to the caller after the transient has executed
        // completely".  Tracing the identifier and returning is NOT that: the
        // caller reads its own parameter block back unchanged and uses
        // whatever was already in it, which is how MSIPL phase 2 once asked
        // the disk for sector 12 649 471 - the three bytes it read as a
        // sector were the area identifier it had just passed in.
        if (!host_.runTransient(r.id, r.p2, r.p3, r.xr1, r.xr2, r.taskBlock, r.requestBlock))
            trace_.csp("transient {:02X} ({}): no body here - the caller's blocks at "
                       "XR1 = {:06X}, XR2 = {:06X} are UNCHANGED and it will read them back",
                       r.id, name(r.id), r.xr1, r.xr2);
    }
    // Control returns to the caller only after the transient has executed
    // completely (SA21-9436 3-142), so the area frees here for now.  A
    // transient that genuinely blocks would leave the area busy and call
    // setNotBusy later.
    setNotBusy();
}

void TransientArea::setNotBusy()
{
    busy_ = false;
    if (!queue_.empty()) {
        Request r = queue_.front();
        queue_.pop_front();
        run(r);
    }
}

void TransientArea::reset()
{
    busy_ = false;
    queue_.clear();
}

std::string TransientArea::name(uint8_t id)
{
    switch (id) {
    case 0x03: return "nulc";
    case 0x04: return "nurderp";
    case 0x08: return "nuwe";
    case 0x0A: return "nutix (timer)";
    case 0x0E: return "nutr1svc";
    case 0x15: return "nupd";
    case 0x19: return "nuacdump";
    case 0x1B: return "nuwkert";
    case 0x26: return "nuTaMsg";
    case 0x37: return "nutod";
    case 0x38: return "nuApplyPTF";
    case 0x39: case 0x3A: return "nuchgsysvl";
    case 0x3B: return "nucmutil";
    case 0x3C: return "nuavmstg";
    case 0x3D: return "nuwasvtb";
    // The host-transfer parameter-list service, selected on SVC inline
    // parameter 2: 01 retrieves, 02 ends, anything else is error 79.  It is
    // not a sign-on prompt and has no disk member.  SSP guards the call with
    // "am I a hosted M36?" by testing guest 08E0 against 0x80, which the IPL
    // sets for this personality.
    case 0x3E: return "nuRtvTfrM36/nuEndTfrM36 (TFRM36 plist)";
    case 0x3F: return "nuoqi";
    case 0x40: return "nurun400";
    default: return "nuerio";
    }
}

}  // namespace sim36::processors::controlstorage
