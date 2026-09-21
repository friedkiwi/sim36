// The console display's field model, characterised by two real 5250 replies:
// a zero-length SBA that names an unchanged bypass field, and the
// DisplayWrite/36 CREATE DOCUMENT DESCRIPTION record whose field expansion
// must keep the document name right after the 12-byte SYSTEM field.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "Host/ConsoleDisplay.h"
#include "Storage/Ebcdic.h"

using namespace sim36;

namespace {

std::string slice(const std::vector<uint8_t>& b, std::size_t off, std::size_t len)
{
    if (off + len > b.size()) return "<short>";
    return storage::Ebcdic::toAscii(b.data() + off, static_cast<int>(len));
}

std::string roundTrip(const std::string& ascii)
{
    std::vector<uint8_t> e = storage::Ebcdic::fromAscii(ascii);
    return storage::Ebcdic::toAscii(e.data(), static_cast<int>(e.size()));
}

}  // namespace

TEST_CASE("console display: a real tn5250 zero-length SBA preserves the following User ID")
{
    host::ConsoleDisplay display(0, false);
    const std::vector<uint8_t> format = {
        // Bypass field at 4,56, initially blank.
        0x11, 0x04, 0x37, 0x1D, 0x68, 0x00, 0x20, 0x00, 0x04, 0x40, 0x40, 0x40, 0x40,
        // User ID at 6,56, initially blank.
        0x11, 0x06, 0x37, 0x1D, 0x48, 0x00, 0x20, 0x00, 0x08, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    };
    display.apply(format.data(), 0, static_cast<int>(format.size()));

    // Real tn5250 names the unchanged bypass field with an SBA carrying no
    // data, then names the modified User ID field and supplies its bytes.
    const std::vector<uint8_t> reply = {0x06, 0x3D, 0xF1, 0x11, 0x04, 0x38, 0x11, 0x06, 0x38,
                                        0xE8, 0xE5, 0xC1, 0xD5, 0xD1, 0x40, 0x40, 0x40};
    const std::vector<uint8_t> want = {0x06, 0x3D, 0xF1, 0x40, 0x40, 0x40, 0x40,
                                       0xE8, 0xE5, 0xC1, 0xD5, 0xD1, 0x40, 0x40, 0x40};
    CHECK(display.expandModifiedInput(reply) == want);

    // SF leaves its current write address at the first data cell and creates
    // an ending attribute; the following C1 must initialise the field itself,
    // despite the intervening FCW.
    host::ConsoleDisplay shaped(0, false);
    const std::vector<uint8_t> dwField = {0x04, 0x11, 0x00, 0x00, 0x11, 0x17, 0x02, 0x1D,
                                          0x58, 0x20, 0x82, 0x80, 0x20, 0x00, 0x3C, 0xC1};
    shaped.apply(dwField.data(), 0, static_cast<int>(dwField.size()));
    std::vector<uint8_t> first = shaped.buildInput(0xF1);
    REQUIRE(first.size() == 63);
    CHECK(first[3] == 0xC1);

    // Write Error Code is visible, but is an overlay and cannot become
    // input-field content in a later command-42 result.
    std::vector<uint8_t> kbd = storage::Ebcdic::fromAscii("KBD-0099");
    std::vector<uint8_t> wec = {0x04, 0x21};
    wec.insert(wec.end(), kbd.begin(), kbd.end());
    shaped.apply(wec.data(), 0, static_cast<int>(wec.size()));
    CHECK(shaped.renderText().find("KBD-0099") != std::string::npos);
    std::vector<uint8_t> afterError = shaped.buildInput(0xF1);
    REQUIRE(afterError.size() > 3);
    CHECK(afterError[3] == 0xC1);
}

TEST_CASE("console display: truncated modified fields are blank padded")
{
    host::ConsoleDisplay display(0, false);
    const std::vector<uint8_t> format = {
        0x04, 0x40,
        // HISTORY null-fills the panel before defining its input fields.
        0x11, 0x10, 0x01, 0x02, 0x18, 0x50, 0x00,
        // HISTORY's selected-user field is written as SYSTEM followed by
        // two terminal nulls.  The modified-data reply suppresses that tail.
        0x11, 0x10, 0x42, 0x1D, 0x48, 0x20, 0x30, 0x00, 0x08,
        0xE2, 0xE8, 0xE2, 0xE3, 0xC5, 0xD4, 0x00, 0x00,
    };
    display.apply(format.data(), 0, static_cast<int>(format.size()));

    const std::vector<uint8_t> reply = {
        0x10, 0x43, 0xF1, 0x11, 0x10, 0x43,
        0xE2, 0xE8, 0xE2, 0xE3, 0xC5, 0xD4,
    };
    const std::vector<uint8_t> expanded = display.expandModifiedInput(reply);
    REQUIRE(expanded.size() == 11);
    CHECK(slice(expanded, 3, 8) == roundTrip("SYSTEM  "));
}

TEST_CASE("console display: FSEDIT wraparound and empty MDT input fields")
{
    host::ConsoleDisplay display(0, false);
    const std::vector<uint8_t> format = {
        // The field attribute occupies row 1 column 80.  FSEDIT defines its
        // source-line fields this way so their first data cell is row 2
        // column 1.
        0x11, 0x01, 0x50,
        0x1D, 0x48, 0x20, 0x20, 0x00, 0x03,
        0xC1, 0xC2, 0xC3,
    };
    display.apply(format.data(), 0, static_cast<int>(format.size()));

    const std::vector<uint8_t> input = display.buildInput(0xF1);
    REQUIRE(input.size() == 6);
    CHECK(slice(input, 3, 3) == roundTrip("ABC"));

    // Unlike the zero-length bypass SBA above, an empty ordinary MDT field
    // means that the field itself is empty.  FSEDIT returns its option field
    // in precisely this form on Page Down.
    const std::vector<uint8_t> emptyReply = {0x02, 0x01, 0xF5, 0x11, 0x02, 0x01};
    const std::vector<uint8_t> expanded = display.expandModifiedInput(emptyReply);
    REQUIRE(expanded.size() == 6);
    CHECK(slice(expanded, 3, 3) == roundTrip("   "));
}

TEST_CASE("console display: omitted null-filled input fields are returned as blanks")
{
    host::ConsoleDisplay display(0, false);
    const std::vector<uint8_t> format = {
        0x04, 0x40,
        // CATALOG clears the screen with nulls, then defines a defaulted
        // selection field and a wholly empty output-file field.
        0x11, 0x08, 0x38, 0x1D, 0x48, 0x20, 0x30, 0x00, 0x05,
        0xC1, 0xD3, 0xD3, 0x00, 0x00,
        0x11, 0x10, 0x42, 0x1D, 0x48, 0x20, 0x30, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    display.apply(format.data(), 0, static_cast<int>(format.size()));

    // A real Read-MDT reply contains only the field the operator changed;
    // the empty output-file field is omitted altogether.
    const std::vector<uint8_t> reply = {
        0x08, 0x3A, 0xF1, 0x11, 0x08, 0x39, 0xC1, 0xD3, 0xD3,
    };
    const std::vector<uint8_t> expanded = display.expandModifiedInput(reply);
    REQUIRE(expanded.size() == 16);
    CHECK(slice(expanded, 3, 5) == roundTrip("ALL  "));
    CHECK(slice(expanded, 8, 8) == roundTrip("        "));
}

TEST_CASE("console display: DisplayWrite/36 CREATE honors SOH and FCW resequencing")
{
    host::ConsoleDisplay display(0, false);
    //   SYSTEM  r2 c45 ffw=6820 attr=22 len=12  (bypass profile field)
    //   name    r5 c30 ffw=4820 attr=30 len=12
    //   subject r6 c30 ffw=4800 attr=30 len=35
    std::vector<uint8_t> format = {0x11, 0x02, 0x2C, 0x1D, 0x68, 0x20, 0x22, 0x00, 0x0C};
    format.insert(format.end(), 12, 0x40);
    const std::vector<uint8_t> name = {0x11, 0x05, 0x1D, 0x1D, 0x48, 0x20, 0x30, 0x00, 0x0C};
    format.insert(format.end(), name.begin(), name.end());
    format.insert(format.end(), 12, 0x40);
    const std::vector<uint8_t> subject = {0x11, 0x06, 0x1D, 0x1D, 0x48, 0x00, 0x30, 0x00, 0x23};
    format.insert(format.end(), subject.begin(), subject.end());
    format.insert(format.end(), 35, 0x40);

    // Fields 4, 5 and 6.  SOH starts the return at field 5.  Field 6's
    // x'8001' FCW jumps back to field 1; fields 1-3 then proceed normally.
    // This is the chain used by CREATE DOCUMENT DESCRIPTION:
    //     5 -> 6 -> 1 -> 2 -> 3 -> 4 -> ...
    const std::vector<uint8_t> field4 = {0x11, 0x07, 0x1D, 0x1D, 0x48, 0x00, 0x80, 0x07, 0x30, 0x00, 0x19};
    format.insert(format.end(), field4.begin(), field4.end());
    format.insert(format.end(), 25, 0x40);
    const std::vector<uint8_t> field5 = {0x11, 0x08, 0x1D, 0x1D, 0x48, 0x20, 0x30, 0x00, 0x0C};
    format.insert(format.end(), field5.begin(), field5.end());
    format.insert(format.end(), 12, 0x40);
    const std::vector<uint8_t> field6 = {0x11, 0x09, 0x1D, 0x1D, 0x48, 0x20, 0x80, 0x01, 0x30, 0x00, 0x08};
    format.insert(format.end(), field6.begin(), field6.end());
    format.insert(format.end(), 8, 0x40);
    const std::vector<uint8_t> soh = {0x01, 0x03, 0x00, 0x00, 0x05};
    format.insert(format.end(), soh.begin(), soh.end());
    display.apply(format.data(), 0, static_cast<int>(format.size()));

    // The operator reply as a real 5250 client sends it, trailing blanks
    // suppressed.
    const std::vector<uint8_t> reply = {
        0x06, 0x33, 0xF1,                                                  // cursor, AID Enter
        0x11, 0x02, 0x2D, 0xE2, 0xE8, 0xE2, 0xE3, 0xC5, 0xD4,              // r2c45 SYSTEM
        0x11, 0x05, 0x1E, 0xC8, 0xC5, 0xD3, 0xD3, 0xD6,                    // r5c30 HELLO
        0x11, 0x06, 0x1E,                                                  // r6c30 subject:
        0xC8, 0x85, 0x93, 0x93, 0x96, 0x6B, 0x40, 0xA6, 0x96, 0x99, 0x93, 0x84, 0x40,
        0x84, 0x96, 0x83, 0xA4, 0x94, 0x85, 0x95, 0xA3,                    // "Hello, world document"
    };
    std::vector<uint8_t> rec = display.expandModifiedInput(reply);

    REQUIRE(rec.size() > 3);
    CHECK(rec[0] == 0x06);
    CHECK(rec[1] == 0x33);
    CHECK(rec[2] == 0xF1);
    CHECK(slice(rec, 23, 12) == roundTrip("SYSTEM      "));
    CHECK(slice(rec, 35, 12) == roundTrip("HELLO       "));
    const std::string subj = roundTrip("Hello, world document");
    CHECK(slice(rec, 47, subj.size()) == subj);
}
