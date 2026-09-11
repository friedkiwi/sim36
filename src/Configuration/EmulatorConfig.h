// The editable machine definition: model, storage, media, stations and the
// session policy knobs.  The monitor owns one of these from the start; `ipl`
// validates and latches it into a constructed machine.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Configuration/ModelTable.h"

namespace sim36::configuration {

struct StationConfig {
    int port = 0;
    int address = 0;              // 0-6; the 3-bit twinax station address
    std::string role = "display";
    std::string deviceCode = "11";   // 3180 Model 2
    bool deviceCodeGiven = false;
    int listenPort = 0;
    std::string listenHost = "127.0.0.1";
    // Work-station configuration record +0A: host ownership policy, not
    // AUTOSIGNON and not a bit copied into a guest block.
    bool signOnAtIpl = false;

    std::string id() const { return std::to_string(port) + "." + std::to_string(address); }
    bool isPrinter() const;
    bool isConsole() const { return port == 0 && address == 0; }

    // The three roles a slot can have, a closed set.  The role decides the
    // device class in the six-byte configuration record and which kind of
    // 5250 client the host end will serve.
    static const std::vector<std::string>& roles();
};

// A tape drive: declares the drive and, optionally, a folder to mount at
// power-on.  A tape's container is a FOLDER (a manifest plus one blob per
// tape file).  Leave folderPath empty for an EMPTY drive.
struct TapeConfig {
    std::string folderPath;
    // Writable by default, unlike the fixed disk and diskette: a tape's whole
    // purpose is save/restore, and a save writes it.
    bool readOnly = false;
};

class EmulatorConfig {
public:
    std::string volumePath;
    bool volumeReadOnly = true;
    // Writes are accepted and held in memory, never committed.  Takes
    // precedence over volumeReadOnly when set.
    bool volumeOverlay = false;
    // The model ceiling, not a guess: the control-storage IPL writes CCR size
    // code 0x05 (= 1 MB) to guest 0x084C unconditionally, so 1 MB is what
    // every Advanced/36 says it is.
    int mainStorageKb = 1024;

    // A flat diskette image to have in the drive at power-on, or empty for an
    // empty drive.  There is deliberately no geometry beside it: the volume
    // declares its own layout in VOL1.
    std::string diskettePath;
    bool disketteReadOnly = true;

    // Sectors of task work area, direct area word 1124.  SA21-9436's TWAL
    // entry: "The default value is 60 sectors."
    int taskWorkAreaSectors = 60;

    // The seven bytes the host processor description occupies in low
    // storage, measured on a running machine: EBCDIC "150" at 08E2..08E4,
    // packed BCD 2270 at 08E6/08E7, EBCDIC "440" at 0A88..0A8A.  They
    // describe the HOST, so they are configuration.
    std::string hostModel = "150";
    int hostProcessorFeature = 0x2270;
    std::string hostProcessorModel = "440";
    std::string iplType = "unattend";

    // Default AUTOSIGNON value for the transfer request the listener
    // synthesises for an attaching 5250 client.  Host policy; reaches no
    // guest storage.
    bool listenerAutoSignOn = false;

    // The router event key a console activation posts to the resident
    // command router.
    int consoleSignOnRouterKey = 0x14;
    bool consoleSignOnUseRouter = true;
    bool consoleSignOnStatement = false;
    bool consoleSignOnRequest = false;
    bool wsInteractive = false;

    // The default display transport: one listener serves every display
    // station.
    bool stationMultiplex = true;
    std::string multiplexHost = "127.0.0.1";
    int multiplexPort = 2300;

    // The front panel's load-source selection (the RELOAD source phase 1
    // consults).
    std::string iplSourceName = "disk";
    // Where the CONTROL PROCESSOR gets phase 1: `disk` reads the boot record
    // from sector 8191; `diskette` reads the diskette-resident phase 1.
    std::string loadSourceName = "disk";

    // The user-facing knob.  Everything else about the processor complex
    // derives from it.
    std::string model = "advanced36";

    std::vector<StationConfig> stations;
    std::unique_ptr<TapeConfig> tape;

    EmulatorConfig() = default;
    EmulatorConfig(const EmulatorConfig& other);
    EmulatorConfig& operator=(const EmulatorConfig& other);

    bool loadsFromDiskette() const;
    bool iplRequestsReload() const;
    int iplSource() const;
    CspKind cspKind() const { return ModelTable::kindOf(model); }
    std::string cspVariant() const { return ModelTable::variantOf(model); }
    int maxMainStorageKb() const { return ModelTable::maxStorageKb(model); }

    const StationConfig* console() const;
    StationConfig* findStation(int port, int address);

    // The default machine's work stations, applied only when a configuration
    // declares none of its own: 0.0 the console, 0.1-0.6 displays listening
    // on 127.0.0.1:2301-2306.
    void applyDefaultStationsIfNoneDeclared();

    // The legacy INI reader.  Throws ConfigError.
    static EmulatorConfig load(const std::string& path);

    // Validate a complete machine definition before any host resources are
    // opened.  Throws ConfigError.
    void validate(const std::string& path);
};

}  // namespace sim36::configuration
