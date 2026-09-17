#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
printer_tmp=
trap 'test -z "$printer_tmp" || rm -rf "$printer_tmp"' EXIT
if [ -n "${SIM36_SSP75_VOLUME:-}" ]; then
    SIM36_VOLUME=$SIM36_SSP75_VOLUME
elif [ -f images/volumes/ssp75.img ]; then
    SIM36_VOLUME=$PWD/images/volumes/ssp75.img
elif [ -f images/volumes/ssp75.img.gz ]; then
    printer_tmp=$(mktemp -d)
    gzip -dc images/volumes/ssp75.img.gz > "$printer_tmp/ssp75.img"
    SIM36_VOLUME=$printer_tmp/ssp75.img
elif [ -z "${SIM36_VOLUME:-}" ]; then
    echo "SKIP: no pristine SSP 7.5 volume (set SIM36_SSP75_VOLUME)"
    exit 77
fi
export SIM36_VOLUME
SIM36=${SIM36:-build/linux-make/sim36}
export SIM36
if [ ! -x "$SIM36" ]; then
    echo "SKIP: no sim36 executable at $SIM36 (set SIM36)"
    exit 77
fi
out=$(python3 test/ipl-printer-rebuild.py)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F \
    "PASS: shipped PB-printer topology completed SSP 7.5 file rebuild and W3 reached SIGN ON"
