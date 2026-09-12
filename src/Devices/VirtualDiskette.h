// Diskette IOS, SVC 41.  Delayed and privileged; XR1 carries the IOB and the
// Q-byte's bit 7 is "wait on this event" (SA21-9436 3-133).
//
// This device has no supervisor-side command table, and that is a finding
// rather than a gap: the supervisor copies the IOB into a request structure
// and hands the whole thing to the diskette driver without interpreting the
// commands, which are the device's own.  So they come from the device's own
// documentation: SA21-9243-4 (System/34 Functions Reference) figure 8-3,
// pages 8-6..8-11.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Storage/DisketteBackend.h"

namespace sim36::devices {

// The diskette input/output block, which is not the disk's.
//
// Bytes 0..6 are the event control mask and +0x0A/+0x0B the command and its
// modifier, shared with every other device IOB, and +0x0D..0x0F is the data
// buffer, resolved for every device SVC.  Everything else differs: the
// layout is SA21-9243-4 figure 8-3 (8-6..8-11) shifted by +7, and the shift
// is pinned at three points by things this machine does (the command at
// +0x0A, the data address at +0x0D which IPL phase 1 stores an assigned
// buffer into, and the five-byte CHRNX at +0x24..0x28 which phase 1 fills
// with 00 00 07 00 02: cylinder 0, head 0, record 7, 128-byte records, three
// of them, the VOL1 label and the two HDR1s after it).
class DisketteIoBlock {
public:
    // Only five of these are confirmed (the command, the modifier, the data
    // address, the autoloader slot and CHRNX) and those are the only five
    // the model reads.  The rest are a best fit of the field shapes onto the
    // System/34 layout: the +7 shift is exact from 0x1C upwards and loose
    // between 0x10 and 0x1B.
    static constexpr int kOffFlags = 0x07;
    static constexpr int kOffCommand = 0x0A;
    static constexpr int kOffCommandModifier = 0x0B;
    static constexpr int kOffDataBuffer = 0x0D;   // 3 bytes, rightmost at 0x0F
    static constexpr int kOffStatus0 = 0x10;      // 0x10..0x13
    // Number of sectors - 1 for a SEQUENTIAL-SECTOR transfer: two bytes,
    // right-justified, at 0x11..0x12.  IPL phase 1 builds it beside the
    // address, and the value it writes (27 = 28 sectors from sequential
    // sector 5) ends exactly at the last sector of #IPLBOOT's extent.
    static constexpr int kOffSequentialSectorCount = 0x11;
    static constexpr int kOffAutoloaderSlot = 0x14;   // phase 1 writes it before D8
    static constexpr int kOffTaskBlock = 0x15;        // 3 bytes
    static constexpr int kOffErrorRetryCount = 0x18;
    // The next sequential sector to process, written by the DEVICE and read
    // by the caller: SA21-9243-4 8-11's "the SS of the last sector processed
    // +1".  The reload module's read loop copies 0x1C..0x1D into its request
    // at 0x1E..0x1F after every transfer and compares it with the HDR1 end
    // of extent; this write is what ends a volume.
    static constexpr int kOffNextSequentialSector = 0x1C;
    // The sequential sector address the caller supplies, used when modifier
    // bit 2 is clear (no CHRNX at all).  Phase 1's recalibrate seek carries
    // FFFF here, which SA21-9243-4 8-12 names as the recalibrate address.
    static constexpr int kOffSequentialSector = 0x1E;
    static constexpr int kOffResidualSectorCount = 0x20;
    // Reads 0000 in every observed IOB; role unknown, nothing reads it.
    static constexpr int kOffStartingSectorAddress = 0x21;
    // Where the System/34's sequential-sector count would land under the +7
    // shift; reads 00 on every observed IOB, so it is traced, never read.
    static constexpr int kOffSequentialCountS34 = 0x23;
    // CHRNX: cylinder, head, record, record length code, count - 1.
    static constexpr int kOffCylinder = 0x24;
    static constexpr int kOffHead = 0x25;
    static constexpr int kOffRecord = 0x26;
    static constexpr int kOffRecordLength = 0x27;
    static constexpr int kOffRecordCount = 0x28;

    // Command codes: the System/34 set (SA21-9243-4 8-8) plus D0.  IPL phase
    // 1 issues DB, DA, D8, D0 and D2 in that order, which is abort
    // autoloader, orient autoloader, select diskette, seek, read: the exact
    // sequence the manual prescribes.
    static constexpr int kCommandSeek = 0xD0;
    static constexpr int kCommandReadData = 0xD1;
    static constexpr int kCommandReadDataControlAm = 0xD2;
    static constexpr int kCommandReadId = 0xD3;
    static constexpr int kCommandWriteData = 0xD5;
    static constexpr int kCommandWriteDataControlAm = 0xD6;
    static constexpr int kCommandWriteId = 0xD7;
    static constexpr int kCommandSelectDiskette = 0xD8;
    static constexpr int kCommandEjectDiskette = 0xD9;
    static constexpr int kCommandOrientAutoloader = 0xDA;
    static constexpr int kCommandAbortAutoloader = 0xDB;
    // DE and DF are not in the System/34 set and nothing names them.  They
    // are the pair IPL phase 1 issues, and on a drive with no autoloader DE
    // is the first diskette command of the whole IPL.
    static constexpr int kCommandUndecodedDe = 0xDE;
    static constexpr int kCommandUndecodedDf = 0xDF;

    // Autoloader slot 01, "I/O slot 1 (carriage orient position)",
    // SA21-9243-4 8-10: the only slot a drive without a magazine has.
    static constexpr int kIoSlot1 = 0x01;

    // Modifier bit 2 (mask 0x20): this IOB has a CHRNX field; clear selects
    // sequential-sector addressing.
    static constexpr int kModifierChrnx = 0x20;
    static constexpr int kModifierMfm = 0x80;
    static constexpr int kModifierReturnOnNotReady = 0x40;
    static constexpr int kModifierReturnOnEndOfTrack = 0x04;
    // Modifier bits 6-7: the physical record length for sequential-sector
    // addressing, "the same as bits 6 and 7 of the command modifier" as the
    // N byte (SA21-9243-4 8-11).
    static constexpr int kModifierSectorSize = 0x03;

    // SA21-9243-4 8-13: a recalibrate is started by a seek with "either a
    // sequential sector address of hexadecimal FFFF or a CHRNX cylinder byte
    // of hexadecimal FF".
    static constexpr int kSequentialRecalibrate = 0xFFFF;

    // Completion codes, SA21-9243-4 8-7, the diskette's own: 40 successful,
    // 41 permanent I/O error, 42 end of volume, 43 not ready or empty slot,
    // 44 end of track, 49 unsupported control record.  The event control
    // mask's iob+6 carries 0x40 | code, so these are the low nibbles.
    static constexpr int kPermanentError = 1;
    static constexpr int kEndOfVolume = 2;
    static constexpr int kNotReady = 3;
    static constexpr int kEndOfTrack = 4;
    static constexpr int kUnsupportedControlRecord = 9;

    // Record length code N -> bytes, or 0 if N is not one.
    static int sectorBytesOf(int n);
    static const char* commandName(int command);
    static const char* knownCommands();
    // The modifier, spelled out: IBM numbers bits from 0 = most significant.
    static std::string modifierText(int modifier);
};

class VirtualDiskette {
public:
    VirtualDiskette(machine::MachineState& m, monitor::Tracer& trace) : m_(m), trace_(trace) {}

    // What is in the drive, or null.  Insert and eject are runtime
    // operations because that is what a diskette drive is: an empty drive is
    // a completely normal machine state.
    storage::DisketteBackend* medium() { return medium_.get(); }
    const storage::DisketteBackend* medium() const { return medium_.get(); }
    bool hasMedium() const { return medium_ != nullptr; }

    long long readsIssued() const { return readsIssued_; }
    long long recordsRead() const { return recordsRead_; }
    long long writesIssued() const { return writesIssued_; }
    long long recordsWritten() const { return recordsWritten_; }
    // The most recent transfer, so the monitor can show what a read produced.
    const std::vector<uint8_t>& lastRead() const { return lastRead_; }
    bool hasLastRead() const { return hasLastRead_; }
    // How many DE/DF requests have been answered without being understood.
    long long undecodedCommands() const { return undecodedCommands_; }

    // Swapping media under a running machine is a CHANGE and latches; the
    // power-on load into an empty drive is not.
    void insert(std::unique_ptr<storage::DisketteBackend> medium);
    bool eject();

    // The byte written into the guest structure for a device this
    // configuration does not provide: the machine's own decline.
    static constexpr uint8_t kNotInThisConfiguration = 0x20;
    static constexpr int kOffDeviceStatus = 12;

    // Execute the IOB at `iob`.  False when the request was refused; the
    // completion code has been posted either way.
    bool execute(int iob, uint8_t qByte);

private:
    bool isRecalibrate(int iob, int command, int modifier);
    bool seek(int iob, int modifier);
    bool transfer(int iob, int command, int modifier, int bufferField, bool writing);
    bool select(int iob);
    bool ejectCommand(int iob);
    bool autoloader(int iob, int command);
    bool undecoded(int iob, int command);

    machine::MachineState& m_;
    monitor::Tracer& trace_;
    std::unique_ptr<storage::DisketteBackend> medium_;
    // The attachment has seen its media change and has not been
    // recalibrated since.  A media change is LATCHED, and while it is
    // latched the answer is completion 41, permanent error, given without
    // touching the medium.  What clears it is the recalibrate, on the
    // manual's own authority: SA21-9243-4 8-13, "A recalibrate is the only
    // way to clear a not ready condition."
    bool mediaChanged_ = false;
    long long readsIssued_ = 0, recordsRead_ = 0, writesIssued_ = 0, recordsWritten_ = 0;
    long long undecodedCommands_ = 0;
    std::vector<uint8_t> lastRead_;
    bool hasLastRead_ = false;
};

}  // namespace sim36::devices
