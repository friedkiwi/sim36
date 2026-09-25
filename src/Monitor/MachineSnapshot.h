// The checkpoint file: a gzip-compressed record of the machine definition,
// the mounted media and, when a machine is constructed, its complete
// volatile state.  The format is fixed by a magic and a version and is
// read back exactly as it was written; a checkpoint never migrates.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"
#include "Storage/TapeBackend.h"

namespace sim36::machine { class Machine; }

namespace sim36::monitor {

class MachineSnapshot {
public:
    static constexpr int kVersion = 23;   // 23: exact workstation TU ownership in pending I/O

    struct StationState {
        std::string id;
        int tubAddress = 0;
        int outputMode = 0, inviteReadMode = 0;
        bool inviteOutstanding = false;
        uint8_t activeReadMode = 0;
        long long outputStreams = 0, outputBytes = 0, inputRecords = 0;
        bool hasLastOutput = false;
        std::vector<uint8_t> lastOutput;
    };

    struct PrinterState {
        std::string id;
        int pubAddress = 0;
        long long outputStreams = 0, outputBytes = 0;
        bool hasLastOutput = false;
        std::vector<uint8_t> lastOutput;
    };

    struct RuntimeState {
        std::vector<uint8_t> main;
        std::vector<uint16_t> atr;
        long long cycles = 0;
        int m36Src = 0;
        uint16_t iar = 0, arr = 0, xr1 = 0, xr2 = 0;
        uint16_t wr[8] = {};
        uint8_t psr = 0, pactDir = 0, pactXr1 = 0, pactXr2 = 0, pactIar = 0, pactReg = 0, pactAtr = 0, pactCsp = 0, pmr = 0,
                cmr = 0;
        bool stopped = false, atPreemptionPoint = false;
        std::string stopReason;
        long long instructions = 0;
        long long schedulerNow = 0;
        int currentTaskBlock = 0, currentRequestBlock = 0;
        processors::controlstorage::As36ControlStorageProcessor::CheckpointState csp;
        devices::DeviceSet::PendingCheckpoint devices;
        storage::TapePosition tapePosition;
        bool hasTapePosition = false;
        std::vector<StationState> stations;
        std::vector<PrinterState> printers;
    };

    struct Loaded {
        configuration::EmulatorConfig config;
        bool powered = false;
        uint32_t trace = 0;
        std::unique_ptr<RuntimeState> runtime;
        std::string mediaDirectory;
    };

    // Write the checkpoint.  Throws std::runtime_error with the reason when
    // the machine is not at a checkpointable boundary or the media are
    // absent; the file is written whole or not at all.
    static void save(const std::string& path, const configuration::EmulatorConfig& definition,
                     machine::Machine* machine, uint32_t pendingTrace);
    // Read a checkpoint into a fresh media directory beside it.  Throws
    // std::runtime_error on a malformed file; the media directory is then
    // removed.
    static Loaded load(const std::string& path);
    // The same complete volatile-state record a checkpoint carries, without
    // the media, for the panic dump: one encoder, so the emergency path
    // cannot quietly omit a latch.
    static std::vector<uint8_t> writeDiagnosticRuntime(machine::Machine& machine);

private:
    class Writer;
    class Reader;
    static void writeConfig(Writer& w, const configuration::EmulatorConfig& c);
    static configuration::EmulatorConfig readConfig(Reader& r);
    static void writeRuntime(Writer& w, machine::Machine& m);
    static RuntimeState readRuntime(Reader& r);
};

}  // namespace sim36::monitor
