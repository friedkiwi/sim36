#include "Host/ConsoleDisplay.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "Storage/Ebcdic.h"

namespace sim36::host {

bool ConsoleField::covers(int r, int c) const
{
    int start = (row - 1) * ConsoleDisplay::kCols + (col - 1);
    int here = (r - 1) * ConsoleDisplay::kCols + (c - 1);
    int span = ConsoleDisplay::kRows * ConsoleDisplay::kCols;
    int delta = ((here - start) % span + span) % span;
    return delta < length;
}

ConsoleDisplay::ConsoleDisplay(int limit, bool writeLog)
    : screen_(static_cast<std::size_t>(kRows * kCols), 0x40), limit_(limit), writeLog_(writeLog)
{
    clearScreen();
}

std::string ConsoleDisplay::renderText() const
{
    std::string text;
    for (int row = 0; row < kRows; row++) {
        for (int col = 0; col < kCols; col++)
            text += row == kRows - 1 && col < static_cast<int>(operatorError_.size())
                        ? ebcdicChar(operatorError_[static_cast<std::size_t>(col)])
                        : ebcdicChar(screen_[static_cast<std::size_t>(row * kCols + col)]);
        text += '\n';
    }
    return text;
}

void ConsoleDisplay::clearScreen()
{
    std::fill(screen_.begin(), screen_.end(), 0x40);
    operatorError_.clear();
}

int ConsoleDisplay::offset(int row, int col)
{
    int p = (row - 1) * kCols + (col - 1);
    int span = kRows * kCols;
    return ((p % span) + span) % span;
}

void ConsoleDisplay::emit(const std::string& line)
{
    log_.push_back(line);
    while (static_cast<int>(log_.size()) > limit_) log_.erase(log_.begin());
    if (writeLog_) fmt::print("console: {}\n", line);
}

void ConsoleDisplay::apply(const uint8_t* data, int offset0, int length)
{
    int end = offset0 + length, i = offset0;
    std::string text;
    int textRow = row_, textCol = col_;

    auto flush = [&]() {
        if (text.empty()) return;
        bool blank = true;
        for (char c : text)
            if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
        if (!blank) emit(fmt::format("PUT {},{} '{}'", textRow, textCol, text));
        text.clear();
    };

    while (i < end) {
        uint8_t b = data[i];
        if (b == 0x04 && i + 1 < end) {   // ESC + command
            uint8_t cmd = data[i + 1];
            i += 2;
            if (cmd == 0x40 || cmd == 0x4F) {
                flush();
                emit("CLEAR");
                clearScreen();
                fields_.clear();
                row_ = col_ = 1;
            } else if (cmd == 0x50) {   // Clear Format Table
                flush();
                fields_.clear();
            } else if (cmd == 0x11 && i + 1 < end) {
                uint8_t cc2 = data[i + 1];
                i += 2;
                // CC2 unlock clears X-system/inhibit, restoring the message
                // line saved by Write Error Code.
                if ((cc2 & 0x08) != 0) operatorError_.clear();
            } else if (cmd == 0x21 || cmd == 0x22) {
                flush();
                if (cmd == 0x22 && i + 1 < end) i += 2;
                std::vector<uint8_t> message;
                while (i < end && data[i] != 0x04) {
                    // Under Write Error Code, IC changes the visible cursor
                    // only; it is not part of the message text.
                    if (data[i] == 0x13 && i + 2 < end) {
                        cursorRow_ = data[i + 1];
                        cursorCol_ = data[i + 2];
                        i += 3;
                    } else {
                        if (data[i] >= 0x40) message.push_back(data[i]);
                        i++;
                    }
                }
                // A terminal saves the message row and overlays it until
                // X-II is cleared.  It is not field data and must never enter
                // buildInput or expandModifiedInput.
                operatorError_ = message;
                emit(fmt::format("ERROR '{}'", storage::Ebcdic::toAscii(operatorError_.data(), operatorError_.size())));
            } else if (cmd == 0x23 && i + 2 < end) {   // Roll
                int direction = data[i++];
                int top = data[i++], bottom = data[i++];
                int lines = direction & 0x1F;
                if ((direction & 0x80) == 0) lines = -lines;
                if (lines != 0) {
                    int row = lines < 0 ? top : bottom;
                    int limit = lines < 0 ? bottom + 1 : top - 1;
                    int step = lines < 0 ? 1 : -1;
                    for (; row != limit; row += step) {
                        int target = row + lines;
                        if (row >= 0 && row < kRows && target >= top && target <= bottom && target >= 0 && target < kRows)
                            std::copy(screen_.begin() + row * kCols, screen_.begin() + (row + 1) * kCols,
                                      screen_.begin() + target * kCols);
                    }
                }
                emit(fmt::format("ROLL top={} bottom={} lines={}", top, bottom, lines));
            }
            continue;
        }
        if (b == 0x11 && i + 2 < end) {   // Set Buffer Address
            flush();
            row_ = data[i + 1];
            col_ = data[i + 2];
            textRow = row_;
            textCol = col_;
            i += 3;
            continue;
        }
        if (b == 0x13 && i + 2 < end) {   // Insert Cursor
            // IC carries the address it places the cursor at; it does not
            // move the buffer address.
            cursorRow_ = data[i + 1];
            cursorCol_ = data[i + 2];
            i += 3;
            continue;
        }
        if (b == 0x01 && i + 1 < end) {   // Start of Header
            int n = data[i + 1];
            i += 2 + n;
            continue;
        }
        if (b == 0x1D && i + 1 < end) {   // Start of Field
            flush();
            i += 1;
            // 5250 Start of Field: SF, optional Field Format Word (2),
            // optional Field Control Words (2 each), attribute (1), field
            // length (2).  The FFW is identified by bit 0x40 in its first
            // byte and an FCW by bit 0x80 in its first byte.  This is the
            // common encoding, not the whole order: an unusual FCW chain
            // would mis-frame the length.
            int ffw = -1;
            if (i + 1 < end && (data[i] & 0x40) != 0) {
                ffw = (data[i] << 8) | data[i + 1];   // FFW implies input-capable
                i += 2;
            }
            while (i + 1 < end && (data[i] & 0x80) != 0) i += 2;   // FCWs
            int attrRow = row_, attrCol = col_;
            uint8_t attr = 0;
            if (i < end) {
                attr = data[i];
                poke(attrRow, attrCol, attr);
                i += 1;
                col_++;
            }
            int len = 0;
            if (i + 1 < end) {
                len = (data[i] << 8) | data[i + 1];
                i += 2;
            }

            // The two length bytes describe the field; they occupy no screen
            // cell.  The field's content is whatever the guest writes next,
            // starting at the cell after the attribute, so the buffer
            // address must NOT be advanced past it here.
            ConsoleField f;
            f.row = attrRow;
            f.col = attrCol + 1;
            f.length = len;
            f.ffw = ffw;
            f.attribute = attr;
            f.mdt = ffw >= 0 && ((ffw >> 8) & 0x08) != 0;
            fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                         [&](const ConsoleField& x) { return x.row == f.row && x.col == f.col; }),
                          fields_.end());
            fields_.push_back(f);
            // A real controller writes the ending attribute after an input
            // field, then restores its current write address to the first
            // data cell.
            if (f.isInput()) {
                int endField = (offset(f.row, f.col) + f.length) % static_cast<int>(screen_.size());
                screen_[static_cast<std::size_t>(endField)] = 0x20;
            }
            textRow = row_;
            textCol = col_;
            emit(fmt::format("FIELD {},{} length: {}{}", f.row, f.col, len,
                             f.isInput() ? (f.bypass() ? " input bypass" : " input") : " protected"));
            continue;
        }
        if (b == 0x02 && i + 3 < end) {   // Repeat to Address
            flush();
            int toRow = data[i + 1], toCol = data[i + 2];
            uint8_t fill = data[i + 3];
            int from = offset(row_, col_), to = offset(toRow, toCol);
            for (int p = from;; p = (p + 1) % static_cast<int>(screen_.size())) {
                screen_[static_cast<std::size_t>(p)] = fill;
                if (p == to) break;
            }
            row_ = toRow;
            col_ = toCol;
            textRow = row_;
            textCol = col_;
            i += 4;
            continue;
        }
        if (b < 0x40) {
            i += 1;
            continue;
        }
        if (text.empty()) {
            textRow = row_;
            textCol = col_;
        }
        text += ebcdicChar(b);
        poke(row_, col_, b);
        if (++col_ > kCols) {
            col_ = 1;
            if (++row_ > kRows) row_ = 1;
        }
        i += 1;
    }
    flush();
}

// One EBCDIC byte as a printable character, using the same code page table
// the VTOC reader uses.
char ConsoleDisplay::ebcdicChar(uint8_t b)
{
    std::string s = storage::Ebcdic::toAscii(&b, 1);
    char ch = s.empty() ? ' ' : s[0];
    return ch < ' ' || ch > '~' ? ' ' : ch;
}

uint8_t ConsoleDisplay::ebcdicByte(char ch)
{
    std::vector<uint8_t> b = storage::Ebcdic::fromAscii(std::string(1, ch));
    return b.empty() ? static_cast<uint8_t>(0x40) : b[0];
}

ConsoleField* ConsoleDisplay::fieldAt(int row, int col)
{
    for (ConsoleField& f : fields_)
        if (f.isInput() && f.covers(row, col)) return &f;
    return nullptr;
}

std::vector<ConsoleField> ConsoleDisplay::inputFields() const
{
    std::vector<ConsoleField> list;
    for (const ConsoleField& f : fields_)
        if (f.isInput()) list.push_back(f);
    std::stable_sort(list.begin(), list.end(),
                     [](const ConsoleField& x, const ConsoleField& y) { return offset(x.row, x.col) < offset(y.row, y.col); });
    return list;
}

ConsoleField* ConsoleDisplay::typeInto(int row, int col, const std::string& text)
{
    ConsoleField* f = fieldAt(row, col);
    if (f == nullptr) return nullptr;
    f->pending = text;
    f->hasPending = true;
    f->mdt = true;
    // A display echoes keystrokes into its own buffer as they are typed, so
    // the value survives the transmission and answers the NEXT read too,
    // which is what `Press Enter to continue` depends on.
    int start = offset(f->row, f->col);
    for (int n = 0; n < f->length; n++)
        screen_[static_cast<std::size_t>((start + n) % static_cast<int>(screen_.size()))] =
            n < static_cast<int>(f->pending.size()) ? ebcdicByte(f->pending[static_cast<std::size_t>(n)])
                                                    : static_cast<uint8_t>(0x40);
    setCursor(row, col);
    return f;
}

// The bytes a field currently holds on the screen.
std::vector<uint8_t> ConsoleDisplay::screenContent(const ConsoleField& f) const
{
    std::vector<uint8_t> bytes(static_cast<std::size_t>(std::max(0, f.length)));
    int start = offset(f.row, f.col);
    for (int n = 0; n < f.length; n++)
        bytes[static_cast<std::size_t>(n)] = screen_[static_cast<std::size_t>((start + n) % static_cast<int>(screen_.size()))];
    return bytes;
}

std::vector<uint8_t> ConsoleDisplay::expandModifiedInput(const std::vector<uint8_t>& response)
{
    if (response.size() < 3) return response;
    bool sawSba = false;
    std::size_t i = 3;
    while (i + 2 < response.size() && response[i] == 0x11) {
        sawSba = true;
        int row = response[i + 1], col = response[i + 2];
        i += 3;
        ConsoleField* f = fieldAt(row, col);
        // A real Read-MDT reply can name an unchanged/bypass field with SBA
        // and ZERO following data bytes; the next byte is another SBA.  The
        // SBA orders delimit what the client actually supplied; omitted
        // bytes retain the display's existing screen content.
        std::size_t next = i;
        while (next < response.size() && response[next] != 0x11) next++;
        if (f != nullptr) {
            int take = std::min(f->length, static_cast<int>(next - i));
            int start = offset(f->row, f->col);
            for (int n = 0; n < take; n++)
                screen_[static_cast<std::size_t>((start + n) % static_cast<int>(screen_.size()))] =
                    response[i + static_cast<std::size_t>(n)];
        }
        i = next;
    }
    if (!sawSba) {
        // A Read-MDT answer with no newly modified fields is exactly
        // cursor+AID.  Command 42 still returns EVERY input field from the
        // retained format/screen state; zero wire field bytes does not mean
        // a zero-length input area.  Monitor console input is already in
        // the expanded positional shape, so only the three-byte wire form
        // needs expansion here.
        if (response.size() != 3) return response;
        std::vector<uint8_t> retained = {response[0], response[1], response[2]};
        for (const ConsoleField& f : inputFields()) {
            std::vector<uint8_t> c = screenContent(f);
            retained.insert(retained.end(), c.begin(), c.end());
        }
        return retained;
    }

    std::vector<uint8_t> result = {response[0], response[1], response[2]};
    for (const ConsoleField& f : inputFields()) {
        std::vector<uint8_t> c = screenContent(f);
        result.insert(result.end(), c.begin(), c.end());
    }
    return result;
}

std::vector<uint8_t> ConsoleDisplay::buildInput(uint8_t aid) const
{
    std::vector<uint8_t> bytes = {static_cast<uint8_t>(cursorRow_), static_cast<uint8_t>(cursorCol_), aid};
    for (const ConsoleField& f : inputFields()) {
        std::vector<uint8_t> c = screenContent(f);
        bytes.insert(bytes.end(), c.begin(), c.end());
    }
    return bytes;
}

void ConsoleDisplay::clearPending()
{
    for (ConsoleField& f : fields_) {
        f.pending.clear();
        f.hasPending = false;
        f.mdt = false;
    }
}

bool ConsoleDisplay::tryParseAid(const std::string& name, uint8_t& aid)
{
    aid = 0;
    if (name.empty()) return false;
    std::string n;
    for (char c : name) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (!n.empty() && std::isspace(static_cast<unsigned char>(n.front()))) n.erase(n.begin());
    while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back()))) n.pop_back();
    if (n == "ENTER") { aid = 0xF1; return true; }
    if (n == "CLEAR") { aid = 0xBD; return true; }
    if (n == "HELP") { aid = 0xF3; return true; }
    if (n == "ROLLUP") { aid = 0xF5; return true; }
    if (n == "ROLLDOWN") { aid = 0xF4; return true; }
    if (n.rfind("PF", 0) == 0 || n.rfind("F", 0) == 0) {
        std::string digits = n;
        while (!digits.empty() && (digits[0] == 'P' || digits[0] == 'F')) digits.erase(digits.begin());
        bool numeric = !digits.empty();
        for (char c : digits)
            if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
        if (numeric) {
            int k = std::stoi(digits);
            if (k >= 1 && k <= 24) {
                // PF1-12 are 0x31-0x3C, PF13-24 are 0xB1-0xBC.
                aid = k <= 12 ? static_cast<uint8_t>(0x30 + k) : static_cast<uint8_t>(0xB0 + (k - 12));
                return true;
            }
        }
    }
    return false;
}

}  // namespace sim36::host
