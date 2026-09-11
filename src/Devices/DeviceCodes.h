// SC21-9052's device codes, and the two bytes of the six-byte work station
// configuration record that carry them.
//
// This table is SSP's own, read out of the IPL work-station configuration
// phase while it ran.  That phase carries two lists of five-byte device
// signatures, one for displays and one for printers, each entry followed by
// the EBCDIC device codes it serves and terminated by FF, and it matches an
// entry against a record with exactly two comparisons: entry byte 0 against
// record byte 1 (the device family) and entry byte 4 against record byte 3
// (the device type).  A device code is a label on a five-byte signature,
// several codes share one signature, and only two of the five bytes ever
// reach a record.  Every printer signature has (family & 0x30) == 0x20 and
// every display signature has 0x00.
#pragma once

#include <cstdint>
#include <string>

namespace sim36::devices {

class DeviceCodes {
public:
    struct Entry {
        uint8_t family = 0;   // record byte 1
        uint8_t type = 0;     // record byte 3
        bool isPrinter() const { return (family & 0x30) == 0x20; }
    };

    static bool tryLookup(const std::string& code, Entry& entry);

    // The codes, in table order, for an error message that has to tell an
    // operator what is allowed.
    static std::string knownCodes(bool printers);
};

}  // namespace sim36::devices
