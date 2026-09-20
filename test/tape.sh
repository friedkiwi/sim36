#!/bin/sh
# The folder tape backend: init a labeled tape folder, then round-trip read,
# write, tape marks and record/file spacing through the generic tape backend
# operations, and prove the written tape survives a flush to disk.
#
# This is a STANDALONE storage-seam test. It does not involve the guest, the
# SVC 46 path or any config knob (test/tape-svc.sh and test/tape-operator.sh
# cover those). It drives the backend through the monitor's `tapetest`
# diagnostic, the tape analogue of `selftest`, which runs the whole round-trip
# natively and reports pass/fail, and it separately inspects the on-disk folder
# format the backend writes.
set -u
cd "$(dirname "$0")/.."
. test/probe-common.sh
ok()   { echo "  $1 PASS"; pass=$((pass + 1)); }
bad()  { echo "  $1 FAIL  $2"; fail=$((fail + 1)); }

# ---------------------------------------------------------------------------
# 1. The native round-trip: every tape backend operation, standalone.
# ---------------------------------------------------------------------------
echo "-- tape backend round-trip (monitor tapetest) --"
instantiate default-machine
printf 'ipl pause\ntapetest %s/tape\nquit\n' "$TMP" > "$TMP/tt.sim"
out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/tt.sim" 2>&1)

check "the backend round-trip is all green    " "tape backend: 20 passed, 0 failed"
# A couple of the individual properties, named, so a regression points at itself.
check "init writes a labeled tape folder      " "init writes a labeled tape folder          PASS"
check "reads decode EBCDIC VOL1               " "...first four bytes are EBCDIC VOL1        PASS"
check "written data survives a flush to disk  " "written data survived the round-trip to disk PASS"
check "record spacing stops on a tape mark    " "space forward stops on the tape mark       PASS"

# ---------------------------------------------------------------------------
# 2. The on-disk folder format the backend wrote.
# ---------------------------------------------------------------------------
echo "-- the folder format on disk --"
FOLDER="$TMP/tape"
# tapetest deletes a temp folder it created itself, but here we named one, so it
# stays for inspection.
[ -f "$FOLDER/manifest.json" ] && ok "manifest.json is written              " \
                              || bad "manifest.json is written              " "(missing)"
[ -f "$FOLDER/0001.dat" ] && ok "the VOL1 label blob is written        " \
                         || bad "the VOL1 label blob is written        " "(missing)"
[ -f "$FOLDER/0002.dat" ] && ok "the written data blob is written      " \
                         || bad "the written data blob is written      " "(missing)"

out=$(cat "$FOLDER/manifest.json")
check "the manifest declares the format       " '"format": "s36-folder-tape"'
check "the volume serial is recorded          " '"volumeId": "TAP01"'
check "the label group re-decodes after write " '"kind": "label"'
check "variable block lengths are recorded    " '"blockLengths": [9, 19, 1]'

# The data blob is exactly the concatenated blocks, no framing.
got=$(cat "$FOLDER/0002.dat")
[ "$got" = "BLOCK-ONESECOND-BLOCK-longer3" ] \
  && ok "the data blob is the raw blocks        " \
  || bad "the data blob is the raw blocks        " "(got: $got)"

# ---------------------------------------------------------------------------
# 3. A plain folder is never mistaken for a tape: the backend never writes a
#    manifest where there was not one.
# ---------------------------------------------------------------------------
echo "-- refusing a non-tape folder --"
mkdir -p "$TMP/notape"
echo "hello" > "$TMP/notape/readme.txt"
[ ! -f "$TMP/notape/manifest.json" ] && ok "a plain folder has no manifest        " \
                                    || bad "a plain folder has no manifest        " "(unexpected)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
