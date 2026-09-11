// Decodes a System/36 system reference code (SRC) into a device class and,
// where known, a meaning.  Decode only; it never invents a code.
//
// Two tables back it: IBM's own SRC index (the leading value names the
// failing subsystem) and the control processor self-test progress codes
// E880-E897 from SY31-9035.
#pragma once

#include <string>

namespace sim36::monitor {

class SrcCode {
public:
    static std::string deviceClass(int code);
    static std::string describe(int code);
};

}  // namespace sim36::monitor
