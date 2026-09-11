// EBCDIC conversion.  The volume is written in code page 500 throughout; the
// tape manifest uses code page 037.  Both are single-byte code pages whose
// 256 entries all map to Latin-1 code points, so decoding never fails and
// encoding substitutes '?' (0x6F) for anything outside the page, which is
// what the reference emulator's runtime encoder did.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sim36::storage {

class Ebcdic {
public:
    enum class CodePage { Cp500, Cp037 };

    static constexpr uint8_t kSubstitute = 0x6F;   // EBCDIC '?'

    static uint16_t toUnicode(uint8_t b, CodePage page = CodePage::Cp500);

    // Bytes to UTF-8 text.
    static std::string toAscii(const uint8_t* bytes, std::size_t length,
                               CodePage page = CodePage::Cp500);
    static std::string toAscii(const std::vector<uint8_t>& bytes, std::size_t offset,
                               std::size_t length, CodePage page = CodePage::Cp500);

    // UTF-8 text to bytes.
    static uint8_t fromChar(uint32_t codePoint, CodePage page = CodePage::Cp500);
    static std::vector<uint8_t> fromAscii(const std::string& utf8,
                                          CodePage page = CodePage::Cp500);
};

}  // namespace sim36::storage
