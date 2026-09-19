// The native Advanced/36 boundary for System/36 extended-control-storage
// language assists.  XFER saves the MSP in its request block, then calls one
// of these synchronously.  An assist owns language execution only: task
// scheduling and restarting the MSP remain control-processor responsibilities.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace sim36::machine { class MachineState; }
namespace sim36::monitor { class Tracer; }

namespace sim36::processors::controlstorage {

enum class AssistStatus {
    Completed,
    NotImplemented,
    GuestError,
    Failed,
};

struct AssistResult {
    AssistStatus status = AssistStatus::Failed;
    std::string detail;

    bool completed() const { return status == AssistStatus::Completed; }
    static AssistResult complete() { return {AssistStatus::Completed, {}}; }
    static AssistResult notImplemented(std::string detail)
    {
        return {AssistStatus::NotImplemented, std::move(detail), -1};
    }
    // Preserve a guest-requested abnormal termination (for example NuBasic's
    // fatal invalid-state nuerr2(61)) separately from a host/emulator failure.
    // Recoverable BASIC arithmetic and range conditions instead return through
    // guest continuation vectors and must not use this result.
    static AssistResult guestError(uint8_t code, std::string detail)
    {
        return {AssistStatus::GuestError, std::move(detail), code};
    }
    static AssistResult failed(std::string detail) { return {AssistStatus::Failed, std::move(detail), -1}; }

    int errorCode = -1;
};

// Everything the two assists need from the surrounding emulator, without
// handing either one the CSP itself.  The SLIC implementations consume the
// current task/request blocks, follow translated 24-bit guest pointers, move
// byte strings across page boundaries, update the saved MSP register image,
// and trace failures.  Numeric and bytecode state belongs to the individual
// assist, or (where architecturally visible) to guest storage.
class AssistContext {
public:
    AssistContext(machine::MachineState& machine, monitor::Tracer& trace, int taskBlock,
                  int requestBlock, uint8_t q, uint8_t r, uint16_t sourceIar)
        : machine_(machine), trace_(trace), taskBlock_(taskBlock), requestBlock_(requestBlock),
          q_(q), r_(r), sourceIar_(sourceIar) {}

    int taskBlock() const { return taskBlock_; }
    int requestBlock() const { return requestBlock_; }
    uint8_t q() const { return q_; }
    uint8_t r() const { return r_; }
    uint16_t sourceIar() const { return sourceIar_; }
    monitor::Tracer& trace() { return trace_; }

    // A guest address is real when bit 0x800000 is clear and task-translated
    // when it is set.  Range operations validate every translated page before
    // moving bytes, and therefore also cover strings and numeric stack values
    // that cross a 2 KiB page boundary.
    bool resolveGuest(int address, bool forWrite, int& real);
    bool readGuest(int address, uint8_t* destination, int length);
    bool writeGuest(int address, const uint8_t* source, int length);
    bool readGuestByte(int address, uint8_t& value);
    bool writeGuestByte(int address, uint8_t value);
    bool readGuestHalf(int address, uint16_t& value);
    bool writeGuestHalf(int address, uint16_t value);
    static int guestOffset(int address, int displacement);

    // The request block is both the XFER parameter block and the saved MSP
    // register image.  These accessors keep its physical-storage nature
    // explicit and avoid restoring stale live registers from an assist.
    uint8_t requestByte(int offset);
    uint16_t requestHalf(int offset);
    int requestAddress(int offset);
    uint16_t requestWorkRegister(int number);
    void setRequestByte(int offset, uint8_t value);
    void setRequestHalf(int offset, uint16_t value);
    void setRequestAddress(int offset, int value);
    void setRequestWorkRegister(int number, uint16_t value);

    uint8_t taskByte(int offset);
    uint16_t taskHalf(int offset);
    int taskAddress(int offset);
    void setTaskByte(int offset, uint8_t value);
    void setTaskHalf(int offset, uint16_t value);
    void setTaskAddress(int offset, int value);

    const std::string& storageFailure() const { return storageFailure_; }

private:
    void failedStorage(const char* operation, int address, int length);

    machine::MachineState& machine_;
    monitor::Tracer& trace_;
    int taskBlock_;
    int requestBlock_;
    uint8_t q_;
    uint8_t r_;
    uint16_t sourceIar_;
    std::string storageFailure_;
};

class IExtendedControlStoreAssist {
public:
    virtual ~IExtendedControlStoreAssist() = default;
    virtual const char* name() const = 0;
    virtual AssistResult execute(AssistContext& context) = 0;
};

}  // namespace sim36::processors::controlstorage
