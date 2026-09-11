#!/bin/sh
# Configuration-surface parity against the reference emulator over every
# test/config-*.sim.  Skips when SIM36_REFERENCE is not set.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "config-parity: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
exec "$here/tools/diffrun.sh" "$here"/test/config-*.sim "$here"/test/refused/*.sim
