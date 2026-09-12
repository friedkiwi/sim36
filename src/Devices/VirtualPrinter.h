// One work-station-attached printer, as the System/36 sees it.
//
// A twinax printer rides the same controller and the same handler as a
// display.  SA21-9436 5-50: the printer unit block "is used by the work
// station input/output control handler (WSIOCH) routine and the printer
// input/output control handler (PTIOCH) routine ... WSIOCH is common for
// locally attached display stations and work station attached printers."
// So this device sits at a (port, address) on the controller beside the
// displays, is reported by Read Current Configuration in the same six-byte
// record, and is addressed by the same one-byte unit address.  What makes
// it a printer, to the guest, is record byte 1 bits 0x30.
//
// This model owns the System/36 concepts; the host printer backend below it
// owns the socket and RFC 2877's framing.  The guest hands a data stream
// over by address and does not expect it parsed in transit, so this class
// forwards bytes and does not read them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Host/StationBackend.h"
#include "Monitor/Tracer.h"

namespace sim36::devices {

class VirtualPrinter {
public:
    VirtualPrinter(const configuration::StationConfig& cfg, host::PrinterBackend& backend, monitor::Tracer& trace,
                   bool ownsBackend = true);
    ~VirtualPrinter();
    VirtualPrinter(const VirtualPrinter&) = delete;
    VirtualPrinter& operator=(const VirtualPrinter&) = delete;

    std::string id() const { return cfg_.id(); }
    int port() const { return cfg_.port; }
    int address() const { return cfg_.address; }
    const std::string& role() const { return cfg_.role; }
    const std::string& deviceCode() const { return cfg_.deviceCode; }

    // Guest address of this printer's unit block, once SSP has built one.
    // Zero until then.  SA21-9436 5-50: the PUB is 96 bytes and IPL phase 2
    // builds it; the emulator reads it, it does not write it.
    int pubAddress = 0;

    host::PrinterBackend& backend() { return *backend_; }
    const host::PrinterBackend& backend() const { return *backend_; }
    bool attached() const;
    bool ready() const;
    int listenPort() const;
    void start();

    long long outputDataStreams() const { return outputDataStreams_; }
    long long outputDataBytes() const { return outputDataBytes_; }
    bool hasLastOutputDataStream() const { return hasLastOutput_; }
    const std::vector<uint8_t>& lastOutputDataStream() const { return lastOutputDataStream_; }

    // Command hex 21, Output Data: the bytes are forwarded verbatim.  "For
    // printer output data stream commands the IOB contains an address of a
    // data stream in main or control storage" (SA21-9436 5-50).
    bool outputData(const uint8_t* dataStream, int offset, int length);
    // Operator-driven end of job: no recovered guest event means end of
    // spool file.
    bool endJob();
    void detach();
    void restoreCheckpoint(int pub, long long outputStreams, long long outputBytes, const std::vector<uint8_t>* lastOutput);

private:
    configuration::StationConfig cfg_;
    host::PrinterBackend* backend_;
    monitor::Tracer& trace_;
    bool ownsBackend_;
    long long outputDataStreams_ = 0;
    long long outputDataBytes_ = 0;
    bool hasLastOutput_ = false;
    std::vector<uint8_t> lastOutputDataStream_;
};

}  // namespace sim36::devices
