#include "Devices/VirtualPrinter.h"

#include <fmt/format.h>

namespace sim36::devices {

VirtualPrinter::VirtualPrinter(const configuration::StationConfig& cfg, host::PrinterBackend& backend,
                               monitor::Tracer& trace, bool ownsBackend)
    : cfg_(cfg), backend_(&backend), trace_(trace), ownsBackend_(ownsBackend)
{
    // The startup response's object name is this device's name on the wire.
    // Nothing on the volume names a printer, so it is derived from the
    // station's address, which is the one identity that is not invented.
    backend_->objectName = fmt::format("PRT{}{}", cfg.port, cfg.address);
}

VirtualPrinter::~VirtualPrinter()
{
    if (ownsBackend_) delete backend_;
}

bool VirtualPrinter::attached() const { return backend_->attached(); }
bool VirtualPrinter::ready() const { return backend_->ready(); }
int VirtualPrinter::listenPort() const { return backend_->port(); }
void VirtualPrinter::start() { backend_->listen(); }

bool VirtualPrinter::outputData(const uint8_t* dataStream, int offset, int length)
{
    outputDataStreams_++;
    outputDataBytes_ += length;
    lastOutputDataStream_.assign(dataStream + offset, dataStream + offset + length);
    hasLastOutput_ = true;
    trace_.ws("printer {}: output data, {} byte(s)", id(), length);
    return backend_->sendDataStream(dataStream, offset, length);
}

bool VirtualPrinter::endJob()
{
    trace_.ws("printer {}: end of job (operator, not guest - no recovered System/36 event means end of spool file)", id());
    return backend_->endJob();
}

void VirtualPrinter::detach() { backend_->detach(); }

void VirtualPrinter::restoreCheckpoint(int pub, long long outputStreams, long long outputBytes,
                                       const std::vector<uint8_t>* lastOutput)
{
    pubAddress = pub;
    outputDataStreams_ = outputStreams;
    outputDataBytes_ = outputBytes;
    hasLastOutput_ = lastOutput != nullptr;
    lastOutputDataStream_ = lastOutput != nullptr ? *lastOutput : std::vector<uint8_t>();
}

}  // namespace sim36::devices
