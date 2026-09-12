#!/bin/sh
# The printer seam, as a pass/fail test.
#
# Two halves, and they check different things.
#
# The FIRST half runs with nothing attached, like test/workstation-seam.sh: it
# checks that a printer slot is declared to the guest, that `SVC 42` Output Data
# reaches the printer backend rather than the display one, and that the
# configuration refuses the things it should refuse.
#
# The SECOND half attaches a minimal 5250 printer client - fifty lines of
# Python, not `lp5250d` - and asserts the bytes on the wire against RFC 2877's
# own printed figures: the section 9 startup response record, the section 10
# figure 4 print record and the section 10.3 figure 6 null print record. That is
# the part that is evidence. The client is deliberately not the one the
# emulator was developed against, so agreement between them is not a tautology
# the way `wsioch` and the model are.
#
# `lp5250d` itself is the interoperability check and is run by hand - see
# docs/s36/printer-path.md - because it needs to be installed and it forks.
set -e
# The reference's copy of this suite issues its first machine command before
# any machine exists and is refused; `ipl pause` constructs it first here.
cd "$(dirname "$0")/.."
. test/probe-common.sh


# A configuration of its own, on ports nothing else in this suite uses, so that
# a developer with the emulator already running does not get a bind failure.
cat > "$TMP/printer.conf" <<'EOF'
[machine]
  volume           = @VOLUME@
  volume_readonly  = yes
  main_storage_kb  = 512
  ipl_type         = unattend
  ipl_source       = disk
  model            = advanced36

[station 0.0]
  role             = console
  device_code      = 11
  listen           = 127.0.0.1:12390

[station 0.4]
  role             = printer
  device_code      = PB
  listen           = 127.0.0.1:12394
EOF
sed -i "s#@VOLUME@#$SIM36_VOLUME#" "$TMP/printer.conf"

check() {   # check <name> <file> <fixed-string pattern>
    if grep -qF -- "$3" "$2"; then
        echo "  PASS  $1"
        pass=$((pass + 1))
    else
        echo "  FAIL  $1   (looked for: $3)"
        fail=$((fail + 1))
    fi
}


# ---------------------------------------------------------------------------
# 1. Nothing attached: the slot exists and the guest's request reaches it.
# ---------------------------------------------------------------------------

cat > "$TMP/idle.sim" <<'EOF'
ipl pause
trace ws
wsconfig
boot
prtwrite 0.4 C1C2C3
prtend 0.4
stations
quit
EOF

"$SIM36" -c "$TMP/printer.conf" -s "$TMP/idle.sim" > "$TMP/idle.out" 2>&1

# The printer is declared to the guest by the six-byte record `82` Read Current
# Configuration reports, byte 1 bits 0x30 = 0x20 - which is exactly what
# `Nudev5250::isaPrinter` (c185dac4) reads and the only decoded thing that says
# "printer" anywhere on this path.
check "the controller reports a printer slot" "$TMP/idle.out" \
      "0.4 unit 04 printer code PB (20 11)"
check "its configuration record carries the printer device class" "$TMP/idle.out" \
      "record 04 20 00 11 00 00"

# SVC 42, the IOB, the unit address, the printer backend. The unit address is
# what selects the device - `wsdvc`'s default arm is `wsprintr`, which is where
# 42 goes directly, so the SVC number does not partition the command set.
check "SVC 42 reaches the device through the control processor" "$TMP/idle.out" \
      "SVC 42 (Delayed) iob 030000 -> completed"
check "the request is routed by unit address to the printer" "$TMP/idle.out" \
      "unit=04 (port 0 address 4)"
check "the data stream reaches the PRINTER backend, not the display one" "$TMP/idle.out" \
      "put: 3 bytes from guest 030100 to printer 0.4"
check "with nothing attached it is accounted as dropped, not lost" "$TMP/idle.out" \
      "print record, 3 byte(s) of data stream dropped, no session attached"

# End of job is the operator's, and the trace says so rather than implying a
# guest event that has not been recovered.
check "end of job says it is operator policy" "$TMP/idle.out" \
      "no recovered System/36 event means end of spool file"

# ---------------------------------------------------------------------------
# 2. The configuration refuses what it cannot honestly express.
# ---------------------------------------------------------------------------

refuse() {   # refuse <name> <sed-expression-applied-to-printer.conf> <pattern>
    sed -e "$2" "$TMP/printer.conf" > "$TMP/bad.conf"
    "$SIM36" -c "$TMP/bad.conf" -s /dev/null > "$TMP/bad.out" 2>&1 || true
    check "$1" "$TMP/bad.out" "$3"
}

refuse "a printer with no device_code is refused, with the reason" \
       '/^  device_code      = PB/d' \
       "must state its device_code"
# signon_at_ipl is valid native display ownership policy. It is deliberately
# not tested as printer behavior here: it is neither a printer protocol option
# nor TFRM36 AUTOSIGNON.
refuse "an unknown role is refused, naming the closed set" \
       's|role             = printer|role             = plotter|' \
       "a slot is one of console, display, printer"
refuse "a printer at 0.0 is refused" \
       's|role             = console|role             = printer|' \
       "station 0.0 cannot be a printer"

# ---------------------------------------------------------------------------
# 3. A real socket: the RFC 2877 records, byte for byte.
# ---------------------------------------------------------------------------

cat > "$TMP/client.py" <<'PYEOF'
"""A minimal 5250 printer client. Negotiates enough of RFC 1205 section 2 to
reach 5250 mode, announces IBM-3812-1, then records every logical record the
server sends. It builds nothing and interprets nothing - it is a witness."""
import socket, sys, time

IAC, SE, SB, WILL, WONT, DO, DONT, EOR = 255, 240, 250, 251, 252, 253, 254, 239
BINARY, TTYPE, ENDREC, NEWENV = 0, 24, 25, 39

TERM = (sys.argv[3] if len(sys.argv) > 3 else "IBM-3812-1").encode()

s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
s.settimeout(6)
records, buf, state, sub = [], bytearray(), "data", bytearray()
deadline = time.time() + 20

def send(*b): s.sendall(bytes(b))

while time.time() < deadline:
    try:
        chunk = s.recv(4096)
    except socket.timeout:
        break
    if not chunk:
        break
    i = 0
    while i < len(chunk):
        c = chunk[i]; i += 1
        if state == "data":
            if c == IAC: state = "iac"
            else: buf.append(c)
        elif state == "iac":
            if c == IAC: buf.append(c); state = "data"
            elif c in (WILL, WONT, DO, DONT): verb = c; state = "verb"
            elif c == SB: sub = bytearray(); state = "sub"
            elif c == EOR:
                records.append(bytes(buf)); buf = bytearray(); state = "data"
            else: state = "data"
        elif state == "verb":
            # The server asks; a printer client agrees to exactly what RFC 1205
            # section 2 needs and refuses everything else.
            if verb == DO:
                send(IAC, WILL if c in (NEWENV, TTYPE, BINARY, ENDREC) else WONT, c)
            elif verb == WILL:
                send(IAC, DO if c in (BINARY, ENDREC) else DONT, c)
            state = "data"
        elif state == "sub":
            if c == IAC: state = "subiac"
            else: sub.append(c)
        elif state == "subiac":
            if c == IAC: sub.append(c); state = "sub"
            elif c == SE:
                if len(sub) >= 2 and sub[0] == TTYPE and sub[1] == 1:
                    send(IAC, SB, TTYPE, 0, *TERM, IAC, SE)
                elif len(sub) >= 2 and sub[0] == NEWENV and sub[1] == 1:
                    # VAR TERM = <type>, the way tn5250 reports it.
                    send(IAC, SB, NEWENV, 0, 0, *b"TERM", 1, *TERM, IAC, SE)
                state = "data"
            else: state = "sub"
    if len(records) >= int(sys.argv[2]):
        break

for r in records:
    print(r.hex())
s.close()
PYEOF

# The emulator is driven through a FIFO so it stays alive while the client is
# attached: a script argument runs to the end and exits.
mkfifo "$TMP/in"
sleep 60 > "$TMP/in" &
hold=$!

"$SIM36" -c "$TMP/printer.conf" -t ws < "$TMP/in" > "$TMP/live.out" 2>&1 &
emu=$!
sleep 3
# The reference's copy lets the client negotiate before any machine exists:
# the pending trace is not yet applied, the printer has not been created and
# so still announces the backend's default object name, and `boot` is refused.
# The machine is constructed first here, before the client arrives.
echo "ipl pause" > "$TMP/in" &
sleep 2

# Three records expected: the startup response, one print record, the null one.
python3 "$TMP/client.py" 12394 3 > "$TMP/records" 2>"$TMP/client.err" &
client=$!
sleep 3

{ echo "boot"; sleep 2; echo "prtwrite 0.4 C1C2C3"; sleep 2; echo "prtend 0.4"; } > "$TMP/in" &
wait $client 2>/dev/null || true
sleep 1
echo "quit" > "$TMP/in" &
sleep 2
kill $hold $emu 2>/dev/null || true
wait $emu 2>/dev/null || true

# RFC 2877 section 9, figure 1 - IBM's own printed success response record, with
# only the response code, the system name and the object name substituted. The
# variable header length is 05, not the 04 a display record carries, which is
# why `lp5250d` computes its data offset as 6 + record[6] rather than assuming.
check "startup response: RFC 2877 figure 1's header, then I902" "$TMP/records" \
      "004912a090000560060020c0003d0000c9f9f0f2"
# ...and the two names the RFC does identify, EBCDIC and blank padded, where its
# own example carries TARGET and PCPRINTER.
check "startup response: system name and object name follow the code" "$TMP/records" \
      "c9f9f0f2e2f3f6d9c5c6c5d4d7d9e3f0f44040404040"

# RFC 2877 section 10, figure 4: LLLL 12A0 0101 0A 1800 01 000000000000 <data>.
# First of chain AND last of chain, because one Output Data command is one whole
# data stream (SA21-9436 5-50).
check "print record: RFC 2877 section 10 figure 4 header, then the data" "$TMP/records" \
      "001312a001010a180001000000000000c1c2c3"

# RFC 2877 section 10.3, figure 6, quoted exactly: the null print record that
# ends a job. `lp5250d` recognises it by its total length of 0x11 alone.
check "null print record: RFC 2877 section 10.3 figure 6, byte for byte" "$TMP/records" \
      "001112a001010a08000100000000000000"

# And the emulator saw the client for what it is.
check "the client's printer terminal type is accepted at a printer slot" "$TMP/live.out" \
      "terminal type IBM-3812-1"
check "the startup response is sent once negotiation completes" "$TMP/live.out" \
      "record out startup response I902"

# ---------------------------------------------------------------------------
# 4. Typed slots: a display client at a printer endpoint is hung up on.
#
# This is the defect the experiment exposed, in the other direction. The two
# families agree on every Telnet option and differ only in record framing, so a
# mismatched pair negotiates all the way through and then carries nothing - a
# failure that looks exactly like a working session until somebody notices no
# bytes moved.
# ---------------------------------------------------------------------------

mkfifo "$TMP/in2"
sleep 30 > "$TMP/in2" &
hold2=$!

"$SIM36" -c "$TMP/printer.conf" -t ws < "$TMP/in2" > "$TMP/mismatch.out" 2>&1 &
emu2=$!
sleep 3
# As above: construct the machine before the client arrives.
echo "ipl pause" > "$TMP/in2" &
sleep 2

# IBM-3180-2 is in RFC 1205 section 2's display list, and 12394 is the printer.
python3 "$TMP/client.py" 12394 1 IBM-3180-2 > /dev/null 2>&1 &
mm=$!
sleep 4
{ echo "stations"; sleep 1; echo "quit"; } > "$TMP/in2" &
sleep 3
kill $hold2 $emu2 $mm 2>/dev/null || true
wait $emu2 2>/dev/null || true

check "a display client at a printer slot is REFUSED" "$TMP/mismatch.out" \
      "this endpoint is a printer station and the client announced TERMINAL-TYPE IBM-3180-2"
check "the refusal is counted where an operator will see it" "$TMP/mismatch.out" \
      "session(s) REFUSED for announcing a display terminal type at a printer slot"
check "and no startup response was sent to it" "$TMP/mismatch.out" \
      "0 startup response(s)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
