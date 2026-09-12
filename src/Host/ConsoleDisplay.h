// The console attachment's decoder and format table.
//
// On every 5250 machine work-station address 0.0 is the CONSOLE.  It is a
// terminal, and it has to be present for the IPL to complete.  The twinax /
// work-station bus is a generic device bus; what differs is the ATTACHMENT:
// a display is attached over telnet, and the console is attached to this
// emulator's own operator interface.
//
// This is deliberately NOT a 5250 renderer.  The console is silent almost
// all of the time, and the moment it is not is the moment something has gone
// badly wrong; what is useful then is a faithful log of the operations the
// guest issued, with their positions.  So each write decodes to lines like
//
//     console: PUT 6,56 'IPL SIGN ON'
//     console: CLEAR
//
// It DOES keep the one piece of display state a terminal cannot answer a
// read without: the FORMAT TABLE built from the guest's own Start-of-Field
// orders, and the screen image the fields draw their unmodified content
// from.
//
// THE RECORD CONTRACT.  The work station data manager treats the retained
// device record three ways by the SSP command it answers: for a Read MDT
// (0x22) it rewrites each SBA order into a screen offset, so the record is
// SBA + data per modified field; for 0x32 it lays down a header and a count;
// for Read Input Fields (0x42) it copies the retained record verbatim from
// +3.  The console sign-on read is command 0x42, so the record has to arrive
// already in the positional all-fields shape: cursor(2) + AID(1) + every
// input field's data concatenated in the order selected by SOH/FCWs, with no
// SBA orders.  That is what buildInput emits.  With resequencing disabled,
// the sign-on panel's fields are returned in normal screen order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim36::host {

// One entry of the display's format table, built from a Start-of-Field
// order.  row/col are 1-based and name the FIRST DATA position: the cell
// after the field's leading attribute byte.
struct ConsoleField {
    int row = 0, col = 0, length = 0;
    int ffw = -1;                 // -1 when the SF carried none
    int resequenceNext = -1;      // x'80nn' FCW, or -1 for normal successor
    uint8_t attribute = 0;
    bool mdt = false;             // Modify Data Tag
    bool hasPending = false;      // typed by `console put`, not yet sent
    std::string pending;

    // An SF with no Field Format Word only changes attributes; it defines
    // no input field and never appears in an input record.
    bool isInput() const { return ffw >= 0; }
    // FFW byte 0 bit x'20': keyboard entry bypassed (a badge reader field,
    // say).  A bypass field is still an INPUT field and is still transmitted
    // by a Read Input Fields, which is why the User ID lands at offset 9 and
    // not at offset 0.
    bool bypass() const { return ffw >= 0 && ((ffw >> 8) & 0x20) != 0; }
    // Attributes x'27' x'2F' x'37' x'3F' are non-display.
    bool nonDisplay() const { return (attribute & 0x07) == 0x07; }
    bool covers(int r, int c) const;
};

class ConsoleDisplay {
public:
    static constexpr int kRows = 24, kCols = 80;

    explicit ConsoleDisplay(int limit = 500, bool writeLog = true);

    int cursorRow() const { return cursorRow_; }
    int cursorCol() const { return cursorCol_; }
    // Whether decoded guest writes are echoed as `console:` lines.  A
    // TN5250-attached system console disables this presentation; the
    // retained display state is still updated for monitor commands and
    // diagnostics.
    bool writeLog() const { return writeLog_; }
    void setWriteLog(bool on) { writeLog_ = on; }
    const std::vector<std::string>& log() const { return log_; }
    int logCount() const { return static_cast<int>(log_.size()); }
    void clearLog() { log_.clear(); }
    std::vector<ConsoleField>& fields() { return fields_; }
    const std::vector<ConsoleField>& fields() const { return fields_; }

    // A non-mutating copy for crash diagnostics.
    std::vector<uint8_t> copyScreenBytes() const { return screen_; }
    // Render the retained 24x80 image without interpreting any additional
    // 5250 orders.  Diagnostic presentation only.
    std::string renderText() const;

    // Decode one outbound 5250 record into operation lines, and keep the
    // screen image and format table it builds.  Recognised: Clear Unit,
    // Clear Format Table, Set Buffer Address, Insert Cursor, Start of Field,
    // Repeat to Address, Roll, Write Error Code and EBCDIC text.  Anything
    // else is skipped rather than guessed at.
    void apply(const uint8_t* data, int offset, int length);
    void apply(const std::vector<uint8_t>& data) { apply(data.data(), 0, static_cast<int>(data.size())); }

    // Place the cursor, as `console put` does before typing.
    void setCursor(int row, int col) { cursorRow_ = row; cursorCol_ = col; }
    // The input field covering a screen position, or nullptr.
    ConsoleField* fieldAt(int row, int col);
    // The format table's input fields in the order selected by the SOH and
    // x'80nn' resequencing FCWs (screen order when resequencing is disabled).
    std::vector<ConsoleField> inputFields() const;
    // Type into the field covering (row, col), as an operator would.  The
    // value is held per field, so a session that fills User ID, Date and
    // Time transmits all three.  nullptr when no input field covers that
    // position.
    ConsoleField* typeInto(int row, int col, const std::string& text);
    // Merge a terminal Read-MDT reply into the device's screen image and
    // construct the later Read-Input-Fields result: cursor(2), AID(1), then
    // every input field contiguously, including keyboard-bypass fields and
    // honoring the format table's input-field resequencing chain.
    std::vector<uint8_t> expandModifiedInput(const std::vector<uint8_t>& response);
    // The 5250 input record the guest expects: cursor row, cursor column,
    // AID, then EVERY input field's data in format-table return order with no
    // SBA orders.
    std::vector<uint8_t> buildInput(uint8_t aid) const;
    // Drop the typed values and the MDTs after a record is sent, exactly as
    // a display does when it transmits.
    void clearPending();
    // AID codes an operator can send from the monitor.
    static bool tryParseAid(const std::string& name, uint8_t& aid);

private:
    void clearScreen();
    static int offset(int row, int col);
    void poke(int row, int col, uint8_t value) { screen_[static_cast<std::size_t>(offset(row, col))] = value; }
    void emit(const std::string& line);
    static char ebcdicChar(uint8_t b);
    static uint8_t ebcdicByte(char ch);
    std::vector<uint8_t> screenContent(const ConsoleField& f) const;

    int row_ = 1, col_ = 1;
    int cursorRow_ = 1, cursorCol_ = 1;
    int firstInputField_ = 0;     // SOH byte 3; zero disables resequencing
    std::vector<uint8_t> screen_;
    std::vector<uint8_t> operatorError_;
    std::vector<ConsoleField> fields_;
    std::vector<std::string> log_;
    int limit_;
    bool writeLog_;
};

}  // namespace sim36::host
