#include "Devices/VirtualDiskette.h"

#include <cstring>

#include <fmt/format.h>

#include "Devices/IoBlock.h"

namespace sim36::devices {

using storage::DisketteBackend;
using storage::DisketteGeometry;

// ---- DisketteIoBlock ----------------------------------------------------------------

int DisketteIoBlock::sectorBytesOf(int n)
{
    switch (n) {
        case 0: return 128;
        case 1: return 256;
        case 2: return 512;
        case 3: return 1024;
        default: return 0;
    }
}

const char* DisketteIoBlock::commandName(int command)
{
    switch (command) {
        case kCommandSeek: return "seek";
        case kCommandReadData: return "read data";
        case kCommandReadDataControlAm: return "read data/control address mark";
        case kCommandReadId: return "read identification field";
        case kCommandWriteData: return "write data";
        case kCommandWriteDataControlAm: return "write data/control address mark";
        case kCommandWriteId: return "write identification field";
        case kCommandSelectDiskette: return "select diskette";
        case kCommandEjectDiskette: return "eject diskette";
        case kCommandOrientAutoloader: return "orient autoloader";
        case kCommandAbortAutoloader: return "abort autoloader";
        case kCommandUndecodedDe: return "UNDECODED DE";
        case kCommandUndecodedDf: return "UNDECODED DF";
        default: return "unknown";
    }
}

const char* DisketteIoBlock::knownCommands()
{
    return "D0 seek, D1/D2 read, D5/D6 write, D8 select, D9 eject, DA orient, DB abort";
}

std::string DisketteIoBlock::modifierText(int modifier)
{
    std::string s;
    if ((modifier & kModifierMfm) != 0) s += "MFM ";
    if ((modifier & kModifierReturnOnNotReady) != 0) s += "return-on-not-ready ";
    s += (modifier & kModifierChrnx) != 0 ? "CHRNX" : "sequential-sector";
    if ((modifier & kModifierReturnOnEndOfTrack) != 0) s += " return-on-end-of-track";
    int n = modifier & 0x03;
    s += fmt::format(" N={} ({} B)", n, sectorBytesOf(n));
    return s;
}

// ---- VirtualDiskette -----------------------------------------------------------------

void VirtualDiskette::insert(std::unique_ptr<DisketteBackend> medium)
{
    bool change = medium_ != nullptr;
    medium_ = std::move(medium);
    if (change) mediaChanged_ = true;
}

bool VirtualDiskette::eject()
{
    if (!medium_) return false;
    medium_.reset();
    mediaChanged_ = true;
    return true;
}

bool VirtualDiskette::execute(int iob, uint8_t qByte)
{
    int command = m_.readByte(iob + DisketteIoBlock::kOffCommand);
    int modifier = m_.readByte(iob + DisketteIoBlock::kOffCommandModifier);
    int bufferField = m_.readAddr24(iob + DisketteIoBlock::kOffDataBuffer);
    uint16_t eye = m_.readHalf(iob);

    trace_.diskIo("SVC 41 iob={:06X} eye={:04X} cmd={:02X} ({}) mod={:02X} [{}] buffer={:06X} Q={:02X}", iob, eye, command,
                  DisketteIoBlock::commandName(command), modifier, DisketteIoBlock::modifierText(modifier), bufferField,
                  qByte);

    if (!hasMedium()) {
        // An empty drive is a normal machine state, not an emulator gap, so
        // the request is ANSWERED rather than refused: the guest is told the
        // drive is not ready and decides what to do.  SA21-9243-4 8-7 gives
        // 43 as "not ready or empty slot if an autoloader command", and IPL
        // phase 1's diskette wrapper tests the posted byte for hex 43 by
        // name with a distinct arm for it.
        m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
        IoBlock::complete(m_, iob, DisketteIoBlock::kNotReady);
        trace_.diskIo("  no diskette in the drive - iob+{} = {:02X}, completion {:02X} (SA21-9243-4 8-7: 43 = not ready or "
                      "empty slot). `diskette insert <image>` puts one in",
                      kOffDeviceStatus, kNotInThisConfiguration, 0x40 | DisketteIoBlock::kNotReady);
        return true;
    }

    // The latched media change: every operation answers 41 without touching
    // the medium until a recalibrate clears it (SA21-9243-4 8-13).  DE/DF
    // are outside it because they touch neither the medium nor the driver.
    if (mediaChanged_ && command != DisketteIoBlock::kCommandUndecodedDe &&
        command != DisketteIoBlock::kCommandUndecodedDf && !isRecalibrate(iob, command, modifier)) {
        trace_.diskIo("  the media has changed and the drive has not been recalibrated since - completion 41 without "
                      "touching the medium, which is what NuRdDskt's gone gate does (ffffffffc1715610). A recalibrate "
                      "clears it (SA21-9243-4 8-13)");
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return true;
    }

    switch (command) {
        case DisketteIoBlock::kCommandSeek: return seek(iob, modifier);
        case DisketteIoBlock::kCommandReadData:
        case DisketteIoBlock::kCommandReadDataControlAm: return transfer(iob, command, modifier, bufferField, false);
        case DisketteIoBlock::kCommandWriteData:
        case DisketteIoBlock::kCommandWriteDataControlAm: return transfer(iob, command, modifier, bufferField, true);
        case DisketteIoBlock::kCommandSelectDiskette: return select(iob);
        case DisketteIoBlock::kCommandEjectDiskette: return ejectCommand(iob);
        case DisketteIoBlock::kCommandOrientAutoloader:
        case DisketteIoBlock::kCommandAbortAutoloader: return autoloader(iob, command);
        case DisketteIoBlock::kCommandUndecodedDe:
        case DisketteIoBlock::kCommandUndecodedDf: return undecoded(iob, command);
        default:
            // Refused, not answered: a command this model cannot perform on a
            // diskette that IS present is an emulator gap, and answering
            // would let the guest run on believing a transfer happened.
            trace_.diskIo("  command {:02X} ({}) is NOT IMPLEMENTED. The decoded set is {}. SA21-9243-4 figure 8-3 (8-8) "
                          "lists the device's commands; this model implements the ones MSIPL phase 1 issues plus read "
                          "and write. docs/s36/diskette-ios.md",
                          command, DisketteIoBlock::commandName(command), DisketteIoBlock::knownCommands());
            IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
            return false;
    }
}

// A seek whose sequential sector address is FFFF: the recalibrate, and the
// one command that gets through a latched media change.
bool VirtualDiskette::isRecalibrate(int iob, int command, int modifier)
{
    return command == DisketteIoBlock::kCommandSeek && (modifier & DisketteIoBlock::kModifierChrnx) == 0 &&
           m_.readHalf(iob + DisketteIoBlock::kOffSequentialSector) == DisketteIoBlock::kSequentialRecalibrate;
}

// Command D0, seek.  It moves the carriage and transfers nothing, and this
// model has no carriage: a flat image is addressed absolutely on every
// access, so a seek has nothing to change and completes.  The address is
// only checked when the IOB says it has one: IPL phase 1 seeks with an IOB
// whose modifier bit 2 is CLEAR and whose block is 0x24 bytes long, so it
// has no CHRNX field at all.
bool VirtualDiskette::seek(int iob, int modifier)
{
    if ((modifier & DisketteIoBlock::kModifierChrnx) == 0) {
        int ss = m_.readHalf(iob + DisketteIoBlock::kOffSequentialSector);
        if (ss == DisketteIoBlock::kSequentialRecalibrate) {
            // SA21-9243-4 8-13: FFFF in the sequential sector address IS the
            // recalibrate, the only way to clear a not-ready condition.  It
            // is the only seek phase 1 and the reload module ever issue.
            mediaChanged_ = false;
            trace_.diskIo("  seek: sequential sector address FFFF is a RECALIBRATE (SA21-9243-4 8-13). No carriage to move "
                          "on a flat image; media-change latch cleared");
            IoBlock::complete(m_, iob, 0);
            return true;
        }

        int sc, sh, sr;
        if (!medium_->geometry().sequentialToRecord(ss, sc, sh, sr)) {
            trace_.diskIo("  seek: sequential sector {} is not on this volume ({})", ss, medium_->geometry().toString());
            IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
            return true;
        }
        trace_.diskIo("  seek: sequential sector {} = C/H/R {}/{}/{}. A flat image has no carriage to move, so there is "
                      "nothing to do",
                      ss, sc, sh, sr);
        IoBlock::complete(m_, iob, 0);
        return true;
    }

    int c = m_.readByte(iob + DisketteIoBlock::kOffCylinder);
    const DisketteGeometry& g = medium_->geometry();
    if (c >= g.cylinders()) {
        trace_.diskIo("  seek to cylinder {}: this volume has {}", c, g.cylinders());
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return true;
    }
    trace_.diskIo("  seek to cylinder {} of {}", c, g.cylinders());
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Read (D1, D2) and write (D5, D6).  Modifier bit 2 selects CHRNX
// addressing (the five bytes the supervisor forwards one at a time) or
// sequential-sector addressing (no CHRNX at all: the address at iob+0x1E,
// the size from the modifier's own bits 6-7).
bool VirtualDiskette::transfer(int iob, int command, int modifier, int bufferField, bool writing)
{
    const DisketteGeometry& g = medium_->geometry();
    int c, h, r, records, declared;
    int ss = 0;
    bool sequential = (modifier & DisketteIoBlock::kModifierChrnx) == 0;

    if (sequential) {
        ss = m_.readHalf(iob + DisketteIoBlock::kOffSequentialSector);
        int sx = m_.readHalf(iob + DisketteIoBlock::kOffSequentialSectorCount);
        // Address zero is NOT a request to continue from a device-held
        // position; it falls through to the "not a data address" refusal
        // below and says so.
        records = sx + 1;
        declared = DisketteIoBlock::sectorBytesOf(modifier & DisketteIoBlock::kModifierSectorSize);

        // END OF VOLUME.  The boundary comes from the Advanced/36's own
        // driver, which forms the LAST sector a request would touch (start +
        // count - 1) and tests it against the literal 1184 when the sector
        // size is 1024: exactly lastSequentialSector() for 8-inch 1024-byte
        // media.  Testing the END of the transfer is why a request straddling
        // the boundary transfers nothing rather than part of itself.  A
        // well-behaved reader never sees this: the reload module bounds its
        // loop by the HDR1 end of extent.  The measured-silent answer is 40
        // with residual FF and nothing transferred, traced loudly.
        if (ss >= 1 && ss + records - 1 > g.lastSequentialSector() && declared == 1024) {
            trace_.diskIo("  sequential sector {}..{} runs past the last sequential sector of this volume ({}) - end of "
                          "volume; residual FF, nothing transferred",
                          ss, ss + records - 1, g.lastSequentialSector());
            m_.writeByte(iob + DisketteIoBlock::kOffResidualSectorCount, 0xFF);
            IoBlock::complete(m_, iob, 0);
            return true;
        }

        if (!g.sequentialToRecord(ss, c, h, r)) {
            if (ss >= 1) {
                // Past the last sequential sector but below the 1184 test
                // above: nothing transferred, completion 40, the residual
                // saying how much was not moved.  SA21-9243-4 8-11 defines
                // the residual as "either a hex FF which indicates that all
                // data sectors were transferred or ... the number of sectors
                // that were not transferred, -1".  Measured: 42 here produced
                // SYS-3900, 43 SYS-3906, and 40 + residual is silent.
                trace_.diskIo("  sequential sector {} is past the last sector of this volume ({}) - nothing transferred, "
                              "residual {}, completion 40. The caller learns the diskette is finished from the short "
                              "count, not from an error",
                              ss, g.toString(), records - 1);
                m_.writeByte(iob + DisketteIoBlock::kOffResidualSectorCount, static_cast<uint8_t>(records - 1));
                IoBlock::complete(m_, iob, 0);
                return true;
            }
            // Address zero, the only way to get here.  Cylinder 0 is outside
            // this address space by SA21-9243-4 8-12.
            trace_.diskIo("  sequential sector {:04X} is not a data address on this volume ({}); SA21-9243-4 8-12 starts "
                          "at 0001 = cylinder 1 head 0 record 1 and cylinder 0 cannot be addressed this way",
                          ss, g.toString());
            IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
            return true;
        }
        trace_.diskIo("  sequential sector {} ({} record(s) of {} B) = C/H/R {}/{}/{}; count from iob+11..12, and the "
                      "System/34's +7 position iob+23 reads {:02X}",
                      ss, records, declared, c, h, r, m_.readByte(iob + DisketteIoBlock::kOffSequentialCountS34));
    } else {
        c = m_.readByte(iob + DisketteIoBlock::kOffCylinder);
        h = m_.readByte(iob + DisketteIoBlock::kOffHead);
        r = m_.readByte(iob + DisketteIoBlock::kOffRecord);
        int n = m_.readByte(iob + DisketteIoBlock::kOffRecordLength);
        int x = m_.readByte(iob + DisketteIoBlock::kOffRecordCount);
        records = x + 1;
        declared = DisketteIoBlock::sectorBytesOf(n);
        trace_.diskIo("  CHRNX C={} H={} R={} N={} ({} B) X={} ({} record(s))", c, h, r, n, declared, x, records);
    }

    if (declared == 0) {
        trace_.diskIo("  the record length code is not one of 0=128 1=256 2=512 3=1024 (SA21-9243-4 8-11)");
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return false;
    }
    if (!g.isValid(c, h, r)) {
        trace_.diskIo("  C/H/R {}/{}/{} is not on this volume ({})", c, h, r, g.toString());
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return true;
    }

    // A RECORD may be shorter than the SECTOR that holds it.  The 128-byte
    // label is the portable unit (SA21-9243-4 8-1: cylinder 0 exists in that
    // form "to ensure compatibility between systems") and the physical
    // sector size is a property of the medium: on 5 1/4-inch media the label
    // track is 26 x 256 with the SAME 128-byte label left-justified in each
    // sector.  What is still an error is asking for MORE than the sector
    // holds.
    int actual = g.trackSectorBytes(c, h);
    if (actual < declared) {
        trace_.diskIo("  the IOB asks for {}-byte records but cylinder {} head {} of this volume is recorded {} bytes per "
                      "sector, which cannot hold one. The volume declares its own geometry (VOL1+75) and this model "
                      "believes the volume. docs/file-formats/s36-diskette.md",
                      declared, c, h, actual);
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return true;
    }
    if (actual != declared)
        trace_.diskIo("  {}-byte records in {}-byte sectors: the record is the first {} bytes of each sector", declared,
                      actual, declared);

    // A transfer that runs off the end of the track continues into the next
    // one, head first then cylinder, the order HDR1 extents are written in.
    // It may NOT continue into a track whose sector size differs, because
    // the caller asked for records of one size.
    int cc = c, hh = h, rr = r;
    for (int i = 0; i < records; i++) {
        if (g.trackSectorBytes(cc, hh) < declared) {
            trace_.diskIo("  record {} of {} falls on cylinder {} head {}, which is recorded {} bytes per sector and cannot "
                          "hold a {}-byte record. A transfer does not cross into a track too small for it; SA21-9243-4 8-9 "
                          "has a 'return control on end of track' modifier bit for callers that care, and nothing in this "
                          "corpus exercises it",
                          i + 1, records, cc, hh, g.trackSectorBytes(cc, hh), declared);
            IoBlock::complete(m_, iob, DisketteIoBlock::kEndOfTrack);
            return true;
        }
        if (i + 1 < records && !medium_->next(cc, hh, rr)) {
            trace_.diskIo("  {} record(s) from C/H/R {}/{}/{} runs past the end of the volume", records, c, h, r);
            IoBlock::complete(m_, iob, DisketteIoBlock::kEndOfVolume);
            return true;
        }
    }

    int length = records * declared;
    int addr;
    if (!resolveBuffer(bufferField, addr)) {
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return false;
    }
    if (addr + length > m_.backingBytes()) {
        trace_.diskIo("  buffer {:06X} + {} outside main storage", addr, length);
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return false;
    }

    if (writing && medium_->readOnly()) {
        trace_.diskIo("  REFUSED - {} is read-only. Set `diskette_readonly = no`, or insert a copy: the media corpus under "
                      "docs/s36/media is not regenerable and nothing in this project writes it",
                      medium_->path());
        IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
        return false;
    }

    std::vector<uint8_t> buffer(static_cast<std::size_t>(length));
    if (writing) m_.read(addr, buffer.data(), length);

    cc = c;
    hh = h;
    rr = r;
    for (int i = 0; i < records; i++) {
        if (writing) {
            std::string why;
            if (!medium_->writeRecord(cc, hh, rr, buffer.data() + static_cast<std::ptrdiff_t>(i) * declared, declared, why)) {
                // The host image could not be written; the medium answers a
                // permanent error rather than the guest running on.
                trace_.diskIo("  write of C/H/R {}/{}/{} failed on the host: {}", cc, hh, rr, why);
                IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
                return false;
            }
        } else {
            std::vector<uint8_t> rec;
            if (!medium_->readRecord(cc, hh, rr, rec)) {
                trace_.diskIo("  read of C/H/R {}/{}/{} failed on the host: the image is shorter than its geometry declares",
                              cc, hh, rr);
                IoBlock::complete(m_, iob, DisketteIoBlock::kPermanentError);
                return false;
            }
            std::memcpy(buffer.data() + static_cast<std::ptrdiff_t>(i) * declared, rec.data(),
                        static_cast<std::size_t>(declared));
        }
        if (i + 1 < records) medium_->next(cc, hh, rr);
    }

    if (writing) {
        writesIssued_++;
        recordsWritten_ += records;
        trace_.diskIo("  wrote {} record(s) of {} B from guest {:06X}, C/H/R {}/{}/{} through {}/{}/{}", records, declared,
                      addr, c, h, r, cc, hh, rr);
    } else {
        m_.write(addr, buffer.data(), length);
        lastRead_ = buffer;
        hasLastRead_ = true;
        readsIssued_++;
        recordsRead_ += records;
        trace_.diskIo("  read {} record(s) of {} B to guest {:06X}, C/H/R {}/{}/{} through {}/{}/{}", records, declared, addr,
                      c, h, r, cc, hh, rr);
        if (command == DisketteIoBlock::kCommandReadData)
            trace_.diskIo("  note: command D1 bypasses DELETED control records and D2 does not (SA21-9243-4 8-8). A flat "
                          "sector image has no address-mark distinction, so the two read identically here - the "
                          "difference is recorded rather than modelled");
    }

    // Sequential-sector transfers write their completion fields back.
    // SA21-9243-4 8-11: after an error-free read or write "this field
    // contains the SS of the last sector processed +1"; the residual is
    // "either a hex FF which indicates that all data sectors were
    // transferred or ... the number of sectors that were not transferred,
    // -1".  This write is what ends a volume for the reload module.  The
    // request field iob+1E..1F is left as the caller wrote it.  The CHRNX
    // completion fields are NOT written back: the supervisor does not forward
    // them and no caller decoded so far reads them.
    if (sequential) {
        m_.writeHalf(iob + DisketteIoBlock::kOffNextSequentialSector, static_cast<uint16_t>(ss + records));
        m_.writeByte(iob + DisketteIoBlock::kOffResidualSectorCount, 0xFF);
        trace_.diskIo("  completion: iob+1C..1D next sequential sector -> {} (last processed + 1), residual FF (all "
                      "transferred)",
                      ss + records);
    }
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Command D8, select diskette.  The slot number is iob+0x14, which IPL phase
// 1 writes from guest 0x0861 immediately before issuing the command.  This
// drive has one slot, 01 (SA21-9243-4 8-10's "I/O slot 1 (carriage orient
// position)"); a magazine slot is refused rather than silently treated as
// the one drive.
bool VirtualDiskette::select(int iob)
{
    int slot = m_.readByte(iob + DisketteIoBlock::kOffAutoloaderSlot);
    if (slot != DisketteIoBlock::kIoSlot1 && slot != 0) {
        trace_.diskIo("  select slot {:02X}: this model is a single-slot drive, so only slot 01 (I/O slot 1, SA21-9243-4 "
                      "8-10) exists. Slots 02 and up are the 72MD magazine's and there is no magazine here",
                      slot);
        IoBlock::complete(m_, iob, DisketteIoBlock::kNotReady);
        return true;
    }
    trace_.diskIo("  select slot {:02X} -> {} ({})", slot, medium_->path(), medium_->geometry().toString());
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Command D9, eject diskette: the one device command whose effect on the
// host is real.
bool VirtualDiskette::ejectCommand(int iob)
{
    trace_.diskIo("  eject {}", medium_->path());
    eject();
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Commands DA orient and DB abort, the autoloader pair.  SA21-9243-4 marks
// both "72MD only": they move a picker arm this drive does not have, so on a
// single-slot drive there is nothing to do and they complete.  Whether real
// hardware without a magazine answers 40 or 43 is not established.
bool VirtualDiskette::autoloader(int iob, int command)
{
    trace_.diskIo("  {} ({:02X}) is a 72MD autoloader command (SA21-9243-4 8-8); this drive has one slot and no picker, so "
                  "there is nothing to move. Completed 40 - see docs/s36/diskette-ios.md for why that answer is not "
                  "certain",
                  DisketteIoBlock::commandName(command), command);
    IoBlock::complete(m_, iob, 0);
    return true;
}

// DE and DF, the two commands IPL phase 1 issues that are not in the
// documented set.  Answered rather than refused, and the reason is
// measured: with these completed, phase 1 goes on to seek and to read the
// label track, and its VOL1 field tests pass on real media.  No buffer is
// touched and no medium is moved.
bool VirtualDiskette::undecoded(int iob, int command)
{
    undecodedCommands_++;
    trace_.diskIo("  command {:02X} is UNDECODED - it is not in SA21-9243-4 8-8's set and nothing in this corpus names it. "
                  "Answered complete without touching the medium or the buffer, because phase 1 issues it immediately "
                  "before the seek and the read that DO work; refusing stops the machine here. Request {} of its kind. "
                  "docs/s36/diskette-ios.md",
                  command, undecodedCommands_);
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Same field and same flag as the disk's: the buffer address is resolved
// for EVERY device SVC, real unless bit 0x800000 is set, then translated
// through the task's ATRs.
bool VirtualDiskette::resolveBuffer(int bufferField, int& addr)
{
    if ((bufferField & IoBlock::kDataBufferTranslated) == 0) {
        addr = bufferField & 0x7FFFFF;
        return true;
    }
    if (m_.translate(static_cast<uint16_t>(bufferField), machine::MachineState::kAtrTaskGroup0, true, addr)) {
        trace_.diskIo("  buffer {:06X} is task-translated -> real {:06X}", bufferField, addr);
        return true;
    }
    trace_.diskIo("  buffer {:06X} is task-translated and its page is not mapped", bufferField);
    addr = 0;
    return false;
}

}  // namespace sim36::devices
