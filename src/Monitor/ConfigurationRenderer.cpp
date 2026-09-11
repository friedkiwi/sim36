#include "Monitor/ConfigurationRenderer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <fmt/format.h>

namespace sim36::monitor {

using configuration::EmulatorConfig;
using configuration::StationConfig;

namespace {

const char* onOff(bool v) { return v ? "on" : "off"; }
std::string display(const std::string& v) { return v.empty() ? "(empty)" : v; }
std::string displayPath(const std::string& v) { return v.empty() ? "(none)" : v; }

std::string fixedDiskMode(const EmulatorConfig& c)
{
    return c.volumeOverlay ? "overlay" : c.volumeReadOnly ? "readonly" : "writable";
}

std::string absolutePath(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec) return path;
    return p.lexically_normal().string();
}

std::vector<StationConfig> sorted(const EmulatorConfig& c)
{
    std::vector<StationConfig> s = c.stations;
    std::stable_sort(s.begin(), s.end(), [](const StationConfig& a, const StationConfig& b) {
        return a.port != b.port ? a.port < b.port : a.address < b.address;
    });
    return s;
}

}  // namespace

std::string ConfigurationRenderer::renderHuman(const EmulatorConfig& c, bool latched)
{
    std::string w;
    auto line = [&](const std::string& s) { w += s; w += '\n'; };
    line(fmt::format("configuration {}", latched ? "latched" : "editable"));
    line("machine:");
    line(fmt::format("  model                  {}", c.model));
    line(fmt::format("  csp                    {} ({}) [derived]", configuration::cspKindName(c.cspKind()), c.cspVariant()));
    line(fmt::format("  memory                 {}K", c.mainStorageKb));
    line(fmt::format("  model memory ceiling   {}K [derived]", c.maxMainStorageKb()));
    line(fmt::format("  task work area         {} sectors", c.taskWorkAreaSectors));
    line(fmt::format("  host model             {}", display(c.hostModel)));
    line(fmt::format("  host processor feature {:04X}", c.hostProcessorFeature));
    line(fmt::format("  host processor model   {}", display(c.hostProcessorModel)));
    line(fmt::format("  IPL type               {}", c.iplType));
    line(fmt::format("  IPL source             {}", c.iplSourceName));

    line("session policy:");
    line(fmt::format("  listener auto-signon   {}", onOff(c.listenerAutoSignOn)));
    line(fmt::format("  signon use router      {}", onOff(c.consoleSignOnUseRouter)));
    line(fmt::format("  signon statement       {}", onOff(c.consoleSignOnStatement)));
    line(fmt::format("  signon request         {}", onOff(c.consoleSignOnRequest)));
    line(fmt::format("  signon router key      {:X}", c.consoleSignOnRouterKey));
    line(fmt::format("  workstation interactive {}", onOff(c.wsInteractive)));

    line("media:");
    line(fmt::format("  disk0                  {}  {}", displayPath(c.volumePath), fixedDiskMode(c)));
    line(fmt::format("  diskette0              {}  {}", displayPath(c.diskettePath),
                     c.disketteReadOnly ? "readonly" : "writable"));
    line(fmt::format("  tape0                  {}  {}", displayPath(c.tape ? c.tape->folderPath : ""),
                     !c.tape || !c.tape->readOnly ? "writable" : "readonly"));

    line("terminal:");
    line(fmt::format("  multiplex              {}", onOff(c.stationMultiplex)));
    line(fmt::format("  multiplex listen       {}:{}", c.multiplexHost, c.multiplexPort));

    line("stations:");
    std::vector<StationConfig> stations = sorted(c);
    if (stations.empty()) {
        line("  (none explicitly defined; the default topology is applied at IPL)");
        return w;
    }
    for (const StationConfig& s : stations) {
        std::string transport = s.isConsole() ? "operator"
            : s.listenPort == 0 ? "off"
            : s.listenHost + ":" + std::to_string(s.listenPort);
        line(fmt::format("  {:<3} role={} device-code={} signon-at-ipl={} listen={}",
                         s.id(), s.role, s.deviceCode, onOff(s.signOnAtIpl), transport));
    }
    return w;
}

std::string ConfigurationRenderer::renderReplay(const EmulatorConfig& c)
{
    std::string w;
    auto line = [&](const std::string& s) { w += s; w += '\n'; };
    line(fmt::format("set machine model {}", quoteArgument(c.model)));
    line(fmt::format("set machine memory {}K", c.mainStorageKb));
    line(fmt::format("set machine task-work-area-sectors {}", c.taskWorkAreaSectors));
    line(fmt::format("set machine host-model {}", quoteArgument(c.hostModel)));
    line(fmt::format("set machine host-processor-feature {:04X}", c.hostProcessorFeature));
    line(fmt::format("set machine host-processor-model {}", quoteArgument(c.hostProcessorModel)));
    line(fmt::format("set machine ipl-type {}", quoteArgument(c.iplType)));
    line(fmt::format("set machine ipl-source {}", quoteArgument(c.iplSourceName)));
    line(fmt::format("set machine load-source {}", quoteArgument(c.loadSourceName)));
    line(fmt::format("set machine listener-auto-signon {}", onOff(c.listenerAutoSignOn)));
    line(fmt::format("set machine signon-use-router {}", onOff(c.consoleSignOnUseRouter)));
    line(fmt::format("set machine signon-statement {}", onOff(c.consoleSignOnStatement)));
    line(fmt::format("set machine signon-request {}", onOff(c.consoleSignOnRequest)));
    line(fmt::format("set machine signon-router-key {:X}", c.consoleSignOnRouterKey));
    line(fmt::format("set machine ws-interactive {}", onOff(c.wsInteractive)));

    // Set the endpoint before enabling the listener: the opposite order
    // transiently binds the default port.
    line(fmt::format("set terminal multiplex listen {}",
                     quoteArgument(c.multiplexHost + ":" + std::to_string(c.multiplexPort))));
    line(fmt::format("set terminal multiplex {}", onOff(c.stationMultiplex)));

    const std::string diskMode = fixedDiskMode(c);
    if (!c.volumePath.empty())
        line(fmt::format("attach disk0 {} {}", quoteArgument(absolutePath(c.volumePath)), diskMode));
    else
        line(fmt::format("set disk0 {}", diskMode));

    if (!c.diskettePath.empty())
        line(fmt::format("attach diskette0 {} {}", quoteArgument(absolutePath(c.diskettePath)),
                         c.disketteReadOnly ? "ro" : "rw"));
    else
        line(fmt::format("set diskette0 {}", c.disketteReadOnly ? "readonly" : "writable"));

    if (c.tape) {
        if (!c.tape->folderPath.empty())
            line(fmt::format("attach tape0 {} {}", quoteArgument(absolutePath(c.tape->folderPath)),
                             c.tape->readOnly ? "ro" : "rw"));
        else
            line(fmt::format("set tape0 {}", c.tape->readOnly ? "readonly" : "writable"));
    }

    for (const StationConfig& s : sorted(c)) {
        line(fmt::format("set station {} role {}", s.id(), quoteArgument(s.role)));
        line(fmt::format("set station {} device-code {}", s.id(), quoteArgument(s.deviceCode)));
        line(fmt::format("set station {} signon-at-ipl {}", s.id(), onOff(s.signOnAtIpl)));
        if (s.listenPort != 0)
            line(fmt::format("set station {} listen {}", s.id(),
                             quoteArgument(s.listenHost + ":" + std::to_string(s.listenPort))));
    }
    return w;
}

std::string ConfigurationRenderer::quoteArgument(const std::string& value)
{
    for (char ch : value)
        if (ch == '\r' || ch == '\n' || ch == '\0')
            throw std::invalid_argument("configuration values cannot contain a newline or NUL character");
    bool quote = value.empty();
    for (char ch : value)
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';' || ch == '\'' || ch == '"' || ch == '\\')
            quote = true;
    if (!quote) return value;
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else out.push_back(ch);
    }
    out += "\"";
    return out;
}

}  // namespace sim36::monitor
