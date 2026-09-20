#include "Devices/VirtualTape.h"

#include <algorithm>

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::devices {

using processors::controlstorage::Ecm;
using storage::TapeResult;

const char* NuTaIob::commandName(int command)
{
    switch (command) {
        case kCommandActivate: return "activate";
        case kCommandSetSession: return "set session";
        case kCommandInitializeStandard: return "initialize standard labels";
        case kCommandReadVolumeLabels: return "read volume labels";
        case kCommandWriteHeaderLabels: return "write header labels";
        case kCommandFindDataSet: return "find data set";
        case kCommandReadData: return "read data";
        case kCommandReadDataAlt: return "read data (alt)";
        case kCommandWriteData: return "write data";
        case kCommandFinishDataSet: return "finish data set";
        case kCommandFinalizeVolume: return "finalize volume";
        case kCommandWriteDataAlt: return "write data (alt)";
        case kCommandUnload: return "unload";
        case kCommandControl: return "control";
        default: return "unmapped";
    }
}

void VirtualTape::load(std::unique_ptr<storage::ITapeBackend> medium)
{
    unload();
    activeHeaderLabels_.clear();
    medium_ = std::move(medium);
    if (medium_) medium_->load();
}

bool VirtualTape::unload()
{
    if (!medium_) return false;
    medium_->unload();
    medium_.reset();
    activeHeaderLabels_.clear();
    return true;
}

bool VirtualTape::execute(int iob, uint8_t qByte)
{
    int command = m_.readByte(iob + NuTaIob::kOffCommand);
    int modifier = m_.readByte(iob + NuTaIob::kOffModifier);
    int length = m_.readHalf(iob + NuTaIob::kOffBlockLength);
    int bufferField = m_.readAddr24(iob + NuTaIob::kOffDataBuffer);
    uint16_t eye = m_.readHalf(iob);

    trace_.diskIo("SVC 46 iob={:06X} eye={:04X} cmd={:02X} ({}) mod={:02X} len={} buffer={:06X} Q={:02X}", iob, eye, command,
                  NuTaIob::commandName(command), modifier, length, bufferField, qByte);

    if (!hasCartridge()) {
        // An empty (or unloaded) drive is a normal machine state, so the
        // request is ANSWERED rather than refused: the drive reports
        // not-ready and the guest decides.  The device-status byte and the
        // not-ready completion mirror the diskette's empty-drive answer; the
        // exact tape MIC for not-ready is INFERRED, so none is written.
        m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
        IoBlock::complete(m_, iob, NuTaIob::kCompletionError);
        trace_.diskIo("  no cartridge loaded - iob+{} (0x0C) = {:02X}, completion {:02X}. `tapesvc`/an operator mounts one. "
                      "docs/s36/tape-svc-integration.md",
                      kOffDeviceStatus, kNotInThisConfiguration, Ecm::kComplete | NuTaIob::kCompletionError);
        return true;
    }

    // NuTapeIo::entry accepts 1..0x31.  Its compare is against decimal 48
    // after subtracting one; the earlier 0x40 ceiling confused that decimal
    // bound with a hexadecimal command value.  Anything else is malformed
    // and refused rather than answered, the way disk refuses an undecodable
    // command.
    if (command < NuTaIob::kCommandMin || command > NuTaIob::kCommandMax) {
        trace_.diskIo("  command {:02X} is outside NuTapeIobCheck's accepted range {:02X}..{:02X} - refused (the A/36 "
                      "returns its error code 9 here). docs/s36/tape-svc-integration.md",
                      command, NuTaIob::kCommandMin, NuTaIob::kCommandMax);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    switch (command) {
        case NuTaIob::kCommandActivate: return activate(iob, modifier);
        case NuTaIob::kCommandSetSession: return setSession(iob, modifier);
        case NuTaIob::kCommandInitializeStandard: return initializeStandard(iob, modifier, length, bufferField);
        case NuTaIob::kCommandReadVolumeLabels: return readVolumeLabels(iob, modifier, length, bufferField);
        case NuTaIob::kCommandWriteHeaderLabels: return writeHeaderLabels(iob, modifier, length, bufferField);
        case NuTaIob::kCommandFindDataSet: return findDataSet(iob, modifier, length, bufferField);
        case NuTaIob::kCommandReadData:
        case NuTaIob::kCommandReadDataAlt: return read(iob, command, length, bufferField);
        case NuTaIob::kCommandWriteData:
        case NuTaIob::kCommandWriteDataAlt: return write(iob, command, length, bufferField);
        case NuTaIob::kCommandFinishDataSet: return finishDataSet(iob, modifier, length);
        case NuTaIob::kCommandFinalizeVolume: return finalizeVolume(iob, modifier, length);
        case NuTaIob::kCommandUnload: return unloadCommand(iob, modifier, length);
        case NuTaIob::kCommandControl: return control(iob, modifier);
        default:
            // A command accepted as well-formed but whose operation this
            // model has not pinned from NuTapeIo.  Refused and counted loudly.
            unmappedCommands_++;
            trace_.diskIo("  command {:02X} is a valid tape IOB command but its operation is NOT MAPPED in this model - "
                          "its NuTapeIo arm has not yet been established. Refused rather than guessed. "
                          "Request {} of its kind. docs/s36/tape-svc-integration.md",
                          command, unmappedCommands_);
            postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
            return false;
    }
}

bool VirtualTape::activate(int iob, int modifier)
{
    // Commands below 0x10 bypass NuTapeIo's jump table.  Command 01 runs the
    // tapeRemoved/readyTape activation path before completing.  FROMLIBR's
    // observed request is modifier 00; other variants remain unsupported.
    if (modifier != 0) {
        trace_.diskIo("  command 01 modifier {:02X} has not been established - refused", modifier);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    trace_.diskIo("  command 01/00 activates the already-loaded tape session (V4R4 NuTapeIo c23e21e8)");
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::setSession(int iob, int modifier)
{
    // V4R4 NuTapeIo c23e33fc accepts 00, 01 and 03.  The native FROMLIBR
    // path uses 03 and calls readiness/session handling, not a data mover.
    if (modifier != 0 && modifier != 1 && modifier != 3) {
        trace_.diskIo("  command 02 modifier {:02X} is not one of 00, 01, 03 - NuTapeIo error arm", modifier);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    trace_.diskIo("  command 02/{:02X} establishes the loaded tape session without moving media", modifier);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::initializeStandard(int iob, int modifier, int length, int bufferField)
{
    // TAPEINIT's observed 12/00 request reaches the shared 11/12 arm at
    // V4R4 c23e3a30.  That arm requires an 80-byte buffer, copies the label,
    // writes it at load point, writes two filemarks through vtable 0x188,
    // and rewinds through vtable 0x190.  Only command 12/00 is observed and
    // exposed here; command 11 remains refused.
    if (modifier != 0 || length != 80) {
        trace_.diskIo("  command 12 requires modifier 00 and one 80-byte VOL1; got {:02X}/{}", modifier, length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    std::vector<uint8_t> label(80);
    if (!m_.readGuest24Range(bufferField, label.data(), static_cast<int>(label.size()))) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    static constexpr uint8_t vol1[] = {0xE5, 0xD6, 0xD3, 0xF1};
    if (!std::equal(std::begin(vol1), std::end(vol1), label.begin())) {
        trace_.diskIo("  command 12 buffer is not an EBCDIC VOL1 label - refused");
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    medium_->rewind();
    TapeResult r = medium_->writeBlock(label.data(), 0, static_cast<int>(label.size()));
    if (r == TapeResult::NotReady) return notReady(iob);
    if (r == TapeResult::WriteProtected) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicWriteProtected);
        return false;
    }
    if (r != TapeResult::Ok || medium_->writeTapeMark() != TapeResult::Ok ||
        medium_->writeTapeMark() != TapeResult::Ok) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    medium_->rewind();
    writesIssued_++;
    controlOps_++;
    trace_.diskIo("  command 12 wrote VOL1 and two filemarks, then rewound; {}", medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::readVolumeLabels(int iob, int modifier, int length, int bufferField)
{
    // V4R4 command 13 is arm c23e359c.  It requires 0x370 bytes, runs the
    // rewind driver method (vtable 0x190), then tapRdLbls.  The SSP request
    // uses modifier 03.  Only the verified 80-byte VOL1 transfer is exposed;
    // the remainder of the 0x370-byte work area is left untouched.
    if (modifier != 3 || length != 0x370) {
        trace_.diskIo("  command 13 requires modifier 03 and length 880; got {:02X}/{}", modifier, length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    std::vector<std::pair<int, int>> extents;
    if (!m_.guest24Extents(bufferField, 80, true, extents)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    medium_->rewind();
    std::vector<uint8_t> label;
    TapeResult r = medium_->readBlock(label);
    if (r != TapeResult::Ok || label.size() != 80 || label[0] != 0xE5 || label[1] != 0xD6 || label[2] != 0xD3 ||
        label[3] != 0xF1) {
        trace_.diskIo("  command 13 did not find an 80-byte EBCDIC VOL1 at load point ({})", storage::tapeResultName(r));
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicTapeMark);
        return false;
    }
    if (!m_.writeGuest24Range(bufferField, label.data(), 80)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    lastRead_ = label;
    hasLastRead_ = true;
    readsIssued_++;
    trace_.diskIo("  command 13 rewound and returned the 80-byte VOL1 label; {}", medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::writeHeaderLabels(int iob, int modifier, int length, int bufferField)
{
    // The native FROMLIBR create path reaches command 14 after tapFind has
    // stopped beyond the two terminal marks.  Its V4R4 jump-table arm at
    // c23e2ad0 validates the label count and calls tapWrLbls1, which writes
    // the individual labels and the closing filemark.  The observed request
    // is four standard 80-byte labels.  Expose precisely that form; other
    // label counts and modifiers remain unsupported until established.
    if (modifier != 3 || length != 320) {
        trace_.diskIo("  command 14 requires modifier 03 and four 80-byte labels; got {:02X}/{}", modifier, length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    if (medium_->readOnly()) {
        trace_.diskIo("  command 14 REFUSED - {} is write-protected", medium_->path());
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicWriteProtected);
        return false;
    }

    std::vector<uint8_t> labels(static_cast<std::size_t>(length));
    if (!m_.readGuest24Range(bufferField, labels.data(), length)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    static constexpr uint8_t ids[4][4] = {
        {0xC8, 0xC4, 0xD9, 0xF1}, // HDR1
        {0xC8, 0xC4, 0xD9, 0xF2}, // HDR2
        {0xE4, 0xC8, 0xD3, 0xF1}, // UHL1
        {0xE4, 0xC8, 0xD3, 0xF2}, // UHL2
    };
    for (int record = 0; record < 4; record++) {
        if (!std::equal(std::begin(ids[record]), std::end(ids[record]), labels.begin() + record * 80)) {
            trace_.diskIo("  command 14 record {} is not the expected EBCDIC standard label - refused", record + 1);
            postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
            return false;
        }
    }

    int spaced = 0;
    TapeResult r = medium_->spaceFiles(-1, spaced);
    if ((r != TapeResult::Ok && r != TapeResult::TapeMark) || spaced != 1) {
        trace_.diskIo("  command 14 could not back over the terminal filemark: {} after {} mark(s)",
                      storage::tapeResultName(r), spaced);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    for (int record = 0; record < 4; record++) {
        r = medium_->writeBlock(labels.data(), record * 80, 80);
        if (r != TapeResult::Ok) {
            trace_.diskIo("  command 14 label write {} stopped on {}", record + 1, storage::tapeResultName(r));
            postError(iob, NuTaIob::kCompletionError,
                      r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
            return false;
        }
        writesIssued_++;
    }
    r = medium_->writeTapeMark();
    if (r != TapeResult::Ok) {
        postError(iob, NuTaIob::kCompletionError,
                  r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
        return false;
    }
    activeHeaderLabels_ = labels;
    controlOps_++;
    trace_.diskIo("  command 14 wrote HDR1/HDR2/UHL1/UHL2 and the closing filemark; {}",
                  medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::finishDataSet(int iob, int modifier, int length)
{
    // Command 19 dispatches to the V4R4 c23e2784 arm.  In the observed
    // FROMLIBR write sequence it follows the last command-21 data block with
    // modifier 00 and a 512-byte work-buffer length.  The arm writes a mark,
    // derives the trailer group from the labels retained by command 14,
    // writes those 80-byte records through tapWrtLbls, and writes a closing
    // mark.  Preserve the header fields while changing the standard label
    // identifiers HDR->EOF and UHL->UTL.
    if (modifier != 0 || length != 512 || activeHeaderLabels_.size() != 320) {
        trace_.diskIo("  command 19 requires an active command-14 label group and request 00/512; got {:02X}/{}", modifier,
                      length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    if (medium_->readOnly()) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicWriteProtected);
        return false;
    }

    TapeResult r = medium_->writeTapeMark();
    if (r != TapeResult::Ok) {
        postError(iob, NuTaIob::kCompletionError,
                  r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
        return false;
    }
    std::vector<uint8_t> trailers = activeHeaderLabels_;
    static constexpr uint8_t eof[] = {0xC5, 0xD6, 0xC6}; // EOF
    static constexpr uint8_t utl[] = {0xE4, 0xE3, 0xD3}; // UTL
    std::copy(std::begin(eof), std::end(eof), trailers.begin());
    std::copy(std::begin(eof), std::end(eof), trailers.begin() + 80);
    std::copy(std::begin(utl), std::end(utl), trailers.begin() + 160);
    std::copy(std::begin(utl), std::end(utl), trailers.begin() + 240);
    for (int record = 0; record < 4; record++) {
        r = medium_->writeBlock(trailers.data(), record * 80, 80);
        if (r != TapeResult::Ok) {
            postError(iob, NuTaIob::kCompletionError,
                      r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
            return false;
        }
        writesIssued_++;
    }
    r = medium_->writeTapeMark();
    if (r != TapeResult::Ok) {
        postError(iob, NuTaIob::kCompletionError,
                  r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
        return false;
    }
    controlOps_ += 2;
    trace_.diskIo("  command 19 closed the data file and wrote EOF1/EOF2/UTL1/UTL2 plus a closing mark; {}",
                  medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::finalizeVolume(int iob, int modifier, int length)
{
    // The observed FROMLIBR export follows command 19 with 1B/00 and the
    // same 512-byte work-area length.  The local command jump table selects
    // c23e3cb4; its first media operation is the proxy-vtable tape-mark
    // method.  This supplies the second consecutive mark required at logical
    // end of tape, after which the arm only finalizes the driver/session.
    if (modifier != 0 || length != 512 || activeHeaderLabels_.size() != 320) {
        trace_.diskIo("  command 1B requires an active completed data set and request 00/512; got {:02X}/{}", modifier,
                      length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    TapeResult r = medium_->writeTapeMark();
    if (r != TapeResult::Ok) {
        postError(iob, NuTaIob::kCompletionError,
                  r == TapeResult::WriteProtected ? NuTaIob::kMicWriteProtected : NuTaIob::kMicInvalidCommand);
        return false;
    }
    activeHeaderLabels_.clear();
    controlOps_++;
    trace_.diskIo("  command 1B wrote the second terminal mark and finalized the tape session; {}",
                  medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::unloadCommand(int iob, int modifier, int length)
{
    // After finalizing the exported volume, FROMLIBR reactivates the session
    // and issues 27/00 with its 512-byte work area and a null data pointer.
    // The V4R4 jump table maps command 27 to c23e3964; its accepted branch
    // calls driver-vtable slot 0x198, the IoTapeProxy unload operation.
    // FROMLIBR supplies its 512-byte work area; BLDLIBR supplies the
    // 4096-byte input buffer.  Neither is transferred by the unload arm.
    if (modifier != 0 || (length != 512 && length != 4096)) {
        trace_.diskIo("  command 27 requires an observed unload request 00/512 or 00/4096; got {:02X}/{}", modifier,
                      length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    const std::string path = medium_->path();
    medium_->unload();
    activeHeaderLabels_.clear();
    controlOps_++;
    trace_.diskIo("  command 27 unloaded and flushed {}; cartridge remains present but not ready", path);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

bool VirtualTape::findDataSet(int iob, int modifier, int length, int bufferField)
{
    // V4R4 NuTapeIo::tapFind (c23e4d60) accepts the FROMLIBR 16/03 request
    // with a 0x1e0-byte work area.  It copies 17 bytes from the beginning of
    // that area, rewinds, reads 80-byte label records, compares that key with
    // HDR1+4, and spaces over label files until it finds the requested data
    // set.  On success tapRdLbls consumes the remaining header labels and the
    // closing mark, leaving the drive at the first data block.  tapLbls2
    // stores the accumulated byte count at IOB+0x12 and moves the contiguous
    // HDR1-onward label group into the guest work area.
    if (modifier != 3 || length != 0x1E0) {
        trace_.diskIo("  command 16 requires modifier 03 and length 480; got {:02X}/{}", modifier, length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }

    std::vector<uint8_t> wanted(17);
    if (!m_.readGuest24Range(bufferField, wanted.data(), static_cast<int>(wanted.size()))) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    static constexpr uint8_t hdr1[] = {0xC8, 0xC4, 0xD9, 0xF1};
    medium_->rewind();
    bool matched = false;
    int records = 0;
    std::vector<uint8_t> labels;
    for (;;) {
        std::vector<uint8_t> block;
        TapeResult r = medium_->readBlock(block);
        if (r == TapeResult::Ok) {
            records++;
            readsIssued_++;
            lastRead_ = block;
            hasLastRead_ = true;
            if (!matched && block.size() == 80 &&
                std::equal(std::begin(hdr1), std::end(hdr1), block.begin()) &&
                std::equal(wanted.begin(), wanted.end(), block.begin() + 4)) {
                matched = true;
            }
            if (matched) labels.insert(labels.end(), block.begin(), block.end());
            continue;
        }
        if (r == TapeResult::TapeMark) {
            if (matched) {
                if (labels.size() > static_cast<std::size_t>(length) ||
                    !m_.writeGuest24Range(bufferField, labels.data(), static_cast<int>(labels.size()))) {
                    postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
                    return false;
                }
                m_.writeHalf(iob + NuTaIob::kOffReturnedLength, static_cast<uint16_t>(labels.size()));
                activeHeaderLabels_ = labels;
                trace_.diskIo("  command 16 found the requested 17-byte HDR1 identifier after {} label record(s); {}",
                              records, medium_->readPosition().toString());
                IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
                return true;
            }
            continue;
        }
        if (r == TapeResult::NotReady) return notReady(iob);

        trace_.diskIo("  command 16 did not find the requested HDR1 identifier: {} after {} record(s); {}",
                      storage::tapeResultName(r), records, medium_->readPosition().toString());
        // NuTapeIo::tapFind returns internal condition 0x001b.  The decoded
        // NuTapeMicSrcGenS routine maps it through the local SLIC table entry
        // 0x6236 and generates the complete guest status below.  A real
        // #CATP/FROMLIBR run accepts it and proceeds to command 0x14.
        m_.writeHalf(iob + NuTaIob::kOffMicSource, 0x7462);
        m_.writeHalf(iob + NuTaIob::kOffMic, 0x1B36);
        IoBlock::complete(m_, iob, NuTaIob::kCompletionEndOfFile);
        return true;
    }
}

bool VirtualTape::finishReadDataSet(int iob)
{
    // Command 22's c23e23ec arm maps the driver's filemark condition 1C to
    // tapRdLbls and then tapEofHan.  The latter compares the trailer group
    // with the header group saved by tapFind and, for command 22, writes
    // completion nibble 2 at IOB+06.  A native BLDLIBR trace establishes
    // that the filemark preceding EOF1 is already consumed when this path
    // begins.  Consume the four 80-byte trailers and their closing mark so
    // the next tape operation starts at the following file.
    if (activeHeaderLabels_.size() != 320) {
        trace_.diskIo("  command 22 reached a data filemark without the four labels retained by command 16 - refused");
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    std::vector<uint8_t> expected = activeHeaderLabels_;
    static constexpr uint8_t eof[] = {0xC5, 0xD6, 0xC6}; // EOF
    static constexpr uint8_t utl[] = {0xE4, 0xE3, 0xD3}; // UTL
    std::copy(std::begin(eof), std::end(eof), expected.begin());
    std::copy(std::begin(eof), std::end(eof), expected.begin() + 80);
    std::copy(std::begin(utl), std::end(utl), expected.begin() + 160);
    std::copy(std::begin(utl), std::end(utl), expected.begin() + 240);

    for (int record = 0; record < 4; record++) {
        std::vector<uint8_t> actual;
        TapeResult r = medium_->readBlock(actual);
        if (r == TapeResult::NotReady) return notReady(iob);
        if (r != TapeResult::Ok || actual.size() != 80 ||
            !std::equal(actual.begin(), actual.end(), expected.begin() + record * 80)) {
            trace_.diskIo("  command 22 trailer record {} did not match its retained header: {} / {} byte(s)",
                          record + 1, storage::tapeResultName(r), actual.size());
            postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
            return false;
        }
        readsIssued_++;
        lastRead_ = actual;
        hasLastRead_ = true;
    }
    std::vector<uint8_t> ignored;
    TapeResult r = medium_->readBlock(ignored);
    if (r != TapeResult::TapeMark) {
        trace_.diskIo("  command 22 trailer-label file lacks its closing mark: {}", storage::tapeResultName(r));
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    m_.writeHalf(iob + NuTaIob::kOffReturnedLength, 0);
    activeHeaderLabels_.clear();
    controlOps_++;
    trace_.diskIo("  command 22 consumed matching EOF1/EOF2/UTL1/UTL2 and their closing mark; completion {:02X}; {}",
                  Ecm::kComplete | NuTaIob::kCompletionEndOfDataSet, medium_->readPosition().toString());
    IoBlock::complete(m_, iob, NuTaIob::kCompletionEndOfDataSet);
    return true;
}

// Read data, commands 0x17 and 0x22: the internal tape buffer is copied
// INTO the guest data buffer, a direction that is VERIFIED.  Command 22 also
// reports its transferred length and handles a standard labeled data-file
// boundary through finishReadDataSet(); command 17 has neither contract.
bool VirtualTape::read(int iob, int command, int length, int bufferField)
{
    if (!validLength(iob, length)) return false;

    std::vector<uint8_t> block;
    TapeResult r = medium_->readBlock(block);
    if (r == TapeResult::NotReady) return notReady(iob);

    if (r == TapeResult::TapeMark && command == NuTaIob::kCommandReadDataAlt)
        return finishReadDataSet(iob);

    if (r != TapeResult::Ok) {
        // A tape mark or end of data: a normal condition the guest MUST be
        // able to tell apart from a data block; posting success would hand
        // it stale buffer bytes as a record.  The exact completion nibble
        // and MIC for each is INFERRED, so a single non-success class stands
        // in and says so.
        trace_.diskIo("  read stopped on {} - completion {:02X}, MIC {:04X} (INFERRED class; the per-condition tape code is "
                      "unconfirmed). docs/s36/tape-svc-integration.md",
                      storage::tapeResultName(r), Ecm::kComplete | NuTaIob::kCompletionEndOfFile, NuTaIob::kMicTapeMark);
        postError(iob, NuTaIob::kCompletionEndOfFile, NuTaIob::kMicTapeMark);
        return true;
    }

    if (block.size() > static_cast<std::size_t>(length)) {
        // readBlock() advances on success.  No established native behaviour
        // permits a successful truncated record, so put it back and refuse
        // without touching guest data.
        int spaced = 0;
        TapeResult back = medium_->spaceRecords(-1, spaced);
        trace_.diskIo("  {}-byte tape block exceeds the {}-byte guest buffer; no bytes transferred, rollback {} after {} "
                      "record(s). docs/s36/tape-svc-integration.md",
                      block.size(), length, storage::tapeResultName(back), spaced);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }

    int n = static_cast<int>(block.size());
    std::vector<std::pair<int, int>> extents;
    if (!m_.guest24Extents(bufferField, n, true, extents)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    if (!m_.writeGuest24Range(bufferField, block.data(), n)) {
        trace_.diskIo("  buffer {:06X} + {} is not fully mapped", bufferField, n);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }

    const int dst = extents.empty() ? 0 : extents.front().first;
    lastRead_ = block;
    hasLastRead_ = true;
    readsIssued_++;

    // Command 22's c23e23ec arm writes the requested length here for a full
    // block, or requested length minus the driver's residual for a short
    // block.  $MAINT consumes this count; leaving the cleared field at zero
    // makes a successful transfer look empty.  Command 17 has a different
    // arm and its corresponding output contract remains unestablished.
    if (command == NuTaIob::kCommandReadDataAlt)
        m_.writeHalf(iob + NuTaIob::kOffReturnedLength, static_cast<uint16_t>(n));

    // iob+0x14/0x16 are present (bounded by the block length) but their
    // exact role (residual, bytes moved, current offset) is INFERRED, and
    // nothing decoded shows them handed back to this caller, so they are NOT
    // written.
    trace_.diskIo("  read {} byte(s) of a {}-byte block to guest {:06X} (cmd {:02X}), completion {:02X}", n, block.size(),
                  dst, command, Ecm::kComplete | 0);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

// Write data, commands 0x18 and 0x21: the guest data buffer is copied INTO
// the internal tape buffer, a direction that is VERIFIED.  0x18 versus 0x21
// is not distinguished; both map to a block write.
bool VirtualTape::write(int iob, int command, int length, int bufferField)
{
    if (!validLength(iob, length)) return false;

    std::vector<std::pair<int, int>> extents;
    if (!m_.guest24Extents(bufferField, length, false, extents)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    std::vector<uint8_t> data(static_cast<std::size_t>(length));
    if (!m_.readGuest24Range(bufferField, data.data(), length)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }
    const int src = extents.empty() ? 0 : extents.front().first;
    TapeResult r = medium_->writeBlock(data.data(), 0, length);

    if (r == TapeResult::NotReady) return notReady(iob);
    if (r == TapeResult::WriteProtected) {
        trace_.diskIo("  REFUSED - {} is write-protected. completion {:02X} (INFERRED MIC). docs/s36/tape-svc-integration.md",
                      medium_->path(), Ecm::kComplete | NuTaIob::kCompletionError);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicWriteProtected);
        return false;
    }

    writesIssued_++;
    trace_.diskIo("  wrote {} byte(s) from guest {:06X} to {} (cmd {:02X}), completion {:02X}", length, src, medium_->path(),
                  command, Ecm::kComplete | 0);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

// Control, command 0x10 with modifier 0x10: the completion byte is cleared
// and the call returns without moving data.  Its precise semantics are
// INFERRED, but the decoded behaviour (accept and post complete, transfer
// nothing) is what this reproduces.  Any other modifier is refused.
bool VirtualTape::control(int iob, int modifier)
{
    controlOps_++;
    if (modifier != NuTaIob::kControlModifier) {
        trace_.diskIo("  control command 10 with modifier {:02X}: NuTapeIos requires modifier 10 (both bytes 0x10) and sends "
                      "anything else to its error arm - refused. docs/s36/tape-svc-integration.md",
                      modifier);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    trace_.diskIo("  control command 10/10 -> NuTapeIos clears completion and returns, no transfer (exact semantics "
                  "INFERRED). Request {} of its kind.",
                  controlOps_);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionOk);
    return true;
}

// The length gate for the four data commands: the block length at iob+0x10
// must be 1..0x7FFF.
bool VirtualTape::validLength(int iob, int length)
{
    if (length >= 1 && length <= NuTaIob::kMaxBlockLength) return true;
    trace_.diskIo("  block length {} at iob+0x10 is not 1..{} - NuTapeIobCheck rejects this. docs/s36/tape-svc-integration.md",
                  length, NuTaIob::kMaxBlockLength);
    postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
    return false;
}

bool VirtualTape::notReady(int iob)
{
    m_.writeByte(iob + kOffDeviceStatus, kNotInThisConfiguration);
    IoBlock::complete(m_, iob, NuTaIob::kCompletionError);
    trace_.diskIo("  drive reports not ready (the backend spun down mid-request)");
    return true;
}

// Post a non-success completion: the low nibble goes to the ECM at iob+0x06
// (with the 0x40 complete bit, as every device does) and the MIC halfword
// to iob+0x1E.
void VirtualTape::postError(int iob, int completion, int mic)
{
    m_.writeHalf(iob + NuTaIob::kOffMic, static_cast<uint16_t>(mic));
    IoBlock::complete(m_, iob, completion);
}

}  // namespace sim36::devices
