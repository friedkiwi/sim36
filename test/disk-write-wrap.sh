#!/bin/sh
# SVC 40 A2 with modifier 80 (data field wrap) writes ONE 256-byte area to every
# sector of the transfer (SA21-9243-4 6-4 part 6 bit 0).  Synthetic: an IOB and
# a two-instruction stub poked at `ipl pause`, no SSP product code; the volume
# is attached overlay so nothing is written to the file.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh
count() {
    n=$(printf '%s\n' "$3" | grep -cF "$2" || true)
    if [ "$n" -eq "$4" ]; then
        echo "  PASS  $1"; pass=$((pass + 1))
    else
        echo "  FAIL  $1 (found $n of '$2', wanted $4)"; fail=$((fail + 1))
    fi
}
out=$(run disk-write-wrap)
checkin "the request is decoded as A2 with modifier 80, 4 sectors at 0-based 700000" \
      "cmd=A2/80 sector=700000 count-1=3 buffer=002000" "$out"
checkin "the write takes the wrap arm and names the one source area" \
      "write 4 sector(s) at 700000, data field WRAP: the one 256-byte area at guest 002000" "$out"
checkin "the SVC completed (completion byte 40 in the IOB)" \
      "001a00  c9 c6 00 00 00 00 40" "$out"
count "the request was not refused" "refused" "$out" 0
sect=$(printf '%s\n' "$out" | grep -E '^020[0-3]00 ' | grep -cF "d7 c1 e3 e3 c5 d9 d5 f1" || true)
if [ "$sect" -eq 4 ]; then echo "  PASS  all four sectors carry the 256-byte area at 2000"; pass=$((pass + 1))
else echo "  FAIL  all four sectors carry the 256-byte area at 2000 (found $sect of 4)"; fail=$((fail + 1)); fi
count "no sector carries the bytes at 2100 (only one sector of storage is read)" \
      "d5 d6 e3 40 e3 c8 c9 e2" "$out" 0
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
