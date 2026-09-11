#include <doctest/doctest.h>

#include "Storage/ByteOrder.h"
#include "Storage/Ebcdic.h"

using sim36::storage::Ebcdic;

TEST_CASE("ebcdic: the alphabet and digits round-trip through cp500")
{
    const std::string text = "ABCXYZ abc xyz 0123456789 #@$.,-/";
    auto bytes = Ebcdic::fromAscii(text);
    REQUIRE(bytes.size() == text.size());
    CHECK(bytes[0] == 0xC1);   // 'A'
    CHECK(bytes[5] == 0xE9);   // 'Z'
    CHECK(bytes[6] == 0x40);   // ' '
    CHECK(bytes[15] == 0xF0);  // '0'
    CHECK(Ebcdic::toAscii(bytes.data(), bytes.size()) == text);
}

TEST_CASE("ebcdic: every byte round-trips through both code pages")
{
    for (auto page : {Ebcdic::CodePage::Cp500, Ebcdic::CodePage::Cp037}) {
        for (int b = 0; b < 256; ++b) {
            uint8_t byte = static_cast<uint8_t>(b);
            std::string text = Ebcdic::toAscii(&byte, 1, page);
            auto back = Ebcdic::fromAscii(text, page);
            REQUIRE(back.size() == 1);
            CHECK(back[0] == byte);
        }
    }
}

TEST_CASE("ebcdic: cp500 and cp037 differ where IBM says they do")
{
    // '[' is 0x4A in cp500 and 0xBA in cp037; '!' is 0x4F in cp500, 0x5A in cp037.
    CHECK(Ebcdic::fromAscii("[")[0] == 0x4A);
    CHECK(Ebcdic::fromAscii("[", Ebcdic::CodePage::Cp037)[0] == 0xBA);
    CHECK(Ebcdic::fromAscii("!")[0] == 0x4F);
    CHECK(Ebcdic::fromAscii("!", Ebcdic::CodePage::Cp037)[0] == 0x5A);
}

TEST_CASE("ebcdic: characters outside the page become '?'")
{
    auto bytes = Ebcdic::fromAscii("a\xE2\x82\xAC" "b");   // a € b
    REQUIRE(bytes.size() == 3);
    CHECK(bytes[1] == Ebcdic::kSubstitute);
}

TEST_CASE("byte order: big-endian helpers")
{
    uint8_t buf[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK(sim36::storage::be16(buf) == 0x1234);
    CHECK(sim36::storage::be24(buf) == 0x123456u);
    CHECK(sim36::storage::be32(buf) == 0x12345678u);
    sim36::storage::putBe24(buf, 0xABCDEF);
    CHECK(buf[0] == 0xAB);
    CHECK(buf[2] == 0xEF);
    CHECK(buf[3] == 0x78);
}
