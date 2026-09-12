#!/usr/bin/env python3
"""tn5250drive - a scriptable, headless 5250 display station for SIM/36.

TEST-ONLY.  Nothing here is imported by the emulator; it changes no emulator
behaviour.  It is a *client*: it speaks RFC 1205 / RFC 2877 over a socket to a
station listener exactly as a real 5250 display would, and it keeps the screen
image and format table that a real display keeps.

WHY THIS EXISTS
---------------
``test/tn5250-capture.py`` proves records cross the wire but has no screen
model, so driving a panel means hand-computing SBA addresses and FFW field
lengths out of a hex dump.  This module gives the session a 24x80 buffer, a
field table derived from the guest's own Start-of-Field orders, and
label-addressed input, so a test can say:

    session = Session(2300, name="W1")
    session.connect()
    session.wait_for_text("SIGN ON", timeout=60)
    session.type_into("User ID", "YVANJ")
    session.press("Enter")
    session.wait_for_change(timeout=15)
    print(session.screen.render())

WHAT IT MODELS
--------------
Output (guest -> display), inside a WTD command:
    SOH 01, RA 02, EA 03, TD 10, SBA 11, WEA 12, IC 13, MC 14, WDSF 15, SF 1D
Commands (after ESC 04):
    40 Clear Unit, 4F Clear Unit Alternate, 50 Clear Format Table,
    11 Write To Display, 21/22 Write Error Code, 02/03 Save, 12/13 Restore,
    23 Roll, F3 Write Structured Field,
    42 Read Input Fields, 52 Read MDT Fields, 82 Read MDT Alternate,
    62 Read Screen Immediate, 72 Read Immediate.

Input (display -> guest): cursor address, AID byte, and then either every input
field in screen order (answering Read Input Fields) or SBA+data for each
modified field (answering Read MDT Fields).  Which one is used is decided by
the read command the guest actually issued, not by a guess.

TIMING
------
There is a known emulator defect - docs/s36/ordering-sensitivity-is-an-ipl-stall
-2026-09-07.md - that makes the W7 "IPL is in progress" Put/Get need an answer
within a fraction of a second.  ``arm_auto_answer()`` answers from the reader
thread, in the same call that parses the record, so the reply leaves before the
window closes.  That is scaffolding around the defect, not a feature.

No monitor command (wsinput/wsioch/wswrite) is ever issued from here: every byte
this module sends is a real terminal response to a real guest invitation.
"""

import argparse
import copy
import re
import socket
import sys
import threading
import time

# ---------------------------------------------------------------- telnet ----

IAC, DONT, DO, WONT, WILL, SB, SE, EOR = 255, 254, 253, 252, 251, 250, 240, 239
OPT_BINARY, OPT_EOR, OPT_TTYPE, OPT_NEWENV = 0, 25, 24, 39

# ------------------------------------------------------------ data stream ---

ESC = 0x04

CMD_NAMES = {
    0x02: "SaveScreen", 0x03: "SavePartialScreen",
    0x11: "WriteToDisplay", 0x12: "RestoreScreen", 0x13: "RestorePartialScreen",
    0x21: "WriteErrorCode", 0x22: "WriteErrorCodeToWindow", 0x23: "Roll",
    0x40: "ClearUnit", 0x4F: "ClearUnitAlternate", 0x50: "ClearFormatTable",
    0x42: "ReadInputFields", 0x52: "ReadMdtFields", 0x82: "ReadMdtFieldsAlt",
    0x62: "ReadScreenImmediate", 0x66: "ReadScreenPrint", 0x72: "ReadImmediate",
    0xF3: "WriteStructuredField",
}

# Commands that invite the display to answer with an input record.
READ_COMMANDS = {0x42, 0x52, 0x82, 0x62, 0x72}
# Reads whose response carries every input field, with no SBA orders.
READ_ALL_FIELDS = {0x42}

ORDER_NAMES = {
    0x01: "SOH", 0x02: "RA", 0x03: "EA", 0x10: "TD", 0x11: "SBA",
    0x12: "WEA", 0x13: "IC", 0x14: "MC", 0x15: "WDSF", 0x1D: "SF",
}

AID = {
    "ENTER": 0xF1, "HELP": 0xF3, "ROLLDOWN": 0xF4, "PAGEUP": 0xF4,
    "ROLLUP": 0xF5, "PAGEDOWN": 0xF5, "PRINT": 0xF6, "RECORDBACKSPACE": 0xF8,
    "CLEAR": 0xBD, "AUTOENTER": 0x3F,
}
for _n in range(1, 13):
    AID["F%d" % _n] = 0x30 + _n            # F1..F12 -> 0x31..0x3C
    AID["CMD%d" % _n] = 0x30 + _n
for _n in range(13, 25):
    AID["F%d" % _n] = 0xB0 + (_n - 12)     # F13..F24 -> 0xB1..0xBC
    AID["CMD%d" % _n] = 0xB0 + (_n - 12)

ROWS, COLS = 24, 80
BLANK = 0x40                                # EBCDIC space


def aid_code(name):
    """Resolve 'Enter', 'F3', 'cmd7', or a raw int/0xNN string to an AID byte."""
    if isinstance(name, int):
        return name
    key = str(name).strip().upper().replace(" ", "").replace("-", "")
    if key in AID:
        return AID[key]
    if key.startswith("PF"):
        key = "F" + key[2:]
        if key in AID:
            return AID[key]
    try:
        return int(str(name), 0)
    except ValueError:
        raise KeyError("unknown AID %r" % (name,))


def to_ebcdic(text):
    return text.encode("cp037")


def to_ascii(data):
    return data.decode("cp037", errors="replace")


# ----------------------------------------------------------------- model ----

class Field(object):
    """One entry of the display's format table, built from an SF order.

    ``row``/``col`` are 1-based and name the FIRST DATA position, i.e. the
    position after the field's leading attribute byte, which is what an SBA in
    an input record must point at.
    """

    def __init__(self, row, col, length, attr, ffw, fcws, index):
        self.row, self.col, self.length = row, col, length
        self.attr, self.ffw, self.fcws = attr, ffw, fcws
        self.index = index
        self.mdt = bool(ffw is not None and (ffw >> 8) & 0x08)
        self.label = ""
        self._pending = None                # locally typed value, EBCDIC bytes

    # -- attributes derived from the FFW ------------------------------------
    @property
    def is_input(self):
        """An SF with no FFW only changes attributes; it defines no input."""
        return self.ffw is not None and not self.bypass

    @property
    def bypass(self):
        return self.ffw is not None and bool((self.ffw >> 8) & 0x20)

    @property
    def protected(self):
        return not self.is_input

    @property
    def nondisplay(self):
        """Attributes x'27' x'2F' x'37' x'3F' are non-display (password)."""
        return (self.attr & 0x07) == 0x07

    @property
    def shift(self):
        return 0 if self.ffw is None else (self.ffw >> 8) & 0x07

    @property
    def numeric_only(self):
        return self.shift in (3, 5, 7)

    def positions(self):
        """Yield (row, col) for each data position, wrapping at the margin."""
        offset = (self.row - 1) * COLS + (self.col - 1)
        for i in range(self.length):
            p = (offset + i) % (ROWS * COLS)
            yield p // COLS + 1, p % COLS + 1

    def __repr__(self):
        kind = "input" if self.is_input else ("bypass" if self.bypass else "out")
        return "<Field #%d %s r%02d c%02d len=%d attr=%02X %s%s>" % (
            self.index, kind, self.row, self.col, self.length, self.attr,
            "nondisp " if self.nondisplay else "",
            "label=%r" % self.label if self.label else "")


class Screen(object):
    """A 24x80 display image plus its format table."""

    def __init__(self):
        self.buf = bytearray([BLANK] * (ROWS * COLS))
        self.attr_pos = set()               # offsets occupied by attribute bytes
        self.fields = []
        self.cursor = (1, 1)                # 1-based (row, col)
        self.last_command = None
        self.last_read = None
        self.alarm = False
        self.error = ""
        self.keyboard_unlocked = False

    # -- raw buffer ---------------------------------------------------------
    def _off(self, row, col):
        return ((row - 1) % ROWS) * COLS + ((col - 1) % COLS)

    def clear(self):
        self.buf = bytearray([BLANK] * (ROWS * COLS))
        self.attr_pos = set()
        self.fields = []
        self.cursor = (1, 1)
        self.error = b""
        self.keyboard_unlocked = False

    def clear_format_table(self):
        self.fields = []

    def put(self, off, byte):
        self.buf[off % (ROWS * COLS)] = byte

    # -- reading ------------------------------------------------------------
    def row_text(self, row):
        start = (row - 1) * COLS
        out = []
        for i in range(start, start + COLS):
            b = self.buf[i]
            # Anything below x'40' on a 5250 buffer is a field attribute or a
            # control, not text: it occupies a position and displays as blank.
            out.append(" " if (b < 0x40 or b == 0xFF or i in self.attr_pos)
                       else to_ascii(bytes([b])))
        # Write Error Code uses the terminal's operator-message overlay.  It
        # temporarily covers the configured message row; it is not written
        # into the display buffer that Read Input Fields subsequently returns.
        if row == ROWS and self.error:
            message = to_ascii(self.error)
            out[:min(COLS, len(message))] = message[:COLS]
        return "".join(out)

    def text(self):
        return "\n".join(self.row_text(r) for r in range(1, ROWS + 1))

    def render(self, title=None, fields=False):
        bar = "     +" + "-" * COLS + "+"
        head = "          " + "".join(str((c // 10) % 10) or " "
                                      for c in range(1, COLS + 1))
        ruler = "          " + "".join(str(c % 10) for c in range(1, COLS + 1))
        lines = []
        if title:
            lines.append(title)
        lines += [head, ruler, bar]
        for r in range(1, ROWS + 1):
            lines.append("%4d |%s|" % (r, self.row_text(r)))
        lines.append(bar)
        lines.append("     cursor r%d c%d   last command: %s" % (
            self.cursor[0], self.cursor[1],
            CMD_NAMES.get(self.last_command, self.last_command)))
        if fields:
            lines.append(self.field_table())
        return "\n".join(lines)

    def field_table(self):
        out = ["     field table (%d entries)" % len(self.fields),
               "     %-3s %-6s %-4s %-4s %-4s %-6s %-5s %-24s %s"
               % ("#", "kind", "row", "col", "len", "attr", "ffw", "label", "value")]
        for f in self.fields:
            out.append("     %-3d %-6s %-4d %-4d %-4d %-6s %-5s %-24s %r" % (
                f.index,
                "input" if f.is_input else ("bypass" if f.bypass else "out"),
                f.row, f.col, f.length, "%02X" % f.attr,
                "----" if f.ffw is None else "%04X" % f.ffw,
                f.label[:24], self.field_value(f)))
        return "\n".join(out)

    def field_value(self, field):
        if field._pending is not None:
            return to_ascii(field._pending)
        data = bytes(self.buf[self._off(r, c)] for r, c in field.positions())
        return to_ascii(data.replace(b"\x00", b"\x40"))

    def find(self, needle, ignore_case=True):
        """Return (row, col) of the first occurrence of ``needle``, or None."""
        hay = needle.upper() if ignore_case else needle
        for r in range(1, ROWS + 1):
            line = self.row_text(r)
            if ignore_case:
                line = line.upper()
            c = line.find(hay)
            if c >= 0:
                return (r, c + 1)
        return None

    def contains(self, needle, ignore_case=True):
        return self.find(needle, ignore_case) is not None

    # -- fields -------------------------------------------------------------
    def add_field(self, field):
        # Re-writing a panel re-issues its SFs; replace rather than duplicate.
        self.fields = [f for f in self.fields
                       if not (f.row == field.row and f.col == field.col)]
        self.fields.append(field)
        self.fields.sort(key=lambda f: (f.row, f.col))
        for i, f in enumerate(self.fields):
            f.index = i

    def input_fields(self):
        return [f for f in self.fields if f.is_input]

    def derive_labels(self):
        """Give each input field the caption that precedes it on its row.

        The caption is the protected text between the end of the previous field
        on that row (or column 1) and this field's attribute byte, with the
        SSP-style leader dots and punctuation trimmed off.  That is what a human
        reads as the field's name, so it is what a script should be able to
        address it by.
        """
        by_row = {}
        for f in self.fields:
            by_row.setdefault(f.row, []).append(f)
        for row, fields in by_row.items():
            line = self.row_text(row)
            fields.sort(key=lambda f: f.col)
            prev_end = 0
            for f in fields:
                start = prev_end
                stop = max(start, f.col - 2)        # -1 for 1-based, -1 for attr
                text = line[start:stop]
                f.label = self._clean_label(text)
                prev_end = min(COLS, f.col - 1 + f.length)
            # A field with no caption to its left may still be captioned by the
            # nearest text above it in the same column band; only used as a
            # fallback so ordinary panels keep their real captions.
            for f in fields:
                if not f.label:
                    f.label = self._label_from_above(f)

    def _label_from_above(self, field):
        for r in range(field.row - 1, 0, -1):
            line = self.row_text(r)
            seg = line[max(0, field.col - 6):min(COLS, field.col + field.length + 4)]
            cleaned = self._clean_label(seg)
            if cleaned:
                return cleaned
        return ""

    LEADER = re.compile(r"(?:\.\s*){3,}")

    @classmethod
    def _clean_label(cls, text):
        text = text.replace("\x00", " ")
        # SSP captions its fields as "User ID  . . . . . . . ." and may then put
        # a value hint ("Y,N") between the leader and the field.  The caption is
        # everything before the leader.
        leader = cls.LEADER.search(text)
        if leader:
            text = text[:leader.start()]
        text = text.rstrip()
        while text and text[-1] in ".:· ":
            text = text[:-1]
        text = text.strip()
        if not leader and "  " in text:
            # No leader: a row may hold a caption plus another field's residue,
            # and the phrase nearest the field is the one that names it.
            text = text.rsplit("  ", 1)[-1].strip()
        return text

    def field_by_label(self, label, occurrence=0, input_only=True):
        want = label.strip().upper()
        pool = self.input_fields() if input_only else self.fields
        exact = [f for f in pool if f.label.upper() == want]
        loose = [f for f in pool if want in f.label.upper()]
        hits = exact or loose
        if not hits:
            raise LookupError(
                "no %sfield labelled %r; labels seen: %s"
                % ("input " if input_only else "", label,
                   ", ".join(repr(f.label) for f in pool)))
        return hits[occurrence]

    def field_at(self, row, col):
        for f in self.fields:
            for r, c in f.positions():
                if (r, c) == (row, col):
                    return f
        return None


# ----------------------------------------------------------------- parser ---

class Parser(object):
    """Apply one guest 5250 output record to a Screen."""

    def __init__(self, screen, trace=None):
        self.screen = screen
        self.trace = trace if trace is not None else []

    def _log(self, text):
        self.trace.append(text)

    def apply(self, body):
        s = self.screen
        i, n = 0, len(body)
        off = s._off(*s.cursor)
        while i < n:
            b = body[i]
            if b == ESC and i + 1 < n:
                cmd = body[i + 1]
                i += 2
                s.last_command = cmd
                self._log("ESC %s (04 %02X)" % (CMD_NAMES.get(cmd, "%02X" % cmd), cmd))
                if cmd in (0x40, 0x4F):
                    s.clear()
                    off = 0
                elif cmd == 0x50:
                    s.clear_format_table()
                elif cmd == 0x11:               # Write To Display
                    if i + 1 < n:
                        cc1, cc2 = body[i], body[i + 1]
                        i += 2
                        s.alarm = bool(cc2 & 0x04)
                        if cc2 & 0x08:            # clear X-system/inhibit
                            s.error = b""
                            s.keyboard_unlocked = True
                        self._log("  CC1=%02X CC2=%02X" % (cc1, cc2))
                elif cmd in (0x21, 0x22):       # Write Error Code (+window)
                    if cmd == 0x22:
                        i += 2                  # window start/end addresses
                    # The error line is a saved overlay, not ordinary screen
                    # data.  lib5250 session.c:733..795 saves the message row,
                    # displays the error there, then restores it when X-II is
                    # cleared.  Keeping it out of ``buf`` is also essential:
                    # that buffer supplies later Read-Input-Fields replies.
                    message = bytearray()
                    while i < n and body[i] != ESC:
                        if body[i] == 0x13 and i + 2 < n:  # IC moves cursor only
                            s.cursor = (max(1, body[i + 1]), max(1, body[i + 2]))
                            i += 3
                        else:
                            if body[i] >= 0x40:
                                message.append(body[i])
                            i += 1
                    s.error = bytes(message)
                    s.keyboard_unlocked = False
                    self._log("  operator error %r" % to_ascii(s.error))
                elif cmd == 0x23 and i + 2 < n:  # Roll
                    direction, top, bottom = body[i], body[i + 1], body[i + 2]
                    i += 3
                    lines = direction & 0x1f
                    if not direction & 0x80:
                        lines = -lines
                    if lines:
                        # lib5250 dbuffer.c:869..903 uses zero-based row
                        # indices and deliberately leaves the exposed rows for
                        # the following WTD to fill.
                        rows = range(top, bottom + 1) if lines < 0 \
                            else range(bottom, top - 1, -1)
                        for row in rows:
                            target = row + lines
                            if top <= target <= bottom and 0 <= row < ROWS:
                                src = row * COLS
                                dst = target * COLS
                                s.buf[dst:dst + COLS] = s.buf[src:src + COLS]
                    self._log("  roll top=%d bottom=%d lines=%d" %
                              (top, bottom, lines))
                elif cmd in READ_COMMANDS:
                    if cmd in (0x42, 0x52, 0x82) and i + 1 < n:
                        self._log("  read CC1=%02X CC2=%02X" % (body[i], body[i + 1]))
                        i += 2
                    s.last_read = cmd
                elif cmd == 0xF3:               # Write Structured Field
                    break                       # structured fields end the record
                continue

            if b in ORDER_NAMES:
                name = ORDER_NAMES[b]
                if b == 0x11 and i + 2 < n:                     # SBA
                    row, col = body[i + 1], body[i + 2]
                    i += 3
                    off = s._off(max(1, row), max(1, col))
                    self._log("SBA r%d c%d" % (row, col))
                    continue
                if b == 0x13 and i + 2 < n:                     # IC
                    s.cursor = (max(1, body[i + 1]), max(1, body[i + 2]))
                    self._log("IC r%d c%d" % s.cursor)
                    i += 3
                    continue
                if b == 0x14 and i + 2 < n:                     # MC
                    s.cursor = (max(1, body[i + 1]), max(1, body[i + 2]))
                    i += 3
                    continue
                if b == 0x02 and i + 3 < n:                     # RA
                    row, col, ch = body[i + 1], body[i + 2], body[i + 3]
                    i += 4
                    target = s._off(max(1, row), max(1, col))
                    guard = 0
                    while off != target and guard <= ROWS * COLS:
                        s.put(off, ch)
                        off = (off + 1) % (ROWS * COLS)
                        guard += 1
                    s.put(off, ch)
                    off = (off + 1) % (ROWS * COLS)
                    self._log("RA to r%d c%d char %02X" % (row, col, ch))
                    continue
                if b == 0x03 and i + 3 < n:                     # EA
                    row, col, count = body[i + 1], body[i + 2], body[i + 3]
                    i += 4
                    i += max(0, count - 1)      # attribute-type list
                    target = s._off(max(1, row), max(1, col))
                    guard = 0
                    while off != target and guard <= ROWS * COLS:
                        s.put(off, BLANK)
                        s.attr_pos.discard(off)
                        off = (off + 1) % (ROWS * COLS)
                        guard += 1
                    self._log("EA to r%d c%d" % (row, col))
                    continue
                if b == 0x10 and i + 2 < n:                     # TD
                    ln = (body[i + 1] << 8) | body[i + 2]
                    i += 3
                    for k in range(ln):
                        if i + k < n:
                            s.put(off, body[i + k])
                            off = (off + 1) % (ROWS * COLS)
                    i += ln
                    continue
                if b == 0x12:                                   # WEA
                    i += 3
                    continue
                if b == 0x15 and i + 2 < n:                     # WDSF
                    ln = (body[i + 1] << 8) | body[i + 2]
                    i += max(3, ln + 1)
                    continue
                if b == 0x01 and i + 1 < n:                     # SOH
                    ln = body[i + 1]
                    i += 2 + ln
                    self._log("SOH %d byte(s)" % ln)
                    continue
                if b == 0x1D:                                   # SF
                    i, off = self._start_of_field(body, i + 1, off)
                    continue
                self._log("unhandled order %s (%02X)" % (name, b))
                i += 1
                continue

            s.put(off, b)
            s.attr_pos.discard(off)
            off = (off + 1) % (ROWS * COLS)
            i += 1

        s.derive_labels()
        return self.trace

    def _start_of_field(self, body, i, off):
        """SF: [FFW][FCW...] attr len-hi len-lo.

        Control words are consumed in pairs until a byte whose top three bits
        are 001 - the field attribute - is reached (this is lib5250's rule and
        matches the 5494 data-stream description).
        """
        s = self.screen
        n = len(body)
        ffw, fcws = None, []
        first = True
        while i < n and (body[i] & 0xE0) != 0x20:
            word = (body[i] << 8) | (body[i + 1] if i + 1 < n else 0)
            if first:
                ffw = word
                first = False
            else:
                fcws.append(word)
            i += 2
        if i >= n:
            return n, off
        attr = body[i]
        i += 1
        length = 0
        if i + 1 < n:
            length = (body[i] << 8) | body[i + 1]
            i += 2
        # The attribute byte occupies the current position; the field data
        # starts at the next one.
        s.put(off, attr)
        s.attr_pos.add(off)
        off = (off + 1) % (ROWS * COLS)
        row, col = off // COLS + 1, off % COLS + 1
        field = Field(row, col, length, attr, ffw, fcws, len(s.fields))
        s.add_field(field)
        self._log("SF r%d c%d len=%d attr=%02X ffw=%s" % (
            row, col, length, attr, "----" if ffw is None else "%04X" % ffw))
        # A 5250 controller creates the ending attribute at the cell after the
        # input field, but restores the current write address to the first data
        # cell.  Therefore bytes immediately following SF initialize the field;
        # they do not begin after it.  This is the exact cursor sequence in
        # lib5250 session.c:1759..1805.
        if ffw is not None:
            end = (off + length) % (ROWS * COLS)
            s.put(end, 0x20)
            s.attr_pos.add(end)
        return i, off


# ---------------------------------------------------------------- session ---

HOST = ["127.0.0.1"]


class Session(object):
    """A negotiated 5250 station with a live screen model."""

    def __init__(self, port, host="127.0.0.1", name=None,
                 device_type="IBM-3179-2", device_name=None, verbose=False):
        self.port, self.host = port, host
        self.name = name or ("port%d" % port)
        self.device_type = device_type
        # RFC 2877 section 4's DEVNAME USERVAR.  A 5250 server that multiplexes
        # devices onto one port uses it to skip its own selection screen; the
        # emulator's station multiplexer does exactly that.
        self.device_name = device_name
        self.verbose = verbose
        self.sock = None
        self.screen = Screen()
        self.records = []                   # raw logical records, guest -> here
        self.sent = []                      # raw logical records, here -> guest
        self.trace = []
        self.lock = threading.RLock()
        self.negotiated = threading.Event()
        self.invited = threading.Event()    # a read command is outstanding
        self.updated = threading.Event()
        self.generation = 0                 # bumped on every applied record
        self._auto_aid = None
        self._auto_limit = 0
        self._auto_count = 0
        self._stop = False
        self._reader = None
        self._pending = b""
        self._saved_screen = None

    # -- lifecycle ----------------------------------------------------------
    def connect(self, timeout=20.0, negotiate_timeout=25.0, retries=3):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection((self.host, self.port), 1.0)
                break
            except (ConnectionRefusedError, socket.timeout, OSError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("%s: no listener on %s:%d"
                                       % (self.name, self.host, self.port))
                time.sleep(0.1)
        self.sock.settimeout(0.2)
        self._reader = threading.Thread(target=self._run, daemon=True)
        self._reader.start()
        if not self.negotiated.wait(negotiate_timeout):
            # A listener that accepts but does not negotiate means we arrived
            # while the emulator was still building the station.  Drop the
            # socket and try again rather than failing the whole run.
            self.close()
            if retries > 0:
                self._stop = False
                self.negotiated.clear()
                self._pending = b""
                return self.connect(timeout, negotiate_timeout, retries - 1)
            raise RuntimeError("%s: 5250 negotiation did not complete" % self.name)
        return self

    def close(self):
        self._stop = True
        if self._reader:
            self._reader.join(timeout=2)
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- reader -------------------------------------------------------------
    def _run(self):
        while not self._stop:
            try:
                data = self.sock.recv(8192)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            self._pending += data
            self._pending = self._demux(self._pending)

    def _demux(self, data):
        """Telnet split: verbs, IAC IAC undoubling, IAC EOR record boundaries."""
        i, cur = 0, b""
        while i < len(data):
            c = data[i]
            if c == IAC:
                if i + 1 >= len(data):
                    return data[i:]
                nxt = data[i + 1]
                if nxt == IAC:
                    cur += b"\xff"
                    i += 2
                    continue
                if nxt == EOR:
                    self._record(cur)
                    cur = b""
                    i += 2
                    continue
                if nxt in (DO, DONT, WILL, WONT):
                    if i + 2 >= len(data):
                        return data[i:]
                    self._verb(nxt, data[i + 2])
                    i += 3
                    continue
                if nxt == SB:
                    end = data.find(bytes([IAC, SE]), i)
                    if end < 0:
                        return data[i:]
                    self._subneg(data[i + 2:end])
                    i = end + 2
                    continue
                i += 2
                continue
            cur += bytes([c])
            i += 1
        return cur

    def _verb(self, verb, opt):
        if verb == DO:
            ans = WILL if opt in (OPT_BINARY, OPT_EOR, OPT_TTYPE, OPT_NEWENV) else WONT
            self.sock.sendall(bytes([IAC, ans, opt]))
        elif verb == WILL:
            ans = DO if opt in (OPT_BINARY, OPT_EOR) else DONT
            self.sock.sendall(bytes([IAC, ans, opt]))

    def _subneg(self, body):
        if not body:
            return
        opt = body[0]
        if opt == OPT_TTYPE and len(body) > 1 and body[1] == 1:
            self.sock.sendall(bytes([IAC, SB, OPT_TTYPE, 0])
                              + self.device_type.encode("ascii")
                              + bytes([IAC, SE]))
        elif opt == OPT_NEWENV and len(body) > 1 and body[1] == 1:
            # RFC 1572 section 2 encoding: USERVAR name VALUE value.  Sent only
            # when the caller asked for a device name; an empty IS is the
            # conforming answer otherwise, and is what this always used to send.
            payload = bytes([IAC, SB, OPT_NEWENV, 0])
            if self.device_name:
                payload += (bytes([3]) + b"DEVNAME" + bytes([1])
                            + self.device_name.encode("ascii"))
            payload += bytes([IAC, SE])
            self.sock.sendall(payload)
        self.negotiated.set()

    # OS/400 asks the device to identify itself with a Write Structured Field
    # carrying a 5250 Query (`04 F3 00 05 D9 70 00`).  A client that does not
    # answer is left sitting on the sign-on screen forever - which is exactly
    # where this driver used to stall.
    #
    # These bytes are TRANSCRIBED from the stock tn5250 client answering a real
    # OS/400 (captured through a TCP tap), not composed from a specification.  A
    # wrong Query Reply is worse than none, so it is copied verbatim.  It
    # describes an IBM-3179-2, which is why `device_type` defaults to that: the
    # terminal type we advertise and the one this reply claims must agree, and we
    # have not observed a reply for any other type.
    QUERY = bytes.fromhex("04f30005d97000")
    QUERY_REPLY = bytes.fromhex(
        "004712a0000004000000000088003ad970800600010100000000000000000000"
        "0000000000000001f3f1f7f900f0f202000000615000ffffffff000000233100"
        "000000000000000000")

    def _is_query(self, body):
        return self.QUERY in body

    def _answer_query(self):
        self.sock.sendall(self.QUERY_REPLY + bytes([IAC, EOR]))
        self.trace.append("[%s] answered the 5250 Query (%d bytes)"
                          % (self.name, len(self.QUERY_REPLY)))

    def _answer_save_screen(self):
        """Return the terminal's current 24x80 image to a Save Screen request.

        RFC 1205 makes the image opaque to the host: SSP stores these bytes and
        later gives them back in an opcode-05 Restore Screen record.  Retain a
        full local model and serialize a self-contained WTD stream in the same
        form as libtn5250's tn5250_wtd_context_convert_nosrc().  Keeping this in
        the client is important--the emulator cannot manufacture or reinterpret
        a terminal-owned image.
        """
        self._saved_screen = copy.deepcopy(self.screen)
        image = self._saved_screen_wtd()
        record, wire = self.frame(0x04, image)
        self.send_record(record, wire)
        self.trace.append("[%s] answered Save Screen with %d image bytes"
                          % (self.name, len(image)))

    def _saved_screen_wtd(self):
        """Encode the current display like libtn5250 ``wtd.c``.

        This deliberately mirrors the reference implementation's no-source
        conversion rather than choosing a private compact representation.  In
        particular, runs of five or more bytes become Repeat-to-Address orders
        and field attribute cells become Start-of-Field orders.  SSP later
        replays these bytes without knowing their meaning.
        """
        s = self.screen
        row, col = s.cursor
        out = bytearray([ESC, 0x12, ESC, 0x40, ESC, 0x11, 0x00, 0x00,
                         0x13, row, col])

        # libtn5250 detects a field while visiting the attribute cell: the
        # field itself starts at the following display position.
        fields = {}
        for field in s.fields:
            data_off = s._off(field.row, field.col)
            if data_off != 0:               # wtd.c does not wrap this lookup
                fields[data_off - 1] = field

        run_char = None
        run_count = 0

        def flush(next_off):
            nonlocal run_char, run_count
            if not run_count:
                return
            if run_count <= 4 and not (run_count == 3 and run_char == 0x00):
                out.extend([run_char] * run_count)
            elif run_char == 0x00:
                # Clear Unit already made this run NUL; move the buffer address
                # to the first cell after it.  A final run is intentionally not
                # flushed below, exactly as wtd.c behaves.
                out.extend([0x11, next_off // COLS + 1,
                            next_off % COLS + 1])
            else:
                target = next_off - 1
                out.extend([0x02, target // COLS + 1,
                            target % COLS + 1, run_char])
            run_char = None
            run_count = 0

        for off, char in enumerate(s.buf):
            field = fields.get(off)
            if field is not None:
                flush(off)
                out.append(0x1D)             # Start of Field
                if field.ffw is not None:
                    ffw = field.ffw | (0x0800 if field.mdt else 0)
                    out.extend([ffw >> 8, ffw & 0xFF])
                for fcw in field.fcws:
                    out.extend([fcw >> 8, fcw & 0xFF])
                attr = char if (char & 0xE0) == 0x20 else field.attr
                out.extend([attr, field.length >> 8, field.length & 0xFF])
                continue
            if run_char != char:
                flush(off)
                run_char = char
            run_count += 1

        # tn5250's converter leaves a trailing run unwritten after Clear Unit;
        # those cells already have the required value.  The session appends any
        # read command only after destroying the conversion context.
        if s.last_read is not None:
            out.extend([ESC, s.last_read, 0x00, 0x00])
        return bytes(out)

    def _restore_screen(self, body):
        """Apply the opaque image previously returned by this terminal."""
        if self._saved_screen is not None:
            self.screen = copy.deepcopy(self._saved_screen)
            self.trace.append("[%s] restored saved terminal screen" % self.name)
        else:
            self.trace.append("[%s] Restore Screen arrived without a saved image"
                              % self.name)

    def _record(self, record):
        if not record:
            return
        if self._is_query(record):
            self._answer_query()
            return
        self.negotiated.set()
        with self.lock:
            self.records.append(record)
            if len(record) < 10:
                return
            opcode = record[9]
            body = record[10:]
            self.trace.append("[%s] record %d: len=%d opcode=%02X"
                              % (self.name, len(self.records) - 1, len(record), opcode))
            save_request = opcode == 0x04 and body == b"\x04\x02"
            restore_request = (opcode == 0x05 and
                               body.startswith(b"\x04\x12"))
            if save_request:
                # Save Screen is a command to the terminal, not display data.
                # Answer after releasing the model lock below.
                pass
            elif restore_request:
                self._restore_screen(body)
            else:
                Parser(self.screen, self.trace).apply(body)
            self.generation += 1
            self.updated.set()
            if not (save_request or restore_request) and (
                    self.screen.last_command in READ_COMMANDS or
                    self.screen.keyboard_unlocked):
                self.invited.set()
            auto = self._auto_aid
        if self.verbose:
            print(self.screen.render("[%s] record %d" % (self.name,
                                                         len(self.records) - 1)))
        if save_request:
            self._answer_save_screen()
        # Answer inside the reader callback: the emulator's IPL stall makes the
        # W7 Put/Get window a fraction of a second wide.
        if auto is not None and self.screen.last_command in READ_COMMANDS:
            if self._auto_limit == 0 or self._auto_count < self._auto_limit:
                self._auto_count += 1
                try:
                    self.press(auto, wait_invite=0)
                except OSError:
                    pass

    # -- waiting ------------------------------------------------------------
    def wait_for_text(self, text, timeout=30.0, poll=0.05):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if self.screen.contains(text):
                    return True
            time.sleep(poll)
        raise TimeoutError("%s: %r never appeared. Screen was:\n%s"
                           % (self.name, text, self.screen.text()))

    def wait_for_invite(self, timeout=30.0):
        if not self.invited.wait(timeout):
            raise TimeoutError("%s: the guest never invited input" % self.name)
        return True

    def wait_for_change(self, timeout=30.0, since=None, poll=0.05):
        mark = self.generation if since is None else since
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.generation != mark:
                return True
            time.sleep(poll)
        raise TimeoutError("%s: the screen did not change" % self.name)

    def settle(self, quiet=0.6, timeout=10.0):
        """Wait until no new record has arrived for ``quiet`` seconds."""
        deadline = time.monotonic() + timeout
        last = self.generation
        stamp = time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(0.05)
            if self.generation != last:
                last = self.generation
                stamp = time.monotonic()
            elif time.monotonic() - stamp >= quiet:
                return True
        return False

    # -- typing -------------------------------------------------------------
    def type_into(self, label, text, occurrence=0):
        """Type into the input field whose caption matches ``label``."""
        with self.lock:
            field = self.screen.field_by_label(label, occurrence)
            return self._type(field, text)

    def type_at(self, row, col, text):
        with self.lock:
            field = self.screen.field_at(row, col)
            if field is None:
                raise LookupError("no field covers r%d c%d" % (row, col))
            return self._type(field, text)

    def type_field(self, index, text):
        with self.lock:
            return self._type(self.screen.fields[index], text)

    def _type(self, field, text):
        if not field.is_input:
            raise ValueError("%r is not an input field" % (field,))
        data = to_ebcdic(text)
        if len(data) > field.length:
            raise ValueError("%r characters do not fit %r" % (text, field))
        if field.numeric_only:
            # Numeric-only fields are entered right-adjusted, blank filled.
            data = to_ebcdic(text.rjust(field.length))
        else:
            data = data + bytes([BLANK]) * (field.length - len(data))
        field._pending = data
        field.mdt = True
        # Reflect it locally so render()/find() show what the operator sees.
        for (r, c), b in zip(field.positions(), data):
            self.screen.put(self.screen._off(r, c), b)
        self.screen.cursor = (field.row, field.col)
        return field

    def clear_field(self, label, occurrence=0):
        return self.type_into(label, "", occurrence)

    # -- sending ------------------------------------------------------------
    def build_input_record(self, aid, cursor=None):
        """RFC 1205 input record: cursor address, AID, then the field data.

        Read Input Fields is answered with every input field, in screen order,
        with no SBA orders.  Read MDT Fields is answered with SBA + data for
        each modified field only.  The guest's own read command decides which.
        """
        s = self.screen
        row, col = cursor or s.cursor
        body = bytearray([row & 0xFF, col & 0xFF, aid_code(aid)])
        read = s.last_read
        if read in READ_ALL_FIELDS:
            for f in s.input_fields():
                body += (f._pending if f._pending is not None
                         else bytes(s.buf[s._off(r, c)] for r, c in f.positions()))
        elif read in (0x62, 0x72):
            pass                            # Read Immediate / Screen: AID only
        else:
            for f in s.input_fields():
                if not f.mdt:
                    continue
                data = (f._pending if f._pending is not None
                        else bytes(s.buf[s._off(r, c)] for r, c in f.positions()))
                body += bytes([0x11, f.row & 0xFF, f.col & 0xFF]) + data
        return self.frame(0x00, bytes(body))

    @staticmethod
    def frame(opcode, body):
        """Wrap a 5250 body in the RFC 1205 logical record header."""
        length = 10 + len(body)
        record = bytes([length >> 8, length & 0xFF, 0x12, 0xA0,
                        0x00, 0x00, 0x04, 0x00, 0x00, opcode]) + body
        wire = record.replace(b"\xff", b"\xff\xff") + bytes([IAC, EOR])
        return record, wire

    def send_record(self, record, wire):
        self.sock.sendall(wire)
        self.sent.append(record)
        self.trace.append("[%s] sent %d bytes: %s"
                          % (self.name, len(record), record.hex()))
        return record

    def press(self, aid="Enter", cursor=None, wait_invite=15.0):
        """Send one AID with the modified fields. Only answers a real invite."""
        if wait_invite:
            self.wait_for_invite(wait_invite)
        with self.lock:
            record, wire = self.build_input_record(aid, cursor)
            self.invited.clear()
            self.screen.keyboard_unlocked = False
            self.send_record(record, wire)
            # libtn5250 session.c ends the outstanding read after transmitting
            # its input record.  A later Save Screen must not resurrect it.
            self.screen.last_read = None
            for f in self.screen.input_fields():
                f._pending = None
                f.mdt = False
        return record

    def arm_auto_answer(self, aid="Enter", limit=0):
        """Answer invited Put/Gets from the reader thread, immediately.

        ``limit`` of 0 means every invite.  This exists only because of the
        emulator's IPL-stall timing defect; see the module docstring.
        """
        self._auto_aid = aid
        self._auto_limit = limit
        self._auto_count = 0
        return self

    def disarm_auto_answer(self):
        self._auto_aid = None


# ------------------------------------------------------------- script CLI ---

HELP = """\
tn5250drive script commands (one per line; # starts a comment):

  connect <port> [name] [devname]
                             attach and negotiate a station; devname is the
                             RFC 2877 DEVNAME the station multiplexer reads
  station <name>             make a station current
  auto <AID> [limit]         answer invited Put/Gets instantly (IPL-stall aid)
  noauto                     stop answering automatically
  wait <text> [timeout]      wait until the screen contains <text>
  waitinvite [timeout]       wait until the guest invites input
  settle [quiet] [timeout]   wait until output stops arriving
  type <label> = <value>     type into the field with that caption
  typeat <row> <col> <value> type into the field covering that position
  press <AID>                Enter / F1..F24 / Help / Clear / RollUp ...
  expect <text>              fail unless the screen contains <text>
  screen                     print the screen with a ruler
  fields                     print the derived field table
  sleep <seconds>
"""


def run_script(lines, sessions=None, out=sys.stdout):
    sessions = sessions if sessions is not None else {}
    current = [None]

    def cur():
        if current[0] is None:
            raise RuntimeError("no station selected; use 'connect' first")
        return sessions[current[0]]

    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        verb = parts[0].lower()
        if verb == "connect":
            port = int(parts[1])
            name = parts[2] if len(parts) > 2 else "port%d" % port
            devname = parts[3] if len(parts) > 3 else None
            sessions[name] = Session(port, host=HOST[0], name=name,
                                     device_name=devname).connect()
            current[0] = name
            print("connected %s on port %d" % (name, port), file=out)
        elif verb == "station":
            current[0] = parts[1]
        elif verb == "auto":
            cur().arm_auto_answer(parts[1] if len(parts) > 1 else "Enter",
                                  int(parts[2]) if len(parts) > 2 else 0)
        elif verb == "noauto":
            cur().disarm_auto_answer()
        elif verb == "wait":
            rest = line[len("wait"):].strip()
            timeout = 30.0
            bits = rest.rsplit(" ", 1)
            if len(bits) == 2:
                try:
                    timeout = float(bits[1])
                    rest = bits[0]
                except ValueError:
                    pass
            cur().wait_for_text(rest.strip('"'), timeout)
        elif verb == "waitinvite":
            cur().wait_for_invite(float(parts[1]) if len(parts) > 1 else 30.0)
        elif verb == "settle":
            cur().settle(float(parts[1]) if len(parts) > 1 else 0.6,
                         float(parts[2]) if len(parts) > 2 else 10.0)
        elif verb == "type":
            label, _, value = line[len("type"):].partition("=")
            cur().type_into(label.strip(), value.strip().strip('"'))
        elif verb == "typeat":
            cur().type_at(int(parts[1]), int(parts[2]),
                          line.split(None, 3)[3].strip('"'))
        elif verb == "press":
            cur().press(parts[1] if len(parts) > 1 else "Enter")
        elif verb == "expect":
            want = line[len("expect"):].strip().strip('"')
            if not cur().screen.contains(want):
                print(cur().screen.render(), file=out)
                raise SystemExit("expect failed: %r not on screen" % want)
            print("expect ok: %r" % want, file=out)
        elif verb == "screen":
            print(cur().screen.render("=== %s ===" % current[0]), file=out)
        elif verb == "fields":
            print(cur().screen.field_table(), file=out)
        elif verb == "sleep":
            time.sleep(float(parts[1]))
        else:
            raise SystemExit("unknown script command %r" % verb)
    return sessions


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Drive a 5250 station against a running SIM/36.",
        epilog=HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--script", help="script file, or - for stdin")
    ap.add_argument("--port", type=int, help="attach this station and dump it")
    ap.add_argument("--wait", help="with --port: wait for this text first")
    ap.add_argument("--timeout", type=float, default=30.0)
    # The driver was written against the emulator's own listeners, which are
    # always on loopback.  A host makes it usable against a REAL machine too -
    # SLONKY's Advanced/36 answers 5250 on port 23 - which is the only way to
    # reach commands OS/400 marks *INTERACT only, STRM36PRC and WRKM36 among
    # them, and the route to running code inside a real SSP guest.
    ap.add_argument("--host", default="127.0.0.1",
                    help="host to connect to (default 127.0.0.1)")
    args = ap.parse_args(argv)
    HOST[0] = args.host

    if args.script:
        lines = (sys.stdin.readlines() if args.script == "-"
                 else open(args.script).read().splitlines())
        sessions = run_script(lines)
        for s in sessions.values():
            s.close()
        return 0
    if args.port:
        s = Session(args.port, host=args.host,
                    name="port%d" % args.port).connect()
        if args.wait:
            s.wait_for_text(args.wait, args.timeout)
        else:
            s.settle(1.0, args.timeout)
        print(s.screen.render("=== port %d ===" % args.port, fields=True))
        s.close()
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
