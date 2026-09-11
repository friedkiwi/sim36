// The architected seam between the MSP and whatever provides control storage
// services.  IBM shipped one SSP across the 5360, 5362 and 5364 and then
// reimplemented the same interface natively for the Advanced/36, so this
// boundary is real rather than imposed.  It is expressed as state and
// transitions rather than a bag of per-SVC methods.
#pragma once

#include <cstdint>
#include <string>

namespace sim36::processors { class MainStorageProcessor; }

namespace sim36::processors::controlstorage {

// How an SVC is dispatched.  39 of the 60 documented SVCs are overlapped,
// so out-of-order completion is the common case.
enum class DispatchClass {
    Immediate,   // serviced at once; the whole MSP stops until it completes
    Overlapped,  // only the issuing task waits; completion out of order is legal
    Delayed,     // queued as an action control element
};

// An SVC as the MSP presents it: F4 Q R [0-3 inline bytes].
struct SvcRequest {
    uint8_t r = 0;
    uint8_t q = 0;
    uint8_t inline1 = 0, inline2 = 0, inline3 = 0;
    DispatchClass dispatch = DispatchClass::Immediate;
    // Issuing MSP instruction and resolved member, when the request came
    // from an actual SVC instruction.
    uint16_t sourceIar = 0;
    std::string sourceMember;
    // Guest address the request block lives at, for handlers that walk it.
    int requestBlock = 0;
    int taskBlock = 0;
};

// Control storage transient scheduling.  The ID space is 0x00-0x40.  The
// area must be modelled even though a native implementation never loads
// anything into it: SSP can observe it through SVC 50's queueing and SVC 18.
class ITransientArea {
public:
    virtual ~ITransientArea() = default;
    virtual bool busy() const = 0;
    virtual int queueDepth() const = 0;
    // SVC 50.  If the area is not busy the transient runs; if it is, the
    // request is queued.
    virtual void schedule(uint8_t transientId, uint8_t inline2, uint8_t inline3, int xr1, int xr2,
                          int taskBlock, int requestBlock) = 0;
    // SVC 18, Set Transient Area Not Busy.
    virtual void setNotBusy() = 0;
};

class IControlStorageProcessor {
public:
    virtual ~IControlStorageProcessor() = default;

    virtual std::string modelName() const = 0;

    // The main storage processor this control processor owns.  The MSP
    // begins hardstopped and is started by the CSP; nothing but the CSP
    // should hold a reference to it.
    virtual MainStorageProcessor& mainStorage() = 0;

    // Stage A: get the control processor itself executing.  On real hardware
    // the microcode load; on a virtual CSP vacuous, but kept explicit.
    virtual void bringUpControlProcessor() = 0;

    // Stage B: the running control processor performs the main storage IPL:
    // guest low storage built, 4 KB of phase 1 at guest 0x1000 from sector
    // 8191, a task block at 0xF00, that task posted, the MSP running.  The
    // MSP is handed an untranslated machine.
    virtual void iplMainProcessor() = 0;

    virtual void controlStorageTerminate() = 0;

    // Classify without executing, so the scheduler can sequence first.
    virtual DispatchClass classify(uint8_t rByte) const = 0;
    // Is this R-byte dispatched at all?
    virtual bool isImplemented(uint8_t rByte) const = 0;
    // Execute a supervisor call.  False if the call was rejected.
    virtual bool svc(SvcRequest& req) = 0;
    // Why the last svc() returned false, or empty.  A refused call stops the
    // machine, and the R-byte alone is not the reason.
    virtual std::string lastRefusal() const = 0;

    // The MSP raised a storage-protection violation, a level 5 interrupt to
    // the control processor.  True if a task was dispatched and the MSP
    // should keep running, false if the machine must stop.
    virtual bool raiseStorageProtection(uint16_t logical, bool forWrite) = 0;

    virtual ITransientArea& transients() = 0;
};

}  // namespace sim36::processors::controlstorage
