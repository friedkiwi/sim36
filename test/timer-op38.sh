#!/bin/sh
# Focused SVC 50 transient 0A operation 38 cancellation test.
#
# Register one exact #CPTC type-2 timer, cancel it from another TRB with the
# same task/key, then cancel again. The first result must be zoned `000003` for
# the immediate 488-unit cancellation; the miss must return zoned `000000`. Cancellation
# must not post a task or manufacture a workstation event.
# docs/s36/svc50-transient0a-nutix-contract-2026-09-06.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh


python3 - "$TMP/register.bin" "$TMP/cancel.bin" "$TMP/miss.bin" "$TMP/code.bin" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytes.fromhex(
    '82 01 AA BB 00 00 01 E8 00 00 00 00 00 00'))
open(sys.argv[2], 'wb').write(bytes.fromhex(
    '08 01 CC DD FF FF FF FF 00 00 00 00 00 00'))
open(sys.argv[3], 'wb').write(bytes.fromhex(
    '08 01 EE FF FF FF FF FF 00 00 00 00 00 00'))
open(sys.argv[4], 'wb').write(bytes.fromhex(
    'F400500A2000 F400500A3800 F400500A3800'))
PY

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/register.bin 0C00
loadfile $TMP/cancel.bin 0D00
loadfile $TMP/miss.bin 0E00
loadfile $TMP/code.bin 1000
trace csp
set pxr2 00
set xr2 0C00
set iar 1000
step 1
set xr2 0D00
step 1
set xr2 0E00
step 1
dump 0C00 14
dump 0D00 14
dump 0E00 14
sched
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


check "op38 removes matching task/key        " \
  'matching registration removed'
check "second op38 reports an actual miss    " \
  'no matching registration, remaining 0 unit(s) -> zoned 00:00:00 at +2..+7'
check "cancel returns type-8 zoned time      " \
  '000d00  08 01 f0 f0 f0 f0 f0 f3'
check "miss result is zoned zero             " \
  '000e00  08 01 f0 f0 f0 f0 f0 f0'
check "native scheduler has no expiry event  " 'now = 0, 0 pending'

posts=$(echo "$out" | grep -c 'immediate nupotcb' || true)
if [ "$posts" -eq 1 ]; then
  echo "  cancel creates no additional task post  PASS"; pass=$((pass + 1))
else
  echo "  cancel creates no additional task post  FAIL (saw $posts total)"; fail=$((fail + 1))
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
