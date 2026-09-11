#!/bin/sh
# Freeing the request-block tail of a combined task allocation must return
# exactly its own bytes, never the neighbouring allocation's.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh
python3 test/build-sqs-vectors.py "$TMP/sqs-ownership.bin"
out=$(run system-queue-ownership)
bad=$(run system-queue-ownership-invalid)

checkin "combined task allocation is class-rounded " 'assign 432 -> 512 bytes at guest F970' "$out"
checkin "the neighboring allocation is independently owned" 'assign 256 -> 256 bytes at guest FB70' "$out"
checkin "native tail free returns its exact 272 bytes" 'free 272 -> 272 bytes at guest FA30' "$out"
checkin "ownership/free-list cross-check remains clean" 'op 6; invariants OK' "$out"
checkin "neighbor remains owned after the tail free" 'allocated FB70..FC6F (100 bytes), allocation #5' "$out"
if echo "$out" | grep -qF 'SQS INVARIANT VIOLATION'; then
  echo "  FAIL  invariant journal (unexpected ownership violation)"; fail=$((fail + 1))
else
  echo "  PASS  invariant journal"; pass=$((pass + 1))
fi
if echo "$out" | grep -qF 'free 272 -> 512 bytes at guest FA30'; then
  echo "  FAIL  class-rounded free (crosses into neighboring allocation)"; fail=$((fail + 1))
else
  echo "  PASS  class-rounded free absent"; pass=$((pass + 1))
fi
checkin "crossing-free first-operation diagnostic" \
  'SQS INVARIANT VIOLATION: op 6: free FA30+200 is not contained in one live allocation; intersects #4 F970+200, #5 FB70+100' "$bad"
checkin "free/allocated overlap audit" 'free FA30+200 overlaps allocation #5 FB70+100' "$bad"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
