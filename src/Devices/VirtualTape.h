// Tape IOS, SVC 46.  The volume's unit definition table declares a 6380
// TAPE TAP01, so from the guest's side the drive exists; what this models is
// that drive, driving a generic tape backend (ITapeBackend; the folder
// format is FolderTapeBackend).  An empty drive is a normal machine state,
// not an emulator gap.
//
// The command set was recovered behaviourally from the Advanced/36's own
// tape emulation, not from a manual: SA21-9436-5 "Tape IOS" (3-138.1)
// defers the whole IOB (command codes, field offsets, status codes) to the
// System Data Areas manual, which is not in this corpus.  What is VERIFIED:
// SVC 46 routing; the shared device-IOB header (iob+0x06 completion, +0x0A
// command, +0x0B modifier, +0x0D/0x0F data buffer); the data-transfer
// length at iob+0x10; the read commands 0x17/0x22 and write commands
// 0x18/0x21 (their DIRECTION proved by which way the byte-mover copies); the
// MIC/status tail at iob+0x1D..0x1F.  What is INFERRED: the exact per-condition
// completion nibble and MIC for a tape mark, end of data and write-protect;
// Command 0x16's labeled-dataset search and position are also verified from
// NuTapeIo::tapFind.  The remaining positioning command mappings and exact
// condition completions are still unsupported rather than inferred.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Storage/TapeBackend.h"

namespace sim36::devices {

// The tape input/output block, recovered by disassembling the Advanced/36's
// tape emulation.  It shares the device-IOB header with the disk, diskette
// and work-station blocks; the fields beyond that are tape's own.  Offsets
// are in hex.  Each field is VERIFIED (read at that offset by a decoded
// routine) or INFERRED (present and bounded, exact role not confirmed).
class NuTaIob {
public:
    // ---- shared device-IOB header (VERIFIED) ----
    static constexpr int kOffCompletion = 0x06;   // the 0x40 bit is "complete", the low nibble the code
    static constexpr int kOffCommand = 0x0A;
    static constexpr int kOffModifier = 0x0B;
    static constexpr int kOffDataBuffer = 0x0D;   // 3 bytes, rightmost at 0x0F; 0x800000 = task-translated

    // ---- tape-specific data-transfer fields ----
    static constexpr int kOffBlockLength = 0x10;   // halfword, 1..0x7FFF for the data commands (VERIFIED)
    static constexpr int kOffReturnedLength = 0x12; // label bytes returned by tapLbls2 (VERIFIED)
    static constexpr int kOffCount0 = 0x14;        // halfword bounded by iob+0x10; role INFERRED, not written back
    static constexpr int kOffCount1 = 0x16;        // its companion; role INFERRED
    static constexpr int kOffMicPrefix = 0x1D;     // guest #CATP tests a two-byte prefix here (VERIFIED)
    static constexpr int kOffMic = 0x1E;           // SLIC-facing MIC halfword; overlaps the guest status tail
    static constexpr int kOffDataFlag = 0x21;      // required non-zero for a data command; role INFERRED

    // ---- commands ----
    static constexpr int kCommandMin = 0x01;   // NuTapeIo::entry subtracts one before its unsigned range check
    static constexpr int kCommandMax = 0x31;   // VERIFIED: (command - 1) must be <= 48 decimal
    static constexpr int kCommandActivate = 0x01;
    static constexpr int kCommandSetSession = 0x02;
    static constexpr int kCommandInitializeStandard = 0x12;
    static constexpr int kCommandReadVolumeLabels = 0x13;
    static constexpr int kCommandFindDataSet = 0x16;
    static constexpr int kCommandReadData = 0x17;      // the tape buffer is copied INTO the guest buffer
    static constexpr int kCommandReadDataAlt = 0x22;   // the 0x17/0x22 distinction is not recovered
    static constexpr int kCommandWriteData = 0x18;     // the guest buffer is copied INTO the tape buffer
    static constexpr int kCommandWriteDataAlt = 0x21;
    static constexpr int kCommandControl = 0x10;       // with modifier 0x10: clear completion and return, no transfer
    static constexpr int kControlModifier = 0x10;
    static constexpr int kMaxBlockLength = 0x7FFF;

    // ---- completion low nibbles (posted at iob+0x06 with the 0x40 bit) ----
    static constexpr int kCompletionOk = 0;          // VERIFIED
    static constexpr int kCompletionError = 4;       // a non-success class; 4 and 5 are written on the error arms
    static constexpr int kCompletionEndOfFile = 5;   // INFERRED: kept distinct from the plain error

    // ---- SLIC-facing MIC values (iob+0x1E); exact guest encodings can differ ----
    static constexpr int kMicInvalidCommand = 0x000A;
    static constexpr int kMicLength = 0x0025;
    static constexpr int kMicTapeMark = 0x0025;
    static constexpr int kMicWriteProtected = 0x000A;

    static const char* commandName(int command);
};

class VirtualTape {
public:
    VirtualTape(machine::MachineState& m, monitor::Tracer& trace) : m_(m), trace_(trace) {}

    // The cartridge in the drive, or null.  Mount and dismount are runtime
    // operations because that is what a tape drive is.
    storage::ITapeBackend* medium() { return medium_.get(); }
    const storage::ITapeBackend* medium() const { return medium_.get(); }
    // True once a cartridge is mounted AND spun up.
    bool hasCartridge() const { return medium_ != nullptr && medium_->loaded(); }

    long long readsIssued() const { return readsIssued_; }
    long long writesIssued() const { return writesIssued_; }
    long long controlOps() const { return controlOps_; }
    long long unmappedCommands() const { return unmappedCommands_; }
    const std::vector<uint8_t>& lastRead() const { return lastRead_; }
    bool hasLastRead() const { return hasLastRead_; }

    // Mount a tape backend and spin it up (SCSI LOAD).  Any cartridge
    // already in the drive is unloaded first.
    void load(std::unique_ptr<storage::ITapeBackend> medium);
    // Dismount the cartridge, flushing any pending writes.
    bool unload();

    // The byte written into the guest structure for a device this
    // configuration does not provide, and where it goes: the same decline
    // every device uses.
    static constexpr uint8_t kNotInThisConfiguration = 0x20;
    static constexpr int kOffDeviceStatus = 12;

    // Execute the IOB at `iob`.  False when the request was refused; the
    // completion code has been posted either way.
    bool execute(int iob, uint8_t qByte);

private:
    bool read(int iob, int command, int length, int bufferField);
    bool write(int iob, int command, int length, int bufferField);
    bool activate(int iob, int modifier);
    bool setSession(int iob, int modifier);
    bool initializeStandard(int iob, int modifier, int length, int bufferField);
    bool readVolumeLabels(int iob, int modifier, int length, int bufferField);
    bool findDataSet(int iob, int modifier, int length, int bufferField);
    bool control(int iob, int modifier);
    bool validLength(int iob, int length);
    bool notReady(int iob);
    void postError(int iob, int completion, int mic);

    machine::MachineState& m_;
    monitor::Tracer& trace_;
    std::unique_ptr<storage::ITapeBackend> medium_;
    long long readsIssued_ = 0, writesIssued_ = 0, controlOps_ = 0, unmappedCommands_ = 0;
    std::vector<uint8_t> lastRead_;
    bool hasLastRead_ = false;
};

}  // namespace sim36::devices
