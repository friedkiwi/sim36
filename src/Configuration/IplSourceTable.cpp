#include "Configuration/IplSourceTable.h"

#include <cctype>
#include <cstdlib>

#include <fmt/format.h>

#include "Configuration/ConfigError.h"
#include "Monitor/CommandRegistry.h"

namespace sim36::configuration {

namespace {

struct Source { const char* name; int value; };

const std::vector<Source>& sources()
{
    static const std::vector<Source> table = {
        {"disk", 0x00},       // no reload; the source bits are never read
        {"diskette", 0x00},
        {"tape", 0x08},
        // The other two tape arms, named by their bit rather than by a guess.
        {"tape-04", 0x04},
        {"tape-10", 0x10},
    };
    return table;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool startsWith0x(const std::string& s)
{
    return s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

}  // namespace

bool IplSourceTable::requestsReload(const std::string& source)
{
    const std::string s = trim(source.empty() ? "disk" : source);
    if (startsWith0x(s)) return true;
    return !monitor::equalsIgnoreCase(s, "disk");
}

std::vector<std::string> IplSourceTable::names()
{
    std::vector<std::string> n;
    for (const Source& s : sources()) n.emplace_back(s.name);
    return n;
}

bool IplSourceTable::tryNormalizeType(const std::string& value, std::string& normalized)
{
    const std::string s = trim(value);
    if (monitor::equalsIgnoreCase(s, "attend") || monitor::equalsIgnoreCase(s, "attended")) {
        normalized = "attend";
        return true;
    }
    if (monitor::equalsIgnoreCase(s, "unattend") || monitor::equalsIgnoreCase(s, "unattended")) {
        normalized = "unattend";
        return true;
    }
    normalized.clear();
    return false;
}

int IplSourceTable::encode(const std::string& source, const std::string& iplType)
{
    int v = 0;
    const std::string s = trim(source.empty() ? "disk" : source);
    if (startsWith0x(s)) {
        v = static_cast<int>(std::strtol(s.c_str() + 2, nullptr, 16));
    } else {
        bool found = false;
        for (const Source& src : sources()) {
            if (monitor::equalsIgnoreCase(s, src.name)) { v = src.value; found = true; break; }
        }
        if (!found) {
            std::string known;
            for (const Source& src : sources()) {
                if (!known.empty()) known += ", ";
                known += src.name;
            }
            throw ConfigError("ipl_source", 0, fmt::format(
                "unknown ipl_source '{}'; known are {}, or a raw 0xNN byte. "
                "See docs/s36/ipl-source.md", s, known));
        }
    }
    std::string type;
    if (!tryNormalizeType(iplType, type))
        throw ConfigError("ipl_type", 0, fmt::format(
            "unknown IPL type '{}'; use attend/attended or unattend/unattended", iplType));
    if (type == "attend") v |= kAttendedFlag;
    return v & 0xFFFF;
}

std::string IplSourceTable::describe(const std::string& source, int word)
{
    if (!requestsReload(source))
        return "no reload requested - the source bits are not read on this path";
    const int b = word & 0xFF;
    std::string src = (b & kSourceMask) == 0
        ? std::string("DISKETTE (SVC 41)")
        : fmt::format("TAPE (SVC 46), arm {}", (b & 0x08) != 0 ? "08" : (b & 0x04) != 0 ? "04" : "10");
    return fmt::format("{}{}", src, (b & kAttendedFlag) != 0 ? ", attended flag on" : "");
}

}  // namespace sim36::configuration
