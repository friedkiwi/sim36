#!/bin/sh
# SVC 26 Prepare Print Buffer, and SVC 36 SMFC.
#
# The addresses and the field list are SA21-9436 3-114's worked example; the
# expected bytes are worked out by hand from that page's own paragraph on what
# the scan does, and from the control codes the SLIC routines emit.  Every
# assertion is on guest storage the guest can read back, not on a trace line
# this emulator chose to print.
#
# docs/s36/svc-prepare-print-buffer.md, docs/s36/svc-smfc.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-print-vectors.py \
        "$TMP/blocks.bin" "$TMP/buffers.bin" "$TMP/program.bin"

cat > "$TMP/run.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/blocks.bin 4540
loadfile $TMP/buffers.bin 6000
loadfile $TMP/program.bin 1000
# The SSP Ideographic feature indicator, SCADSSPF bit SCAMKKKF at guest 0808
# (nuptscan c18df258).  The non-ideographic data below has no SO/SI, so turning
# it on changes none of those results; the ideographic run at the end needs it.
poke 808 80
trace csp
# --- SVC 26, output spooled ------------------------------------------------
set pxr1 00
set xr1 4540
set iar 1000
step 1
dump 6000 10
dump 4550 10
dump 4640 10
# --- SVC 26, output direct to a printer, so the unit block is updated -------
set pxr1 00
set xr1 4580
set iar 1003
step 1
dump 6100 10
dump 4640 10
# --- SVC 36 ----------------------------------------------------------------
set pxr1 00
set xr1 4540
set iar 1006
step 1
set iar 1009
step 1
# --- SVC 26, ideographic (2-byte) data, spooled ----------------------------
set pxr1 00
set xr1 4680
set iar 100C
step 1
dump 6200 10
dump 4690 10
# --- SVC 26, the same data direct (not spooled): refused --------------------
# The spooled run rewrote its buffer in place, so restore the source data first.
loadfile $TMP/buffers.bin 6000
poke 4687 00
set pxr1 00
set xr1 4680
set iar 100C
step 1
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# The whole prepared buffer, byte for byte.  Reading it left to right:
#
#   0c           a form feed, because the skip-before asks for line 1 and the
#                IOB says the printer is on line 10
#   c1 c2        the first two characters, copied
#   34 c8 05     five blanks replaced by a relative horizontal position code -
#                3-113's "when it finds more than three contiguous blanks, it
#                compresses those blanks"
#   c3           the next character
#   ff           what hex 05 became - "if it finds a character less than hex 40
#                (blank), it replaces that character with a hex FF"
#   c4           the last character
#   34 c4 02     position to line 2, the space-after having moved the printer
#   0d           and the carriage return that ends the record
#
# The three bytes after it are what the source data left there, untouched.
check "26  the prepared buffer, byte for byte" \
      '006000  0c c1 c2 34 c8 05 c3 ff c4 34 c4 02 0d c3 05 c4'

# 3-114: "$IOBPLNG is updated to reflect the number of characters in the print
# buffer" - thirteen - and "$IOBPCLN is updated to the new current line value".
# The IOB dump starts at +16, so the line reads:
#   +16 000d  length      +18..20 -   +21..23 004600  pub
#   +24 42    forms 66    +25 02  current line 2
check "26  length and current line updated  " \
      '004550  00 0d 00 00 00 00 46 00 42 02'

# "$IOBP#FF is set to the number of forms feed printer control codes that is
# inserted into the print buffer BEFORE the data is printed" - one, the skip to
# line 1 - "$IOBP#AF contains the number ... AFTER the data" - none.  The dump
# starts at +16 again, so +31 and +32 are the last two bytes shown.
check "26  form feed counts, before and after" \
      '004550  00 0d 00 00 00 00 46 00 42 02 40 01 00 00 01 01'

# Spooled output leaves the printer unit block alone; direct output updates its
# forms length and current line.  The unit block dump starts at +64, so +69 and
# +70 are bytes 6 and 7 of the line.
check "26  spooled leaves the unit block    " '004640  00 00 00 00 00 ee ee'
check "26  direct updates forms and line    " '004640  00 00 00 00 00 42 02'
check "26  and says which it took           " 'direct to printer - unit block 4600'

# The second IOB prepares to the same bytes in its own buffer, which is the
# check that nothing above depended on state left behind by the first call.
check "26  the second buffer matches        " \
      '006100  0c c1 c2 34 c8 05 c3 ff c4 34 c4 02 0d c3 05 c4'

# Ideographic (2-byte) data, spooled.  Reading the buffer left to right:
#
#   0c              form feed, skip-before to line 1 from line 10
#   0e              SO, copied - it enters 2-byte mode (nuptscan c18df318)
#   42 43 44 45     two ward/point pairs, copied verbatim (the point byte is
#                   never turned into FF - c18df388's dedicated loop)
#   0f              SI, copied - it leaves 2-byte mode (c18df34c)
#   34 c4 02        position to line 2, the space-after having moved the printer
#   0d              the carriage return that ends the record
check "26  ideographic buffer, byte for byte " \
      '006200  0c 0e 42 43 44 45 0f 34 c4 02 0d'

# $IOBPLNG = 000b (eleven), PUB 004700, forms 66, current line 2, and $IOBPCTL
# back to 40 - the 2-byte mode bit (08) cleared by the SI (nuptscan c18df368).
check "26  ideographic length, line, mode off " \
      '004690  00 0b 00 00 00 00 47 00 42 02 40 01 00 00 01 01'

# Direct (not spooled) ideographic is refused: nuptckpt (c18df834) would rewrite
# bytes from a printer-capability bit this corpus does not place.
check "26  ideographic direct is refused      " \
      'ideographic 2-byte data with the output direct (not spooled)'

# SVC 36: the Q-byte's only defined bit, and the IOB it was given.
check "36  Q bit 2 on is a receive error    " 'SMFC: RECEIVE error'
check "36  Q bit 2 off is a transmit error  " 'SMFC: TRANSMIT error'
check "36  and it names the IOB from XR1    " 'IOB XR1 = 004540'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
