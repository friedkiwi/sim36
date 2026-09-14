#!/bin/sh
# A synchronous SVC 10 with Q bit 7 off replaces its caller and carries the
# caller's program request area into the replacement request block.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-no-return-transfer-vectors.py \
    "$TMP/blocks.bin" "$TMP/code.bin" "$TMP/target.bin"

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0C00
loadfile $TMP/code.bin 1000
loadfile $TMP/target.bin 2000
trace csp
set iar 1000
step 5
show cpu
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"

check "10  no-return carries request area " \
      'no-return transfer carried 16 byte(s) of program request area'
check "10  replacement observes the marker" \
      'IAR 2028'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
