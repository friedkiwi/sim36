// The transient area, modelled as a resource with busy state and a queue
// even though a native implementation never loads microcode into it.  SSP
// can see the difference through SVC 50's queueing behaviour and SVC 18, so
// an implementation that dispatches transient IDs as plain calls diverges.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"

namespace sim36::processors::controlstorage {

// What the area calls back on the control processor: supply the native body
// of a transient.  False when there is no body here, in which case the
// caller's blocks are left unchanged and the trace says so.
class TransientHost {
public:
    virtual ~TransientHost() = default;
    virtual bool runTransient(uint8_t id, uint8_t p2, uint8_t p3, int xr1, int xr2, int taskBlock,
                              int requestBlock) = 0;
};

class TransientArea : public ITransientArea {
public:
    TransientArea(TransientHost& host, monitor::Tracer& trace) : trace_(trace), host_(host) {}

    bool busy() const override { return busy_; }
    int queueDepth() const override { return static_cast<int>(queue_.size()); }

    // IDs above 0x40 raise error 55.
    static constexpr uint8_t kMaxId = 0x40;

    // The four IDs whose table entry is the error stub.
    static bool isErrorStub(uint8_t id) { return id == 0x1D || id == 0x1E || id == 0x31 || id == 0x36; }
    // Whether this emulator currently supplies the native body.
    static bool hasEmulatedBody(uint8_t id)
    {
        return id == 0x03 || id == 0x05 || id == 0x09 || id == 0x0A || id == 0x37 || id == 0x3E;
    }

    void schedule(uint8_t id, uint8_t p2, uint8_t p3, int xr1, int xr2, int taskBlock, int requestBlock) override;
    void setNotBusy() override;

    // Discard native transient execution state before importing a stopped
    // main-storage image.  The queued requests are host objects, not part of
    // the guest image.
    void reset();

    // The transient's name; 0x0A is the timer, the one ID SA21-9436 happens to
    // name, and it agrees.
    static std::string name(uint8_t id);

private:
    struct Request {
        uint8_t id, p2, p3;
        int xr1, xr2, taskBlock, requestBlock;
    };

    void run(const Request& r);

    monitor::Tracer& trace_;
    TransientHost& host_;
    std::deque<Request> queue_;
    bool busy_ = false;
};

}  // namespace sim36::processors::controlstorage
