#include "Devices/DeviceCodes.h"

#include <cctype>
#include <vector>

namespace sim36::devices {

namespace {

struct Row { const char* code; uint8_t family; uint8_t type; };

// Transcribed from the two tables in the order the guest scans them, first
// occurrence winning, which is the entry SSP itself settles on because it
// scans forwards and stops at the first match.
const std::vector<Row>& rows()
{
    static const std::vector<Row> table = {
        // --- displays
        {"01", 0xC2, 0x00}, {"00", 0xC2, 0x00}, {"10", 0xC2, 0x80}, {"15", 0xC2, 0x10},
        {"11", 0xC2, 0x20},   // 3180 Model 2
        {"21", 0xC2, 0x40}, {"25", 0xC2, 0x40}, {"20", 0xC2, 0x40}, {"22", 0xC2, 0x41},
        {"16", 0xC2, 0x24}, {"26", 0xC2, 0x50}, {"19", 0xC2, 0x18},
        {"30", 0x02, 0x00}, {"31", 0x02, 0x00}, {"32", 0x02, 0x00}, {"34", 0x02, 0x00},
        {"35", 0x02, 0x00}, {"36", 0x02, 0x00},
        // --- printers
        {"PD", 0x2D, 0x11},   // 5219
        {"DA", 0x2D, 0x11},   // 5219 D01 / 3812
        {"DB", 0x2D, 0x11},
        {"AA", 0x20, 0x11},   // 5256 / 5262
        {"PB", 0x20, 0x11},
        {"AB", 0x60, 0x11}, {"AC", 0xA0, 0x11},
        {"CA", 0x28, 0x24},   // 5224 / 5225
        {"PC", 0x28, 0x24},
        {"BA", 0x24, 0x24}, {"BB", 0x64, 0x24}, {"CB", 0x68, 0x24}, {"CC", 0xA8, 0x24},
        {"CD", 0xE8, 0x24},
        {"EA", 0x2C, 0x11},   // 4214
        {"PG", 0x2C, 0x11},
        {"KA", 0x23, 0x24},   // 4234
        {"PK", 0x23, 0x24},
        {"PM", 0xA5, 0x24},   // 4245
        {"MA", 0xA5, 0x24}, {"MB", 0xA5, 0x24},
        {"BL", 0x26, 0x24}, {"PF", 0x26, 0x24}, {"CK", 0x2A, 0x24}, {"CL", 0x6A, 0x24},
        {"PE", 0x2E, 0x24}, {"GA", 0x2E, 0x24}, {"GB", 0x2E, 0x24}, {"GD", 0x2E, 0x24},
        {"GE", 0x2E, 0x24}, {"GF", 0x2E, 0x24}, {"SA", 0x2E, 0x24}, {"SB", 0x2E, 0x24},
        {"SC", 0x2E, 0x24}, {"SD", 0x2E, 0x24},
        {"PS", 0x2E, 0x24},   // 5227
        {"PH", 0x2B, 0x24},   // 4224 IPDS
        {"HA", 0x2B, 0x24}, {"HB", 0x2B, 0x24},
    };
    return table;
}

std::string trimUpper(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string r = s.substr(b, e - b);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

}  // namespace

bool DeviceCodes::tryLookup(const std::string& code, Entry& entry)
{
    entry = Entry{};
    if (code.empty()) return false;
    const std::string key = trimUpper(code);
    for (const Row& r : rows()) {
        if (key == r.code) {
            entry.family = r.family;
            entry.type = r.type;
            return true;
        }
    }
    return false;
}

std::string DeviceCodes::knownCodes(bool printers)
{
    std::string s;
    for (const Row& r : rows()) {
        Entry e{r.family, r.type};
        if (e.isPrinter() != printers) continue;
        if (!s.empty()) s.push_back(' ');
        s += r.code;
    }
    return s;
}

}  // namespace sim36::devices
