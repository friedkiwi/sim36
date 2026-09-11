#!/bin/sh
# Parity gate for configuration inspection with a user-supplied volume:
# `show config`, `save config stdout` and the attach report must be identical
# on both emulators.  Needs SIM36_VOLUME (a System/36 volume image) and
# SIM36_REFERENCE; reports "no volume" and exits 0 when the image is absent.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "config-volume: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "config-volume: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-config-volume.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")
cat >"$work/volume.sim" <<SIM
set terminal multiplex off
set machine memory 512K
attach disk0 "$volume" overlay
set station 0.2 role display
set station 0.0 role console
set station 0.1 role display
set station 0.1 device-code 11
set station 0.1 signon-at-ipl on
show config
save config stdout
attach disk0 "$volume" ro
show config
attach disk0 "$volume" rw
show config
detach disk0
show config
attach diskette0 "$volume" rw
attach tape0 "$work" ro
show config
save config stdout
detach diskette0
detach tape0
save config stdout
quit
SIM
"$here/tools/diffrun.sh" "$work/volume.sim"
