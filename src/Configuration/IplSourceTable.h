// The IPL-source selection, encoded as the low byte of control storage
// direct area word 1074, the word phase 1 reads as its fourth instruction and
// copies to guest 0x08A9.
//
// The same operator selection also tells the control processor where to get
// the initial 4 KB phase 1.  Phase 1 only reaches the reload dispatch below
// when a removable source was selected (guest 0x08B2 bit 0x08).  A normal disk
// IPL never consults this byte.  `disk`, the default, is therefore encoded as
// no reload request rather than as a value of this field.
//
// The dispatch, at guest 0x146A:
//
//   146A  TBF  q=1C $08A9   ; all of bits 1C off?
//   146E  JC   q=90 54      ; NO - some bit on -> 14C5, which reaches SVC 46, TAPE
//   1471  BC   q=87 $17A9   ; YES - all off      -> 17A9, which reaches SVC 41, DISKETTE
#pragma once

#include <string>
#include <vector>

namespace sim36::configuration {

class IplSourceTable {
public:
    // Bits 0x1C of the low byte select the reload source; 0x08 first, then
    // 0x04, and 0x10 only when neither of those is on.
    static constexpr int kSourceMask = 0x1C;

    // Bit 0x80 is a flag above the source field.  Binding it to
    // attended/unattended is emulator POLICY, not a recovered fact: the bit
    // exists, sits above the source field, and is forwarded to guest 0x08A8
    // bit 0x08 for a later phase to read.
    static constexpr int kAttendedFlag = 0x80;

    // Whether this selection asks phase 1 to reload the system library from
    // media.  Everything except `disk` does.
    static bool requestsReload(const std::string& source);

    static std::vector<std::string> names();

    // Canonicalises attend/attended and unattend/unattended; false for
    // anything else, so an unknown string never silently selects unattended.
    static bool tryNormalizeType(const std::string& value, std::string& normalized);

    // The byte the panel hands the machine.  Throws ConfigError.
    static int encode(const std::string& source, const std::string& iplType);

    // What the machine will do with the byte, for the trace and the monitor.
    static std::string describe(const std::string& source, int word);
};

}  // namespace sim36::configuration
