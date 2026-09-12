#include "Devices/VirtualWorkstation.h"

#include <algorithm>

#include <fmt/format.h>

#include "Monitor/CommandRegistry.h"

namespace sim36::devices {

const char* workstationOutputModeName(WorkstationOutputMode mode)
{
    switch (mode) {
        case WorkstationOutputMode::PassThrough: return "PassThrough";
        case WorkstationOutputMode::WtdText: return "WtdText";
        case WorkstationOutputMode::SlicDisplay: return "SlicDisplay";
    }
    return "?";
}

const char* putWithInviteReadModeName(PutWithInviteReadMode mode)
{
    switch (mode) {
        case PutWithInviteReadMode::ReadInputFields20: return "ReadInputFields20";
        case PutWithInviteReadMode::StructuredField21: return "StructuredField21";
    }
    return "?";
}

VirtualWorkstation::VirtualWorkstation(const configuration::StationConfig& cfg, host::WorkstationBackend& backend,
                                       monitor::Tracer& trace, bool ownsBackend)
    : cfg_(cfg), backend_(&backend), trace_(trace), ownsBackend_(ownsBackend), deviceDisplay_(0, false)
{
}

VirtualWorkstation::~VirtualWorkstation()
{
    if (ownsBackend_) delete backend_;
}

// Record byte 1's 0x30 bits, as the device's printer test reads them.
bool VirtualWorkstation::isPrinter() const { return monitor::equalsIgnoreCase(cfg_.role, "printer"); }

// The output adapter is runtime monitor state, not a process option.
// Pass-through is the ordinary PUT byte flow; the other modes remain
// monitor-selected experiments.
void VirtualWorkstation::setOutputMode(WorkstationOutputMode mode)
{
    outputMode_ = mode;
    trace_.ws("station {}: output mode {}", id(), workstationOutputModeName(mode));
}

void VirtualWorkstation::setInviteReadMode(PutWithInviteReadMode mode)
{
    inviteReadMode_ = mode;
    trace_.ws("station {}: PUT-with-invite read mode {}", id(), putWithInviteReadModeName(mode));
}

bool VirtualWorkstation::copyRetainedDeviceInput(std::vector<uint8_t>& copy) const
{
    if (!hasRetainedInput_) return false;
    copy = retainedDeviceInput_;
    return true;
}

bool VirtualWorkstation::attached() const { return backend_->attached(); }
bool VirtualWorkstation::ready() const { return backend_->ready(); }
bool VirtualWorkstation::takeAttentionPending() { return backend_->takeAttentionPending(); }
bool VirtualWorkstation::attentionPending() const { return backend_->attentionPending(); }
bool VirtualWorkstation::takePowerOffPending() { return backend_->takePowerOffPending(); }
bool VirtualWorkstation::powerOffPending() const { return backend_->powerOffPending(); }

void VirtualWorkstation::powerOff()
{
    inviteOutstanding_ = false;
    activeReadMode_ = 0;
    hasRetainedInput_ = false;
    retainedDeviceInput_.clear();
    retainedReadMode_ = 0;
    backend_->clearForPowerOff();
    trace_.ws("station {}: powered off (client gone); invite, read mode and retained input discarded", id());
}

int VirtualWorkstation::listenPort() const { return backend_->port(); }
void VirtualWorkstation::start() { backend_->listen(); }

// Command hex 21, Output Data: the bytes are forwarded.  The System/36 has
// no RFC 1205 opcode to give, so one is chosen here (compatibility policy):
// a station with an invite outstanding gets Put/Get, which RFC 1205 section
// 3 defines as write-then-read, and one without gets Output Only.
bool VirtualWorkstation::outputData(const uint8_t* dataStream, int offset, int length)
{
    return outputOperation(dataStream, offset, length, false);
}

// PUT-with-invite: raw/rendered output and the read command form one
// display body and one RFC 1205 Put/Get record.
bool VirtualWorkstation::outputDataWithInvite(const uint8_t* dataStream, int offset, int length)
{
    return outputOperation(dataStream, offset, length, true);
}

bool VirtualWorkstation::outputOperation(const uint8_t* dataStream, int offset, int length, bool withInvite)
{
    outputDataStreams_++;
    outputDataBytes_ += length;
    lastOutputDataStream_.assign(dataStream + offset, dataStream + offset + length);
    hasLastOutput_ = true;

    // SC30-3533: D9/34 enters WP mode. In that mode the matching input
    // operation is D9/32 READ TEXT SCREEN, not READ MDT/READ INPUT FIELDS.
    // WRITE TO DISPLAY is DP-only and therefore marks the inverse mode
    // transition. The structured fields themselves remain terminal-owned
    // and cross this seam byte-for-byte.
    if (containsTextAssistFormat(dataStream, offset, length)) {
        inviteReadMode_ = PutWithInviteReadMode::StructuredField21;
        trace_.ws("station {}: D934 DEFINE TEXT SCREEN FORMAT selected Controller Text Assist read mode D932", id());
    } else if (containsWriteToDisplay(dataStream, offset, length)) {
        inviteReadMode_ = PutWithInviteReadMode::ReadInputFields20;
        trace_.ws("station {}: DP-mode WRITE TO DISPLAY selected ordinary read mode 0x20", id());
    }

    std::vector<uint8_t> wrapped;
    const uint8_t* wire = dataStream;
    int wireOffset = offset, wireLength = length;
    if (outputMode_ == WorkstationOutputMode::SlicDisplay) {
        wrapped = wrapAsSlicDisplayWriteOnlyMessage(dataStream, offset, length);
        wire = wrapped.data();
        wireOffset = 0;
        wireLength = static_cast<int>(wrapped.size());
        trace_.ws("station {}: experimental SLIC Display::writeOnlyMessage rendered {} WSDM byte(s) as {} 5250 byte(s)",
                  id(), length, wireLength);
    } else if (outputMode_ == WorkstationOutputMode::WtdText && !looksLike5250DataStream(dataStream, offset, length)) {
        wrapped = wrapWsdmTextAs5250(dataStream, offset, length);
        wire = wrapped.data();
        wireOffset = 0;
        wireLength = static_cast<int>(wrapped.size());
        trace_.ws("station {}: WSDM text compatibility adapter wrapped {} raw byte(s) as a {}-byte 5250 Write-To-Display",
                  id(), length, wireLength);
    }

    std::vector<uint8_t> atomic;
    if (withInvite) {
        const bool structured = inviteReadMode_ == PutWithInviteReadMode::StructuredField21;
        const std::vector<uint8_t> suffix = structured
                                                ? std::vector<uint8_t>{0x04, 0xF3, 0x00, 0x08, 0xD9, 0x32, 0x00, 0x80, 0x00, 0x00}
                                                : std::vector<uint8_t>{0x04, 0x52, 0x00, 0x00};
        atomic.assign(wire + wireOffset, wire + wireOffset + wireLength);
        atomic.insert(atomic.end(), suffix.begin(), suffix.end());
        wire = atomic.data();
        wireOffset = 0;
        wireLength = static_cast<int>(atomic.size());
        inviteOutstanding_ = true;
        activeReadMode_ = structured ? static_cast<uint8_t>(0x21) : static_cast<uint8_t>(0x20);
        trace_.ws("station {}: appended recovered operation {} read command ({} byte(s))", id(),
                  structured ? "0x21" : "0x20", suffix.size());
    }

    deviceDisplay_.apply(wire, wireOffset, wireLength);

    trace_.ws("station {}: {}, {} source byte(s){}", id(), withInvite ? "atomic put with invite" : "put", length,
              wireLength == length ? std::string() : fmt::format(" -> {} wire byte(s)", wireLength));
    return backend_->sendPut(wire, wireOffset, wireLength, withInvite);
}

// The exact construction of the display's write-only-message renderer for
// its observed ordinary-write arguments 00,04:
//   04 11 00 10 11 00 04 22 [payload] 20
// 04 11 is Write To Display, 11 is Set Buffer Address, and 22/20 are 5250
// display attribute bytes (white and green).  Selecting this mode is a
// negative control rather than a claim about the default contract.
std::vector<uint8_t> VirtualWorkstation::wrapAsSlicDisplayWriteOnlyMessage(const uint8_t* data, int offset, int length)
{
    std::vector<uint8_t> stream(static_cast<std::size_t>(length) + 9, 0);
    const uint8_t prefix[] = {0x04, 0x11, 0x00, 0x10, 0x11, 0x00, 0x04, 0x22};
    std::copy(prefix, prefix + 8, stream.begin());
    std::copy(data + offset, data + offset + length, stream.begin() + 8);
    stream[stream.size() - 1] = 0x20;
    return stream;
}

// Conservative recognition: the guest's normal 5250 stream starts with ESC
// Clear Unit or ESC Write To Display.
bool VirtualWorkstation::looksLike5250DataStream(const uint8_t* data, int offset, int length)
{
    return length >= 2 && data[offset] == 0x04 &&
           (data[offset + 1] == 0x40 || data[offset + 1] == 0x11 || data[offset + 1] == 0xF3);
}

bool VirtualWorkstation::containsTextAssistFormat(const uint8_t* data, int offset, int length)
{
    const int end = offset + length;
    for (int i = offset; i + 1 < end; i++) {
        if (data[i] != 0x04 || data[i + 1] != 0xF3) continue;
        // WSF consists of consecutive LL/C/T fields. Respect LL rather than
        // byte-scanning the table/text payload for a coincidental D934.
        for (int p = i + 2; p + 4 <= end;) {
            const int fieldLength = (data[p] << 8) | data[p + 1];
            if (fieldLength < 4 || p + fieldLength > end) break;
            if (data[p + 2] == 0xD9 && data[p + 3] == 0x34) return true;
            p += fieldLength;
        }
        return false;
    }
    return false;
}

bool VirtualWorkstation::containsWriteToDisplay(const uint8_t* data, int offset, int length)
{
    const int end = offset + length;
    for (int i = offset; i + 1 < end; i++) {
        if (data[i] != 0x04) continue;
        if (data[i + 1] == 0x11) return true;
        // Everything after WSF is length-delimited structured-field data,
        // not another top-level command to byte-scan.
        if (data[i + 1] == 0xF3) return false;
    }
    return false;
}

// Standalone compatibility adapter for the raw fixed-width character
// buffers observed at the WSDM seam.  It does not invent fields,
// attributes, AID handling, or sign-on content: bytes are laid out in
// 80-column rows using SBA orders, and bytes below EBCDIC space are treated
// as blanks so raw control/NUL bytes cannot become accidental 5250 orders.
std::vector<uint8_t> VirtualWorkstation::wrapWsdmTextAs5250(const uint8_t* data, int offset, int length)
{
    constexpr int kColumns = 80, kRows = 24;
    int count = std::min(length, kColumns * kRows);
    std::vector<uint8_t> stream;
    stream.reserve(static_cast<std::size_t>(count) + 4 + kRows * 3);
    stream.push_back(0x04); stream.push_back(0x40);   // ESC, Clear Unit
    stream.push_back(0x04); stream.push_back(0x11);   // ESC, Write To Display
    stream.push_back(0x00); stream.push_back(0x00);   // CC1/CC2: no invented input mode
    for (int at = 0, row = 1; at < count && row <= kRows; row++) {
        stream.push_back(0x11);                       // Set Buffer Address
        stream.push_back(static_cast<uint8_t>(row));
        stream.push_back(0x01);
        int take = std::min(kColumns, count - at);
        for (int i = 0; i < take; i++) {
            uint8_t b = data[offset + at + i];
            stream.push_back(b < 0x40 ? static_cast<uint8_t>(0x40) : b);
        }
        at += take;
    }
    return stream;
}

// Command hex FF, Invite. SSP invites a station and waits for it to have
// something to say. DP mode carries no data stream; WP mode requires the
// explicit D932 READ TEXT SCREEN which unlocks Controller Text Assist.
bool VirtualWorkstation::invite()
{
    inviteOutstanding_ = true;
    if (inviteReadMode_ == PutWithInviteReadMode::StructuredField21) {
        activeReadMode_ = 0x21;
        trace_.ws("station {}: invited with D932 READ TEXT SCREEN", id());
        return backend_->sendSavedReadMode(0x21);
    }
    activeReadMode_ = 1;
    trace_.ws("station {}: invited", id());
    return backend_->setInputEnabled(true);
}

// Withdraw an invite.  RFC 1205 section 4.2: the server sends Cancel Invite
// "when it needs to reverse the normal flow direction".
bool VirtualWorkstation::cancelInvite()
{
    inviteOutstanding_ = false;
    activeReadMode_ = 0;
    retainedReadMode_ = 0;
    trace_.ws("station {}: invite cancelled", id());
    return backend_->setInputEnabled(false);
}

// Take the next TN5250 body, if any: the retained device record first,
// otherwise the transport's.
bool VirtualWorkstation::tryTakeInput(std::vector<uint8_t>& stream)
{
    if (hasRetainedInput_) {
        stream = std::move(retainedDeviceInput_);
        retainedDeviceInput_.clear();
        hasRetainedInput_ = false;
        retainedReadMode_ = 0;
        inputRecords_++;
        trace_.ws("station {}: retained device input consumed, {} byte(s)", id(), stream.size());
        return true;
    }
    if (!backend_->tryTakeInput(stream)) return false;
    inputRecords_++;
    inviteOutstanding_ = false;
    activeReadMode_ = 0;
    trace_.ws("station {}: input, {} byte(s)", id(), stream.size());
    return true;
}

// Consume one retained terminal response and construct the guest-visible
// result for the SSP work station read command.
bool VirtualWorkstation::tryTakeInputFields(uint8_t command, std::vector<uint8_t>& stream)
{
    const uint8_t responseMode = hasRetainedInput_ ? retainedReadMode_ : activeReadMode_;
    if (!tryTakeInput(stream)) return false;
    retainedReadMode_ = 0;
    if (command == 0x42 && responseMode != 0x21) {
        int wireLength = static_cast<int>(stream.size());
        stream = deviceDisplay_.expandModifiedInput(stream);
        trace_.ws("station {}: command 42 device transform {} wire byte(s) -> {} cursor/AID + contiguous input-field byte(s)",
                  id(), wireLength, stream.size());
    }
    return true;
}

// Expose the real response's cursor/AID status once while retaining the
// record for Read Input Fields.
bool VirtualWorkstation::tryTakeInputStatus(uint16_t& cursor, uint8_t& aid)
{
    return backend_->tryTakeInputStatus(cursor, aid);
}

// The terminal has answered the read command appended to a PUT-with-invite.
// Move exactly one wire response into the display's retained-input slot: the
// device reports its status through the action response first and a later
// Read Input Fields extracts the modified fields.  A subsequent device
// response supersedes unread prior input, matching that single retained
// slot.
bool VirtualWorkstation::tryCompleteInviteResponse()
{
    std::vector<uint8_t> response;
    if (!backend_->tryTakeInput(response)) return false;
    bool superseded = hasRetainedInput_;
    retainedDeviceInput_ = std::move(response);
    hasRetainedInput_ = true;
    retainedReadMode_ = activeReadMode_;
    inviteOutstanding_ = false;
    activeReadMode_ = 0;
    trace_.ws("station {}: PUT-with-invite response claimed from transport; invite complete, {} byte(s) retained in "
              "display input state{}",
              id(), retainedDeviceInput_.size(), superseded ? "; unread prior device input superseded" : "");
    return true;
}

bool VirtualWorkstation::pendingInputHasFields() const { return backend_->pendingInputHasFields(); }
bool VirtualWorkstation::inviteResponsePending() const { return backend_->pendingInput() != 0; }
int VirtualWorkstation::pendingWireInput() const { return backend_->pendingInput(); }
int VirtualWorkstation::pendingInput() const { return backend_->pendingInput() + (hasRetainedInput_ ? 1 : 0); }

bool VirtualWorkstation::beginSaveScreen()
{
    trace_.ws("station {}: RFC-1205 Save Screen request 04/0402", id());
    return backend_->sendSaveScreen();
}

bool VirtualWorkstation::tryTakeSaveScreen(std::vector<uint8_t>& body) { return backend_->tryTakeSaveScreen(body); }
int VirtualWorkstation::pendingSaveScreens() const { return backend_->pendingSaveScreens(); }

bool VirtualWorkstation::restoreScreen(const uint8_t* data, int offset, int length)
{
    trace_.ws("station {}: RFC-1205 Restore Screen, {} terminal-returned byte(s)", id(), length);
    return backend_->sendRestoreScreen(data, offset, length);
}

bool VirtualWorkstation::resumeSavedReadMode(uint8_t mode)
{
    if (mode != 1 && mode != 0x20 && mode != 0x21) return false;
    bool sent = backend_->sendSavedReadMode(mode);
    if (sent) {
        inviteOutstanding_ = true;
        activeReadMode_ = mode;
    }
    return sent;
}

void VirtualWorkstation::detach()
{
    backend_->detach();
    inviteOutstanding_ = false;
    activeReadMode_ = 0;
    hasRetainedInput_ = false;
    retainedDeviceInput_.clear();
    retainedReadMode_ = 0;
}

bool VirtualWorkstation::restoreCheckpoint(int tub, WorkstationOutputMode mode, PutWithInviteReadMode readMode,
                                           bool inviteOutstanding, uint8_t activeReadMode, long long outputStreams,
                                           long long outputBytes, long long inputRecords,
                                           const std::vector<uint8_t>* lastOutput)
{
    if (activeReadMode != 0 && activeReadMode != 1 && activeReadMode != 0x20 && activeReadMode != 0x21) return false;
    tubAddress = tub;
    outputMode_ = mode;
    inviteReadMode_ = readMode;
    inviteOutstanding_ = inviteOutstanding;
    activeReadMode_ = inviteOutstanding ? activeReadMode : static_cast<uint8_t>(0);
    outputDataStreams_ = outputStreams;
    outputDataBytes_ = outputBytes;
    inputRecords_ = inputRecords;
    hasLastOutput_ = lastOutput != nullptr;
    lastOutputDataStream_ = lastOutput != nullptr ? *lastOutput : std::vector<uint8_t>();
    hasRetainedInput_ = false;
    retainedDeviceInput_.clear();
    retainedReadMode_ = 0;
    if (inviteOutstanding && activeReadMode_ == 0x21)
        backend_->sendSavedReadMode(0x21);
    else
        backend_->setInputEnabled(inviteOutstanding);
    return true;
}

}  // namespace sim36::devices
