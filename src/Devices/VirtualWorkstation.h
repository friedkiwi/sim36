// One display station, as the System/36 sees it.
//
// This is the device model and it sits directly above the host backend
// seam, the way the virtual fixed disk sits above the disk backend.  Here
// live the System/36 concepts: the port and address that make up a station
// identity, the device code, whether this is the console, the terminal
// unit block, the invite state.  Below live the host concepts: a socket,
// Telnet negotiation, the RFC 1205 record header.  The backend has never
// heard of an IOB.
//
// The guest supplies an inner WSDM byte range; the device renders it into
// controller orders, carries action/operation state and parses the inbound
// half.  Outbound PUT versus PUT-with-invite and its read suffix are
// retained; an asynchronous A7 completion retains the originating request
// and separates newly arrived wire responses from device-retained field
// input.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Host/ConsoleDisplay.h"
#include "Host/StationBackend.h"
#include "Monitor/Tracer.h"

namespace sim36::devices {

// How guest display bytes cross the standalone-emulator boundary.
// PassThrough exposes the inner WSDM payload directly as a TN5250 body and
// matches the ordinary display PUT byte flow.  WtdText is an explicitly
// selected compatibility adapter for raw WSDM character buffers: it wraps
// them in a minimal 5250 Clear-Unit/Write-To-Display stream while leaving
// streams that already contain 5250 orders untouched.  SlicDisplay is the
// exact byte shape of the display's write-only-message renderer, an
// experimental negative control because the ordinary facade does not call
// that overload.
enum class WorkstationOutputMode { PassThrough, WtdText, SlicDisplay };

// The two read-command suffixes of the display's put-with-invite operation.
// DP mode uses operation 0x20. D934 selects WP mode and its D932 structured
// read (0x21); the monitor can also select either for controlled experiments.
enum class PutWithInviteReadMode { ReadInputFields20, StructuredField21 };

const char* workstationOutputModeName(WorkstationOutputMode mode);
const char* putWithInviteReadModeName(PutWithInviteReadMode mode);

class VirtualWorkstation {
public:
    // The backend is either owned by this station (a machine-owned backend)
    // or shared with the session chassis; `ownsBackend` says which.
    VirtualWorkstation(const configuration::StationConfig& cfg, host::WorkstationBackend& backend,
                       monitor::Tracer& trace, bool ownsBackend = true);
    ~VirtualWorkstation();
    VirtualWorkstation(const VirtualWorkstation&) = delete;
    VirtualWorkstation& operator=(const VirtualWorkstation&) = delete;

    // ---- System/36 identity ---------------------------------------------
    // Port and address: the unit address SSP names in an IOB is
    // (port << 4) | address, masked & 0x77 on the way out and checked & 0x88
    // on the way in.
    std::string id() const { return cfg_.id(); }
    int port() const { return cfg_.port; }
    int address() const { return cfg_.address; }
    const std::string& role() const { return cfg_.role; }
    const std::string& deviceCode() const { return cfg_.deviceCode; }
    bool signOnAtIpl() const { return cfg_.signOnAtIpl; }
    // SC21-9052 page 2-16: "The system console must be placed at work
    // station address 0."
    bool isConsole() const { return cfg_.port == 0 && cfg_.address == 0; }
    bool isPrinter() const;

    // Guest address of this station's terminal unit block, once phase 2 has
    // built it.  Zero until then.
    int tubAddress = 0;

    WorkstationOutputMode outputMode() const { return outputMode_; }
    PutWithInviteReadMode inviteReadMode() const { return inviteReadMode_; }
    void setOutputMode(WorkstationOutputMode mode);
    void setInviteReadMode(PutWithInviteReadMode mode);

    // The host side, exposed so the monitor can report what the session is
    // doing; the guest path goes through the methods below.
    host::WorkstationBackend& backend() { return *backend_; }
    const host::WorkstationBackend& backend() const { return *backend_; }
    // Read-only access to the device's retained display image, for monitor
    // diagnostics.
    const host::ConsoleDisplay& deviceDisplay() const { return deviceDisplay_; }
    host::ConsoleDisplay& deviceDisplay() { return deviceDisplay_; }

    // Non-mutating copy of the device's retained input record; false when
    // there is none.
    bool copyRetainedDeviceInput(std::vector<uint8_t>& copy) const;
    uint8_t activeReadMode() const { return activeReadMode_; }

    bool attached() const;
    bool ready() const;
    // Guest thread only: read and clear this station's I/O-attention latch.
    bool takeAttentionPending();
    bool attentionPending() const;
    bool takePowerOffPending();
    bool powerOffPending() const;
    // The device-model half of a display power-off: every piece of state
    // that belonged to the switched-off terminal dies; the transport is not
    // touched because a new client may already own the socket.
    void powerOff();
    int listenPort() const;
    void start();

    // ---- the guest's side ------------------------------------------------
    bool inviteOutstanding() const { return inviteOutstanding_; }
    long long outputDataStreams() const { return outputDataStreams_; }
    long long outputDataBytes() const { return outputDataBytes_; }
    long long inputRecords() const { return inputRecords_; }
    // The last data stream the guest handed over; false when none yet.
    bool hasLastOutputDataStream() const { return hasLastOutput_; }
    const std::vector<uint8_t>& lastOutputDataStream() const { return lastOutputDataStream_; }

    bool outputData(const uint8_t* dataStream, int offset, int length);
    bool outputDataWithInvite(const uint8_t* dataStream, int offset, int length);
    bool invite();
    bool cancelInvite();
    bool tryTakeInput(std::vector<uint8_t>& stream);
    bool tryTakeInputFields(uint8_t command, std::vector<uint8_t>& stream);
    bool tryTakeInputStatus(uint16_t& cursor, uint8_t& aid);
    bool tryCompleteInviteResponse();
    bool pendingInputHasFields() const;
    bool retainedInputHasFields() const { return hasRetainedInput_ && retainedDeviceInput_.size() > 3; }
    bool inviteResponsePending() const;
    int pendingWireInput() const;
    bool retainedDeviceInput() const { return hasRetainedInput_; }
    int pendingInput() const;
    uint8_t savedReadMode() const { return inviteOutstanding_ ? activeReadMode_ : static_cast<uint8_t>(0); }
    bool beginSaveScreen();
    bool tryTakeSaveScreen(std::vector<uint8_t>& body);
    int pendingSaveScreens() const;
    bool restoreScreen(const uint8_t* data, int offset, int length);
    bool resumeSavedReadMode(uint8_t mode);
    // Detach the session without destroying the station: a slot persists
    // across disconnects.
    void detach();

    // Restore device-model state.  Socket/session state deliberately is not
    // included.  False (nothing changed) when the read mode is not one of
    // the four the device knows.
    bool restoreCheckpoint(int tub, WorkstationOutputMode mode, PutWithInviteReadMode readMode,
                           bool inviteOutstanding, uint8_t activeReadMode, long long outputStreams,
                           long long outputBytes, long long inputRecords, const std::vector<uint8_t>* lastOutput);

private:
    bool outputOperation(const uint8_t* dataStream, int offset, int length, bool withInvite);
    static std::vector<uint8_t> wrapAsSlicDisplayWriteOnlyMessage(const uint8_t* data, int offset, int length);
    static bool looksLike5250DataStream(const uint8_t* data, int offset, int length);
    static bool containsTextAssistFormat(const uint8_t* data, int offset, int length);
    static bool containsWriteToDisplay(const uint8_t* data, int offset, int length);
    static std::vector<uint8_t> wrapWsdmTextAs5250(const uint8_t* data, int offset, int length);

    configuration::StationConfig cfg_;
    host::WorkstationBackend* backend_;
    monitor::Tracer& trace_;
    bool ownsBackend_;
    // The device owns a screen image and format table independently of the
    // attached terminal: it needs that state to turn a Read-MDT wire reply
    // into command 42's contiguous all-fields result.
    host::ConsoleDisplay deviceDisplay_;
    WorkstationOutputMode outputMode_ = WorkstationOutputMode::PassThrough;
    PutWithInviteReadMode inviteReadMode_ = PutWithInviteReadMode::ReadInputFields20;
    bool inviteOutstanding_ = false;
    uint8_t activeReadMode_ = 0;
    // The device keeps received input independently of the live asynchronous
    // put/get operation: moving a response here is what prevents one record
    // from completing two successive A7s while preserving it for Read Input.
    bool hasRetainedInput_ = false;
    std::vector<uint8_t> retainedDeviceInput_;
    // The read operation which produced retainedDeviceInput_. D932 replies
    // are structured fields and must not go through DP-mode field compaction.
    uint8_t retainedReadMode_ = 0;
    struct SavedDisplayState {
        std::vector<uint8_t> terminalBody;
        host::ConsoleDisplay::RetainedState display;
    };
    std::deque<host::ConsoleDisplay::RetainedState> pendingScreenSaveStates_;
    std::deque<SavedDisplayState> savedDisplayStates_;
    static constexpr int kSavedDisplayStateLimit = 16;
    long long outputDataStreams_ = 0;
    long long outputDataBytes_ = 0;
    long long inputRecords_ = 0;
    bool hasLastOutput_ = false;
    std::vector<uint8_t> lastOutputDataStream_;
};

}  // namespace sim36::devices
