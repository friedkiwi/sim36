#include "Monitor/ConfigurationRenderer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <fmt/format.h>

#include "Monitor/CommandRegistry.h"

namespace sim36::monitor {

using configuration::EmulatorConfig;
using configuration::StationConfig;

namespace {

const char* onOff(bool v) { return v ? "on" : "off"; }
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
    if (!equalsIgnoreCase(c.cspType, c.model))
        line(fmt::format("  csp type               {} ({})", c.cspType,
                         configuration::cspKindName(c.cspKind())));
    line(fmt::format("  memory                 {}K [model maximum]", c.maxMainStorageKb()));
    line(fmt::format("  task work area         {} sectors", c.taskWorkAreaSectors));
    line(fmt::format("  IPL type               {}", c.iplType));
    line(fmt::format("  IPL source             {}", c.iplSourceName));

    line("media:");
    line(fmt::format("  disk0                  {}  {}", displayPath(c.volumePath), fixedDiskMode(c)));
    if (c.diskettePath.empty())
        line("  diskette0              (none)");
    else
        line(fmt::format("  diskette0              {}  {}", c.diskettePath,
                         c.disketteReadOnly ? "readonly" : "writable"));
    if (!c.tape || c.tape->path.empty())
        line("  tape0                  (none)");
    else
        line(fmt::format("  tape0                  {}  {}", c.tape->path,
                         c.tape->readOnly ? "readonly" : "writable"));

    line("terminal:");
    line(fmt::format("  multiplex              {}", onOff(c.stationMultiplex)));
    line(fmt::format("  multiplex listen       {}:{}", c.multiplexHost, c.multiplexPort));
    line(fmt::format("  auto-signon            {}", onOff(c.listenerAutoSignOn)));

    line("stations:");
    std::vector<StationConfig> stations = sorted(c);
    if (stations.empty()) {
        line("  (none explicitly defined; the default topology is applied at IPL)");
        return w;
    }
    for (const StationConfig& s : stations) {
        std::string description = fmt::format("  {:<3} role={} device-code={}",
                                              s.id(), s.role, s.deviceCode);
        if (s.isPrinter()) {
            description += " output=";
            description += (s.printerOutput == "file" || s.printerOutput == "txtout" || s.printerOutput == "pdfout")
                ? s.printerOutput + " " + s.printerOutputPath : s.printerOutput;
            if (s.printerOutput == "pdfout") description += " paper=" + s.printerPaper;
        }
        // The multiplexer replaces only display listeners.  Printer TN5250
        // endpoints remain active and should still be reported.
        if (!s.isConsole() && s.listenPort != 0 && (s.isPrinter() || !c.stationMultiplex))
            description += fmt::format(" listen={}:{}", s.listenHost, s.listenPort);
        line(description);
    }
    return w;
}

std::string ConfigurationRenderer::renderReplay(const EmulatorConfig& c)
{
    std::string w;
    auto line = [&](const std::string& s) { w += s; w += '\n'; };
    line(fmt::format("set machine model {}", quoteArgument(c.model)));
    line(fmt::format("set machine csp-type {}", quoteArgument(c.cspType)));
    line(fmt::format("set machine task-work-area-sectors {}", c.taskWorkAreaSectors));
    line(fmt::format("set machine host-model {}", quoteArgument(c.hostModel)));
    line(fmt::format("set machine host-processor-feature {:04X}", c.hostProcessorFeature));
    line(fmt::format("set machine host-processor-model {}", quoteArgument(c.hostProcessorModel)));
    line(fmt::format("set machine ipl-type {}", quoteArgument(c.iplType)));
    line(fmt::format("set machine ipl-source {}", quoteArgument(c.iplSourceName)));
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
        if (!c.tape->path.empty())
            line(fmt::format("attach tape0 {} {}", quoteArgument(absolutePath(c.tape->path)),
                             c.tape->readOnly ? "ro" : "rw"));
        else
            line(fmt::format("set tape0 {}", c.tape->readOnly ? "readonly" : "writable"));
    }

    for (const StationConfig& s : sorted(c)) {
        line(fmt::format("set station {} role {}", s.id(), quoteArgument(s.role)));
        line(fmt::format("set station {} device-code {}", s.id(), quoteArgument(s.deviceCode)));
        line(fmt::format("set station {} signon-at-ipl {}", s.id(), onOff(s.signOnAtIpl)));
        if (s.isPrinter()) {
            if (s.printerOutput == "file" || s.printerOutput == "txtout" || s.printerOutput == "pdfout")
                line(fmt::format("set station {} output {} {}", s.id(), s.printerOutput,
                                 quoteArgument(absolutePath(s.printerOutputPath))));
            else
                line(fmt::format("set station {} output {}", s.id(), s.printerOutput));
            if (s.printerPaper != "green")
                line(fmt::format("set station {} paper {}", s.id(), s.printerPaper));
        }
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
