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
        case kCommandReadData: return "read data";
        case kCommandReadDataAlt: return "read data (alt)";
        case kCommandWriteData: return "write data";
        case kCommandWriteDataAlt: return "write data (alt)";
        case kCommandControl: return "control";
        default: return "unmapped";
    }
}

void VirtualTape::load(std::unique_ptr<storage::ITapeBackend> medium)
{
    unload();
    medium_ = std::move(medium);
    if (medium_) medium_->load();
}

bool VirtualTape::unload()
{
    if (!medium_) return false;
    medium_->unload();
    medium_.reset();
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

    // The command must be in 1..0x40; anything else is a malformed IOB,
    // refused rather than answered, the way the disk refuses a command it
    // cannot decode.
    if (command < NuTaIob::kCommandMin || command > NuTaIob::kCommandMax) {
        trace_.diskIo("  command {:02X} is outside NuTapeIobCheck's accepted range {:02X}..{:02X} - refused (the A/36 "
                      "returns its error code 9 here). docs/s36/tape-svc-integration.md",
                      command, NuTaIob::kCommandMin, NuTaIob::kCommandMax);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }

    switch (command) {
        case NuTaIob::kCommandReadData:
        case NuTaIob::kCommandReadDataAlt: return read(iob, command, length, bufferField);
        case NuTaIob::kCommandWriteData:
        case NuTaIob::kCommandWriteDataAlt: return write(iob, command, length, bufferField);
        case NuTaIob::kCommandControl: return control(iob, modifier);
        default:
            // A command accepted as well-formed but whose tape operation this
            // model has NOT pinned: the positioning opcodes are not dispatched
            // from iob+0x0A in the layer that was read, so mapping one here
            // would be a guess.  Refused and counted, loudly.
            unmappedCommands_++;
            trace_.diskIo("  command {:02X} is a valid tape IOB command but its operation is NOT MAPPED in this model - "
                          "the A/36 SLIC tape layer read here dispatches only data read (17/22), data write (18/21) and "
                          "control (10) from iob+0x0A; positioning is the label layer's. Refused rather than guessed. "
                          "Request {} of its kind. docs/s36/tape-svc-integration.md",
                          command, unmappedCommands_);
            postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
            return false;
    }
}

// Read data, commands 0x17 and 0x22: the internal tape buffer is copied
// INTO the guest data buffer, a direction that is VERIFIED.  What
// distinguishes 0x17 from 0x22 is not recovered, so they map identically.
bool VirtualTape::read(int iob, int command, int length, int bufferField)
{
    if (!validLength(iob, length)) return false;

    std::vector<uint8_t> block;
    TapeResult r = medium_->readBlock(block);
    if (r == TapeResult::NotReady) return notReady(iob);

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

    int n = std::min(static_cast<int>(block.size()), length);
    int dst;
    if (!resolveBuffer(bufferField, dst)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    if (dst + n > m_.backingBytes()) {
        trace_.diskIo("  buffer {:06X} + {} outside main storage", dst, n);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }

    m_.write(dst, block.data(), n);
    lastRead_ = block;
    hasLastRead_ = true;
    readsIssued_++;

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

    int src;
    if (!resolveBuffer(bufferField, src)) {
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicInvalidCommand);
        return false;
    }
    if (src + length > m_.backingBytes()) {
        trace_.diskIo("  buffer {:06X} + {} outside main storage", src, length);
        postError(iob, NuTaIob::kCompletionError, NuTaIob::kMicLength);
        return false;
    }

    std::vector<uint8_t> data(static_cast<std::size_t>(length));
    m_.read(src, data.data(), length);
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

// Same field and same flag as every device SVC: iob+0x0D..0x0F, honouring
// the 0x800000 task-translated bit.
bool VirtualTape::resolveBuffer(int bufferField, int& addr)
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
