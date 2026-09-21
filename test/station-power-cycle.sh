#!/bin/sh
# A client hanging up is a display switched off; the next client is the
# display switched on.  docs/s36/station-power-cycle-2026-09-11.md
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here"
. test/probe-common.sh
out=$(cd "$here" && python3 test/station-power-cycle.py 2>&1)

pass=0
fail=0
check() {
    if printf '%s\n' "$out" | grep -qF "$2"; then
        echo "  PASS  $1"; pass=$((pass + 1))
    else
        echo "  FAIL  $1   (looked for: $2)"; fail=$((fail + 1))
    fi
}

# --- per-station listener --------------------------------------------------
check "the first client on W2's listener gets the sign-on" \
      "phase 1 first client SIGN ON: yes"
check "hanging up is reported as the display powering off" \
      "phase 1 monitor reported the power-off: yes"
check "the retained sign-on invite fails with the physical power-off status" \
      "retained PUT-with-invite (A7) IOB"
check "  ... TU+13=22, device status 80, class back to C0, completion 41" \
      "TU+13=22, +1A=80 (display powered off), class C0, completion 41"
check "  ... and the TFRM36 transfer is released for the next power-on" \
      "transfer released, the next client is a new power-on"
check "the next client on the same listener gets a fresh sign-on" \
      "phase 1 second client SIGN ON: yes"
check "  ... on a live session that answers its keyboard" \
      "phase 1 second client's Enter was answered: yes"

# --- station multiplexer ---------------------------------------------------
check "the first client through the multiplexer gets the sign-on" \
      "phase 2 first client SIGN ON: yes"
check "the multiplexer hang-up powers the display off too" \
      "phase 2 monitor reported the power-off: yes"
check "the next client selecting the same station gets a fresh sign-on" \
      "phase 2 second client SIGN ON: yes"
check "  ... and it is live" \
      "phase 2 second client's Enter was answered: yes"

# --- a signed-on session ---------------------------------------------------
check "the first client signs on to MAIN" \
      "phase 3 first client signed on to MAIN: yes"
check "hanging up from MAIN powers the display off" \
      "phase 3 monitor reported the power-off: yes"
check "the next client is not dropped into the old session" \
      "phase 3 second client sees MAIN instead: no"
check "it gets a sign-on: the session ended with the display" \
      "phase 3 second client SIGN ON: yes"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
