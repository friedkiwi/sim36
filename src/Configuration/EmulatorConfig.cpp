#include "Configuration/EmulatorConfig.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include <fmt/format.h>

#include "Configuration/ConfigError.h"
#include "Configuration/IplSourceTable.h"
#include "Devices/DeviceCodes.h"
#include "Storage/FileNotFoundError.h"
#include "Monitor/CommandRegistry.h"

namespace sim36::configuration {

using monitor::equalsIgnoreCase;

namespace {

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parseBool(const std::string& v)
{
    const std::string s = monitor::toLower(v);
    return s == "yes" || s == "true" || s == "1" || s == "on";
}

// int.Parse semantics: optional sign, decimal digits, whole string.
bool parseInt32(const std::string& v, int& out)
{
    const std::string s = trim(v);
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i == s.size()) return false;
    for (std::size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    errno = 0;
    long long n = std::strtoll(s.c_str(), nullptr, 10);
    if (errno == ERANGE || n > 2147483647LL || n < -2147483648LL) return false;
    out = static_cast<int>(n);
    return true;
}

// Trailing '#' or ';' comments, but only when preceded by whitespace so a
// value may legitimately contain one.
std::string stripComment(const std::string& v)
{
    for (std::size_t i = 1; i < v.size(); ++i)
        if ((v[i] == '#' || v[i] == ';') && std::isspace(static_cast<unsigned char>(v[i - 1])))
            return v.substr(0, i);
    return v;
}

void parseListen(StationConfig& s, const std::string& v, const std::string& path, int line)
{
    std::size_t colon = v.rfind(':');
    if (colon == std::string::npos) throw ConfigError(path, line, "listen needs host:port");
    s.listenHost = v.substr(0, colon);
    int port = 0;
    if (!parseInt32(v.substr(colon + 1), port))
        throw ConfigError(path, line, "Input string was not in a correct format.");
    s.listenPort = port;
}

}  // namespace

bool StationConfig::isPrinter() const { return equalsIgnoreCase(role, "printer"); }

const std::vector<std::string>& StationConfig::roles()
{
    static const std::vector<std::string> r = {"console", "display", "printer"};
    return r;
}

EmulatorConfig::EmulatorConfig(const EmulatorConfig& other) { *this = other; }

EmulatorConfig& EmulatorConfig::operator=(const EmulatorConfig& other)
{
    if (this == &other) return *this;
    volumePath = other.volumePath;
    volumeReadOnly = other.volumeReadOnly;
    volumeOverlay = other.volumeOverlay;
    diskettePath = other.diskettePath;
    disketteReadOnly = other.disketteReadOnly;
    taskWorkAreaSectors = other.taskWorkAreaSectors;
    hostModel = other.hostModel;
    hostProcessorFeature = other.hostProcessorFeature;
    hostProcessorModel = other.hostProcessorModel;
    iplType = other.iplType;
    listenerAutoSignOn = other.listenerAutoSignOn;
    consoleSignOnRouterKey = other.consoleSignOnRouterKey;
    consoleSignOnUseRouter = other.consoleSignOnUseRouter;
    consoleSignOnStatement = other.consoleSignOnStatement;
    consoleSignOnRequest = other.consoleSignOnRequest;
    wsInteractive = other.wsInteractive;
    stationMultiplex = other.stationMultiplex;
    multiplexHost = other.multiplexHost;
    multiplexPort = other.multiplexPort;
    iplSourceName = other.iplSourceName;
    model = other.model;
    cspType = other.cspType;
    stations = other.stations;
    tape = other.tape ? std::make_unique<TapeConfig>(*other.tape) : nullptr;
    return *this;
}

bool EmulatorConfig::loadsFromDiskette() const
{
    if (equalsIgnoreCase(iplSourceName, "disk")) return false;
    return (IplSourceTable::encode(iplSourceName, "unattend") & IplSourceTable::kSourceMask) == 0;
}

bool EmulatorConfig::loadsFromTape() const
{
    return (IplSourceTable::encode(iplSourceName, "unattend") & IplSourceTable::kSourceMask) != 0;
}
bool EmulatorConfig::iplRequestsReload() const { return IplSourceTable::requestsReload(iplSourceName); }
int EmulatorConfig::iplSource() const { return IplSourceTable::encode(iplSourceName, iplType); }

const StationConfig* EmulatorConfig::console() const
{
    for (const StationConfig& s : stations)
        if (s.isConsole()) return &s;
    return nullptr;
}

StationConfig* EmulatorConfig::findStation(int port, int address)
{
    for (StationConfig& s : stations)
        if (s.port == port && s.address == address) return &s;
    return nullptr;
}

const StationConfig* EmulatorConfig::findStation(int port, int address) const
{
    for (const StationConfig& s : stations)
        if (s.port == port && s.address == address) return &s;
    return nullptr;
}

void EmulatorConfig::applyDefaultStationsIfNoneDeclared()
{
    if (!stations.empty()) return;
    StationConfig console;
    console.port = 0;
    console.address = 0;
    console.role = "console";
    stations.push_back(console);
    for (int address = 1; address <= 6; ++address) {
        StationConfig s;
        s.port = 0;
        s.address = address;
        if (address == 1) {
            s.role = "printer";
            s.deviceCode = "PB";
            s.deviceCodeGiven = true;
            s.printerOutput = "console";
        } else {
            s.role = "display";
            s.listenPort = 2300 + address;
        }
        stations.push_back(s);
    }
}

EmulatorConfig EmulatorConfig::load(const std::string& path)
{
    EmulatorConfig c;
    std::string section;
    StationConfig* station = nullptr;
    TapeConfig* tape = nullptr;
    int lineNo = 0;
    bool sawIplSource = false;
    bool sawLegacyLoadSource = false;

    std::ifstream in(path);
    if (!in) throw storage::FileNotFoundError::forPath(path);
    std::string raw;
    while (std::getline(in, raw)) {
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            std::size_t b = 0, e = line.size();
            while (b < e && (line[b] == '[' || line[b] == ']')) ++b;
            while (e > b && (line[e - 1] == '[' || line[e - 1] == ']')) --e;
            section = trim(line.substr(b, e - b));
            station = nullptr;
            tape = nullptr;
            if (section.size() >= 7 && equalsIgnoreCase(section.substr(0, 7), "station")) {
                std::string spec = trim(section.substr(7));
                std::size_t dot = spec.find('.');
                if (dot == std::string::npos || spec.find('.', dot + 1) != std::string::npos)
                    throw ConfigError(path, lineNo, "station needs port.address");
                StationConfig s;
                if (!parseInt32(spec.substr(0, dot), s.port) || !parseInt32(spec.substr(dot + 1), s.address))
                    throw ConfigError(path, lineNo, "Input string was not in a correct format.");
                if (s.address < 0 || s.address > 6)
                    throw ConfigError(path, lineNo,
                        "address must be 0-6; the wire address is three bits with 111 reserved");
                // The port is three bits too, and the controller packs it as
                // ((port & 7) << 4).  Without this check a port of 8 silently
                // aliased onto port 0 and two stations collided on one unit.
                if (s.port < 0 || s.port > 7)
                    throw ConfigError(path, lineNo,
                        "port must be 0-7; WorkStationController.UnitAddress packs it "
                        "as ((port & 7) << 4), so a larger value aliases onto another "
                        "controller instead of failing");
                c.stations.push_back(s);
                station = &c.stations.back();
            } else if (equalsIgnoreCase(section, "tape")) {
                // A single tape drive.  Re-declaring [tape] replaces the earlier one.
                c.tape = std::make_unique<TapeConfig>();
                tape = c.tape.get();
            }
            continue;
        }

        std::size_t eq = line.find('=');
        if (eq == std::string::npos) throw ConfigError(path, lineNo, "expected key = value");
        std::string k = monitor::toLower(trim(line.substr(0, eq)));
        std::string v = trim(stripComment(line.substr(eq + 1)));

        auto intValue = [&](const std::string& s) {
            int n = 0;
            if (!parseInt32(s, n)) throw ConfigError(path, lineNo, "Input string was not in a correct format.");
            return n;
        };

        if (section == "machine") {
            if (k == "volume") c.volumePath = v;
            else if (k == "volume_readonly") c.volumeReadOnly = parseBool(v);
            else if (k == "volume_overlay") c.volumeOverlay = parseBool(v);
            else if (k == "diskette") c.diskettePath = v;
            else if (k == "diskette_readonly") c.disketteReadOnly = parseBool(v);
            else if (k == "diskette_media" || k == "diskette_format" || k == "diskette_geometry")
                throw ConfigError(path, lineNo, fmt::format(
                    "'{}' does not exist and will not: a System/36 diskette declares "
                    "its own geometry in VOL1, so the media size, the sector size, the "
                    "sectors per track and the cylinder count are all read off the "
                    "image. Point `diskette` at a flat image and it works. "
                    "docs/file-formats/s36-diskette.md", k));
            else if (k == "task_work_area_sectors") c.taskWorkAreaSectors = intValue(v);
            else if (k == "host_model") c.hostModel = v;
            else if (k == "host_processor_model") c.hostProcessorModel = v;
            else if (k == "host_processor_feature") {
                // HEX, because the field is packed BCD: the guest sees the
                // digits, so 2270 must land as 22 70.
                bool ok = !v.empty() && v.size() <= 8;
                for (char ch : v) if (!std::isxdigit(static_cast<unsigned char>(ch))) ok = false;
                long hv = ok ? std::strtol(v.c_str(), nullptr, 16) : -1;
                if (!ok || hv < 0 || hv > 0xFFFF)
                    throw ConfigError(path, lineNo, fmt::format(
                        "'{}' is not four hex digits; host_processor_feature is "
                        "written as packed BCD into guest 08E6/08E7 "
                        "(docs/s36/csp-written-low-storage.md)", v));
                c.hostProcessorFeature = static_cast<int>(hv);
            } else if (k == "host_machine_type")
                throw ConfigError(path, lineNo,
                    "'host_machine_type' has been replaced. It wrote a guessed 9402 "
                    "into guest 08E6/08E7; a DUMP MAIN on real hardware reads 2270 "
                    "there, and 08E2/0A88 turned out to be EBCDIC text rather than "
                    "binary. Use host_model, host_processor_feature and "
                    "host_processor_model. docs/s36/csp-written-low-storage.md");
            else if (k == "ipl_type") {
                std::string type;
                if (!IplSourceTable::tryNormalizeType(v, type))
                    throw ConfigError(path, lineNo, "ipl_type must be attend/attended or unattend/unattended");
                c.iplType = type;
            } else if (k == "ipl_source") {
                if (sawLegacyLoadSource && !equalsIgnoreCase(c.iplSourceName, v))
                    throw ConfigError(path, lineNo,
                        "ipl_source conflicts with obsolete load_source; use only ipl_source");
                c.iplSourceName = v;
                sawIplSource = true;
            }
            else if (k == "load_source") {
                if (!equalsIgnoreCase(v, "disk") && !equalsIgnoreCase(v, "diskette") &&
                    !equalsIgnoreCase(v, "tape"))
                    throw ConfigError(path, lineNo,
                        "obsolete load_source must be 'disk', 'diskette', or 'tape'; use ipl_source");
                if (sawIplSource && !equalsIgnoreCase(c.iplSourceName, v))
                    throw ConfigError(path, lineNo,
                        "obsolete load_source conflicts with ipl_source; remove load_source");
                c.iplSourceName = v;
                sawLegacyLoadSource = true;
            } else if (k == "listener_auto_signon") c.listenerAutoSignOn = parseBool(v);
            else if (k == "security")
                throw ConfigError(path, lineNo,
                    "'security' has been removed. It never reached guest storage: SSP "
                    "password security lives in the volume's SECDEF/$PRUID data and the "
                    "sign-on mode byte 0x08AB is #MSNIP/#MSCPR lifecycle state set from "
                    "the volume config. All it selected was the monitor listener's own "
                    "TFRM36 AUTOSIGNON default, now 'listener_auto_signon' - note the "
                    "inverted sense: old 'security = on' is 'listener_auto_signon = off'. "
                    "docs/s36/listener-auto-signon-knob.md");
            else if (k == "signon_use_router") c.consoleSignOnUseRouter = parseBool(v);
            else if (k == "signon_statement") c.consoleSignOnStatement = parseBool(v);
            else if (k == "signon_request") c.consoleSignOnRequest = parseBool(v);
            else if (k == "ws_interactive") c.wsInteractive = parseBool(v);
            else if (k == "station_multiplex") c.stationMultiplex = parseBool(v);
            else if (k == "station_multiplex_listen") {
                std::size_t colon = v.rfind(':');
                if (colon == std::string::npos || colon < 1)
                    throw ConfigError(path, lineNo, "station_multiplex_listen needs host:port");
                c.multiplexHost = v.substr(0, colon);
                c.multiplexPort = intValue(v.substr(colon + 1));
            } else if (k == "signon_router_key") {
                std::string h = (v.size() >= 2 && v[0] == '0' && v[1] == 'x') ? v.substr(2) : v;
                c.consoleSignOnRouterKey = static_cast<int>(std::strtol(h.c_str(), nullptr, 16));
            } else if (k == "model") {
                if (!ModelTable::isKnown(v)) {
                    std::string names;
                    for (const std::string& m : ModelTable::known()) {
                        if (!names.empty()) names += ", ";
                        names += m;
                    }
                    throw ConfigError(path, lineNo, fmt::format(
                        "unknown model '{}'; known models are {}", v, names));
                }
                c.model = v;
            } else if (k == "csp_type") {
                if (!CspTypeTable::isKnown(v)) {
                    std::string names;
                    for (const std::string& t : CspTypeTable::known()) {
                        if (!names.empty()) names += ", ";
                        names += t;
                    }
                    throw ConfigError(path, lineNo, fmt::format(
                        "unknown CSP type '{}'; known types are {}", v, names));
                }
                c.cspType = v;
            } else if (k == "csp_model")
                throw ConfigError(path, lineNo,
                    "'csp_model' is obsolete: it conflated the machine model, the CSP "
                    "implementation and its variant. Use 'model' and 'csp_type'.");
            else throw ConfigError(path, lineNo, "unknown machine key '" + k + "'");
        } else if (tape != nullptr) {
            if (k == "folder" || k == "path") tape->path = v;
            else if (k == "readonly") tape->readOnly = parseBool(v);
            else throw ConfigError(path, lineNo, fmt::format(
                "unknown tape key '{}'; a [tape] section takes 'path' (or legacy 'folder') "
                "for media to mount at power-on and 'readonly' (yes|no). "
                "docs/s36/tape-operator.md", k));
        } else if (station != nullptr) {
            if (k == "role") {
                bool known = false;
                for (const std::string& r : StationConfig::roles()) if (equalsIgnoreCase(r, v)) known = true;
                if (!known)
                    throw ConfigError(path, lineNo, fmt::format(
                        "unknown role '{}'; a slot is one of {}. The role is not "
                        "cosmetic: it sets the device class in the configuration "
                        "record `82` reports (record byte 1 bits 0x30, which "
                        "Nudev5250::isaPrinter reads) and it decides which kind of "
                        "5250 client this endpoint will serve", v, "console, display, printer"));
                station->role = v;
            } else if (k == "device_code") { station->deviceCode = v; station->deviceCodeGiven = true; }
            else if (k == "signon_at_ipl") station->signOnAtIpl = parseBool(v);
            else if (k == "listen") parseListen(*station, v, path, lineNo);
            else if (k == "output") station->printerOutput = monitor::toLower(v);
            else if (k == "output_file") station->printerOutputPath = v;
            else throw ConfigError(path, lineNo, "unknown station key '" + k + "'");
        }
    }

    c.validate(path);
    return c;
}

void EmulatorConfig::validate(const std::string& path)
{
    if (volumePath.empty()) throw ConfigError(path, 0, "[machine] volume is required");

    if (!CspTypeTable::supportsModel(cspType, model))
        throw ConfigError(path, 0, fmt::format(
            "CSP type {} does not support machine model {}; advanced36 currently supports "
            "advanced36, 5363 and 5364", cspType, model));

    std::string type;
    if (!IplSourceTable::tryNormalizeType(iplType, type))
        throw ConfigError(path, 0, "IPL type must be attend/attended or unattend/unattended");
    iplType = type;

    // SC21-9052 page 2-16: "The system console must be placed at work
    // station address 0."  A machine with no console never finishes IPL.
    if (console() == nullptr)
        throw ConfigError(path, 0,
            "a console is required at station 0.0 - SC21-9052 requires the system console "
            "at work station address 0, and phase 3 blocks waiting for a signed-on station");

    for (const StationConfig& s : stations)
        if (s.isPrinter() && s.isConsole())
            throw ConfigError(path, 0,
                "station 0.0 cannot be a printer - SC21-9052 page 2-16 puts the system "
                "console at work station address 0, and MSIPL phase 3 waits for a "
                "signed-on terminal unit block");

    for (const StationConfig& s : stations) {
        if (!s.isPrinter()) continue;
        // The device code is REQUIRED for a printer.  SC21-9052 pages 2-18
        // and 2-19 give printers two-LETTER codes and displays numeric ones,
        // and a display's code on a printer's record reports the wrong
        // device family to SSP.
        if (!s.deviceCodeGiven)
            throw ConfigError(path, 0, fmt::format(
                "station {} is a printer and must state its device_code. There is no "
                "default: SC21-9052 2-18/2-19 gives printer device codes as two-letter "
                "codes while displays get numeric ones, and the code selects the device "
                "family byte SSP matches in record byte 1. Printer codes: {}",
                s.id(), devices::DeviceCodes::knownCodes(true)));
    }

    // Every station's device code has to be one SSP knows, because SSP
    // resolves it in a table of its own and a code that is not in it leaves
    // the station unmatched.
    for (const StationConfig& s : stations) {
        const bool validOutput = s.printerOutput == "tn5250" || s.printerOutput == "console" ||
                                 s.printerOutput == "file" || s.printerOutput == "txtout";
        if (!validOutput)
            throw ConfigError(path, 0, "station " + s.id() +
                " printer output must be tn5250, console, file, or txtout");
        if (!s.isPrinter() && (s.printerOutput != "tn5250" || !s.printerOutputPath.empty()))
            throw ConfigError(path, 0, "station " + s.id() + " is not a printer but has printer output configured");
        const bool pathOutput = s.printerOutput == "file" || s.printerOutput == "txtout";
        if (s.isPrinter() && pathOutput && s.printerOutputPath.empty())
            throw ConfigError(path, 0, "station " + s.id() + " " + s.printerOutput + " output needs a path");
        if (s.isPrinter() && !pathOutput && !s.printerOutputPath.empty())
            throw ConfigError(path, 0, "station " + s.id() + " has an output path but does not use file or txtout output");
        if (s.isPrinter() && s.printerOutput != "tn5250" && s.listenPort != 0)
            throw ConfigError(path, 0, "station " + s.id() +
                " cannot have both a local printer output and a TN5250 listener");
        devices::DeviceCodes::Entry e;
        if (!devices::DeviceCodes::tryLookup(s.deviceCode, e))
            throw ConfigError(path, 0, fmt::format(
                "station {} device_code = {} is not one SSP knows. MSIPL phase 2 "
                "matches record byte 1 and byte 3 against its own table of five-byte "
                "device signatures and takes everything else out of that table, so an "
                "unlisted code never resolves. Display codes: {}. Printer codes: {}",
                s.id(), s.deviceCode, devices::DeviceCodes::knownCodes(false),
                devices::DeviceCodes::knownCodes(true)));
        if (e.isPrinter() != s.isPrinter())
            throw ConfigError(path, 0, fmt::format(
                "station {} is a {} but device_code = {} is a {}'s - its device "
                "family byte {:02X} has (byte & 0x30) == {:02X}, which is what "
                "Nudev5250::isaPrinter (c185dac4) reads out of record byte 1",
                s.id(), s.isPrinter() ? "printer" : "display", s.deviceCode,
                e.isPrinter() ? "printer" : "display", e.family, e.family & 0x30));
    }

    std::set<std::string> seen;
    for (const StationConfig& s : stations)
        if (!seen.insert(s.id()).second) throw ConfigError(path, 0, "duplicate station " + s.id());

    std::set<int> ports;
    for (const StationConfig& s : stations)
        if (s.listenPort != 0 && !ports.insert(s.listenPort).second)
            throw ConfigError(path, 0, "two stations share TCP port " + std::to_string(s.listenPort));
}

}  // namespace sim36::configuration
