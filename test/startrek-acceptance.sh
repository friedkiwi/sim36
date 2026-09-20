#!/bin/sh
# Complete, private-volume-gated STARTREK acceptance path.  FUNLIB remains in
# a caller/CI-owned checkout and both tapes and the writable AS/36 copy live in
# a private temporary directory.
set -eu

cd "$(dirname "$0")/.."
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "$SIM36_VOLUME" ]; then
    echo "SKIP: no private AS/36 volume (set SIM36_VOLUME)"
    exit 77
fi
if [ -z "${FUNLIB_DIR:-}" ] || [ ! -d "$FUNLIB_DIR" ]; then
    echo "SKIP: no pinned FUNLIB checkout (set FUNLIB_DIR)"
    exit 77
fi

work=$(mktemp -d)
cleanup() {
    find "$work" -depth -type f -delete 2>/dev/null || true
    find "$work" -depth -type d -empty -delete 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

python3 tools/build-startrek-tape.py "$work/install-tape" \
    --funlib-dir "$FUNLIB_DIR"

# Each run gets distinct library names even if several runners share a host.
suffix=$(printf '%05d' "$$" | tail -c 6)
install_library="TS$suffix"
restore_library="TR$suffix"
export STARTREK_TAPE="$work/install-tape"
export STARTREK_EXPORT_TAPE="$work/export-tape"
export STARTREK_LIBRARY="$install_library"
export STARTREK_RESTORE_LIBRARY="$restore_library"
export STARTREK_TRACE=disk
export S36_PORT_BASE="${S36_PORT_BASE:-26300}"

runner="${SIM36:-$PWD/build/linux-make/sim36}"
export SIM36="$runner"
if command -v timeout >/dev/null 2>&1; then
    timeout 40m python3 test/startrek-acceptance.py
else
    python3 test/startrek-acceptance.py
fi
