#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::processors::controlstorage {

using machine::MachineState;

void ActionControlElement::build(MachineState& m, int ace, int rb, int tb, uint8_t qByte,
                                 bool captureEcmMultipleWait)
{
    m.writeHalf(ace + kOffEyecatcher, kEyecatcher);
    m.writeAddr24(ace + kOffChainLink, 0);      // not yet queued
    m.writeByte(ace + kOffTbByte7, m.readByte(tb + 7));
    // ace+6 takes the halfword at rb+24, the requester's instruction address.
    m.writeHalf(ace + kOffRbCounter, m.readHalf(rb + RequestBlock::kOffIar));
    m.writeByte(ace + kOffInlineParm1, m.readByte(rb + RequestBlock::kOffInline1));
    m.writeAddr24(ace + kOffXr1, RequestBlock::readXr1Field(m, rb));
    m.writeAddr24(ace + kOffXr2, RequestBlock::readXr2Field(m, rb));
    m.writeAddr24(ace + kOffTaskBlock, tb);
    m.writeHalf(ace + kOffEventType, 0);
    m.writeAddr24(ace + kOffZero24, 0);
    m.writeByte(ace + kOffZero24 + 3, 0);
    // The ECM's multiple-wait bit is part of the request at submission
    // time.  Preserve it in the ACE while the requester's ATR file is still
    // selected: by the time the completion is consumed, the task may have
    // transferred to another program and the same translated address can be
    // unmapped (or name unrelated storage).  nuevt accepts either this ACE
    // flag or the live ECM bit, so latching it here retains that eligibility
    // across the mapping change.
    int ecm = 0;
    const int ecmField = RequestBlock::readXr1Field(m, rb);
    const bool ecmMultipleWait = captureEcmMultipleWait && ecmField != 0 &&
                                 m.resolveGuest24(ecmField, false, ecm) &&
                                 (m.readByte(ecm + Ecm::kOffMultiWait) & 0x80) != 0;
    m.writeByte(ace + kOffFlags,
                static_cast<uint8_t>(kFlagsBase | (qByte & kFlagsMultipleWait) |
                                     (ecmMultipleWait ? kFlagsMultipleWait : 0)));
    m.writeAddr24(ace + kOffXr1Copy, RequestBlock::readXr1Field(m, rb));
}

void ActionControlElement::applyTaskAssociation(MachineState& m, int ace, uint8_t qByte)
{
    if ((qByte & kQDifferentTask) != 0) m.writeAddr24(ace + kOffTaskBlock, m.readAddr24(ace + kOffXr2));
}

void Ecm::post(MachineState& m, int ecm, int completionCode)
{
    m.writeByte(ecm + kOffCompletion, static_cast<uint8_t>(kComplete | (completionCode & 0x0F)));
    m.writeAddr24(ecm + kOffAceAddress, 0);   // unlink; a posted mask is not re-postable
}

}  // namespace sim36::processors::controlstorage
