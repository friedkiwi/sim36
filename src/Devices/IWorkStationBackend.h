// The seam between the work station controller and whatever terminal or
// printer sits behind a slot.  Nothing above it holds a TCP-aware type, so
// replacing the host half means replacing one class, not editing the SVC
// path.  A display has an invite and an input path; a printer has an end of
// job and neither, so the two seams are genuinely different shapes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim36::devices {

class VirtualWorkstation;
class VirtualPrinter;

class IWorkStationBackend {
public:
    virtual ~IWorkStationBackend() = default;

    virtual std::string name() const = 0;
    virtual bool attached() const = 0;
    virtual std::string userName() const = 0;

    virtual bool put(const uint8_t* data, int offset, int length) = 0;
    virtual bool putWithInvite(const uint8_t* data, int offset, int length) = 0;

    virtual bool beginSaveScreen() = 0;
    virtual bool tryTakeSaveScreen(std::vector<uint8_t>& body) = 0;
    virtual int pendingSaveScreens() const = 0;
    virtual bool restoreScreen(const uint8_t* data, int offset, int length) = 0;
    virtual bool resumeSavedReadMode(uint8_t mode) = 0;

    virtual void setInputEnabled(bool enabled) = 0;

    virtual bool tryTakeInputStatus(uint16_t& cursor, uint8_t& aid) = 0;
    virtual bool tryCompleteInviteResponse() = 0;
    virtual bool inviteResponsePending() const = 0;
    virtual bool retainedInputHasFields() const = 0;
    virtual bool pendingInputHasFields() const = 0;

    virtual bool tryTakeInput(std::vector<uint8_t>& stream) = 0;
    virtual bool tryTakeInputFields(uint8_t command, std::vector<uint8_t>& stream) = 0;
    virtual int pendingInput() const = 0;
};

// The printer's side of the seam.  Hand a data stream over verbatim, and
// tell the printer a job is finished: no guest event drives the latter (the
// routine that maps the IOB command to an action code is not decoded for
// it), so the operator drives it.
class IPrinterBackend {
public:
    virtual ~IPrinterBackend() = default;

    // A stable name for traces.  Not the unit address: the controller owns
    // addressing.
    virtual std::string name() const = 0;
    // Is a printer client bound to this slot right now?  A slot with no
    // session is idle, not absent.
    virtual bool attached() const = 0;
    // False when there was nowhere to put it.
    virtual bool sendDataStream(const uint8_t* data, int offset, int length) = 0;
    virtual bool endJob() = 0;
};

// Adapts a VirtualWorkstation, which owns the socket through its host
// backend, to the seam.
class VirtualWorkstationBackend : public IWorkStationBackend {
public:
    explicit VirtualWorkstationBackend(VirtualWorkstation& ws) : ws_(ws) {}

    std::string name() const override;
    bool attached() const override;
    std::string userName() const override;
    bool put(const uint8_t* data, int offset, int length) override;
    bool putWithInvite(const uint8_t* data, int offset, int length) override;
    bool beginSaveScreen() override;
    bool tryTakeSaveScreen(std::vector<uint8_t>& body) override;
    int pendingSaveScreens() const override;
    bool restoreScreen(const uint8_t* data, int offset, int length) override;
    bool resumeSavedReadMode(uint8_t mode) override;
    void setInputEnabled(bool enabled) override;
    bool tryTakeInputStatus(uint16_t& cursor, uint8_t& aid) override;
    bool tryCompleteInviteResponse() override;
    bool inviteResponsePending() const override;
    bool retainedInputHasFields() const override;
    bool pendingInputHasFields() const override;
    bool tryTakeInput(std::vector<uint8_t>& stream) override;
    bool tryTakeInputFields(uint8_t command, std::vector<uint8_t>& stream) override;
    int pendingInput() const override;

private:
    VirtualWorkstation& ws_;
};

// The mirror for a printer.
class VirtualPrinterBackend : public IPrinterBackend {
public:
    explicit VirtualPrinterBackend(VirtualPrinter& p) : p_(p) {}

    std::string name() const override;
    bool attached() const override;
    bool sendDataStream(const uint8_t* data, int offset, int length) override;
    bool endJob() override;

private:
    VirtualPrinter& p_;
};

}  // namespace sim36::devices
