// The panic dump: a support archive made at a monitor boundary after an
// operator observes bad behaviour.  It contains no fixed-disk, diskette or
// tape bytes; the volatile binary record is the checkpoint encoder's
// complete runtime state, and the other entries make common triage possible
// without a decoder or restoring anything.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Host/StationBackend.h"

namespace sim36::machine { class Machine; }

namespace sim36::monitor {

class PanicDump {
public:
    static constexpr int kFormatVersion = 1;

    // Create the archive in the temporary directory and return its path.
    // Throws std::runtime_error when the archive cannot be created; a
    // partial file is removed.  `machine` may be null (no machine had been
    // constructed); `backends` are the chassis listeners.
    static std::string create(const std::string& description, const std::string& reproduction,
                              const configuration::EmulatorConfig& config, machine::Machine* machine,
                              uint32_t pendingTrace, const std::vector<host::StationBackend*>& backends);
};

}  // namespace sim36::monitor
