#!/bin/sh
# The tape OPERATOR surface: the `attach tape0` definition and the monitor
# `tape` command (load/unload/init, and the vtoc/files listings). test/tape.sh
# covers the backend and test/tape-svc.sh the SVC 46 path; this proves an
# operator can create, mount, inspect and dismount a tape, and that a tape
# attached in the machine definition is in the drive at power-on.
#
# It drives the real command surface, not a diagnostic.  The reference's
# version of this gate predates the monitor command language (it appends a
# `[tape]` section to a `.conf` file, and its `tape` commands run before the
# machine is constructed, which the reference refuses today); the definition
# half is expressed here with `attach tape0`, which is what the reference's
# session accepts, and every script constructs the machine with `ipl pause`
# first.  The "unknown tape key" check has no counterpart in the command
# language and is not carried over.
set -u
cd "$(dirname "$0")/.."
. test/probe-common.sh
ok()   { echo "  $1 PASS"; pass=$((pass + 1)); }
bad()  { echo "  $1 FAIL  $2"; fail=$((fail + 1)); }
absent() {  # absent <name> <fixed-string pattern>
  if echo "$out" | grep -qF -- "$2"; then bad "$1" "(should not contain: $2)"; else ok "$1"; fi
}

instantiate default-machine

# ---------------------------------------------------------------------------
# 1. init / load / status / vtoc / files / unload - the full run-time surface.
#
# A fresh tape is inited, mounted and inspected; then a data file is appended
# through the SVC path (tapesvc, which flushes on its own unload) and the TAP
# file is re-loaded so `tape files` catalogs the resulting tape files.
# ---------------------------------------------------------------------------
echo "-- tape init / load / vtoc / files / unload --"
cat > "$TMP/ops.sim" <<EOF
ipl pause
tape
tape load $TMP/created.tap
tape unload
tape init $TMP/tp.tap TAP07 ACME
tape load $TMP/tp.tap
tape
tape vtoc
tape files
boot
tapesvc $TMP/tp.tap
tape load $TMP/tp.tap
tape files
tape unload
tape
quit
EOF
out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/ops.sim" 2>&1)

check "an empty drive reports itself empty     " "tape drive: EMPTY"
check "a missing writable path creates a TAP    " "mounted $TMP/created.tap, volume (unlabeled)"
check "init writes a fresh labeled SIMH tape    " "initialised tape $TMP/tp.tap: volume TAP07, owner ACME"
check "load mounts it and names the volume      " "mounted $TMP/tp.tap, volume TAP07"
check "status shows the mounted medium          " "tape drive: $TMP/tp.tap"
check "  ... and the volume serial              " "volume      TAP07"
check "  ... and the spun-up state              " "loaded (spun up at load point)"
# vtoc decodes the VOL1 label group.
check "vtoc lists the VOL1 volume serial        " "volume id       TAP07"
check "  ... and the owner id                   " "owner id        ACME"
check "  ... and the decoded VOL1 label record  " "VOL1 volumeId=TAP07"
# A blank tape has the VOL1 file plus an empty file for the second mark.  The
# later write replaces that empty terminator, so the count remains two.
check "files lists label plus empty terminator  " "-- tape files (2) --"
check "  ... as an 80-byte label file           " "1  label"
check "files re-lists after guest tape writes   " "-- tape files (4) --"
check "  ... and the guest data file appears    " "2  data"
check "unload dismounts and flushes             " "tape unloaded (writes flushed)"
# The resulting media is a single SIMH file, not a conversion workspace.
[ -f "$TMP/tp.tap" ] && ok "init created a TAP file on disk           " \
                          || bad "init created a TAP file on disk           " "(absent)"

# Positioning and filemark commands report both the operation result and the
# resulting head location.  Use a separate cartridge so this exercise cannot
# alter the listing fixture above.
cat > "$TMP/position.sim" <<EOF
ipl pause
tape init $TMP/pos.tap POS001 TEST
tape load $TMP/pos.tap
tape position
tape space block 1
tape space file 1
tape rewind
tape space file 1
tape mark 2
tape position
tape unload
tape load $TMP/pos.tap ro
tape mark
quit
EOF
out=$($SIM36 -c "$TMP/default-machine.sim" -s "$TMP/position.sim" 2>&1)
check "position reports load point               " "tape position: file 0 block 0 BOT"
check "block spacing stops at a tape mark        " "tape space block 1: Ok, moved 1; file 0 block 1 @mark"
check "file spacing crosses the mark             " "tape space file 1: Ok, moved 1; file 1 block 0 @mark"
check "rewind reports the resulting BOT          " "tape rewind: Ok; file 0 block 0 BOT"
check "mark writes consecutive filemarks         " "tape mark 2: Ok, wrote 2; file 3 block 0 EOD"
check "position sees the consecutive marks       " "tape position: file 3 block 0 EOD"
check "read-only filemark writes are refused     " "tape mark 1: WriteProtected, wrote 0"

# ---------------------------------------------------------------------------
# 2. A tape attached in the definition is in the drive at power-on.
#
# `attach tape0` pointing at the TAP from part 1 must mount at construction,
# so `tape` shows it loaded with no explicit `tape load`.
# ---------------------------------------------------------------------------
echo "-- an attached tape0 mounts at power-on --"
{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/tp.tap rw\n' "$TMP"; } > "$TMP/tape.sim"
printf 'ipl pause\ntape\ntape files\nquit\n' > "$TMP/mounted.sim"
out=$("$SIM36" -c "$TMP/tape.sim" -s "$TMP/mounted.sim" 2>&1)
check "the declared tape is mounted at startup  " "tape drive: $TMP/tp.tap"
check "  ... with its volume serial             " "volume      TAP07"
check "  ... and its files are listable         " "-- tape files (4) --"

# read-only can be requested from the definition.
echo "-- a read-only attach protects the tape --"
{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/tp.tap ro\n' "$TMP"; } > "$TMP/ro.sim"
out=$("$SIM36" -c "$TMP/ro.sim" -s "$TMP/mounted.sim" 2>&1)
check "a readonly tape mounts read-only         " "tape drive: $TMP/tp.tap (read-only)"

# ---------------------------------------------------------------------------
# 3. Creation, blank-file, folder compatibility, and refusal edges.
# ---------------------------------------------------------------------------
echo "-- path type selects the backend --"
: > "$TMP/blank.tap"
{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/blank.tap ro\n' "$TMP"; } > "$TMP/blank.sim"
out=$("$SIM36" -c "$TMP/blank.sim" -s "$TMP/mounted.sim" 2>&1)
check "a blank file is a valid empty TAP       " "tape drive: $TMP/blank.tap (read-only)"
check "  ... and contains no tape files        " "the tape has no files"

mkdir "$TMP/folder"
cat > "$TMP/folder.sim" <<EOF
ipl pause
tape init $TMP/folder FOLD01 LEGACY
tape load $TMP/folder
tape status
quit
EOF
out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/folder.sim" 2>&1)
check "only a directory selects folder media   " "mounted $TMP/folder, volume FOLD01"
check "  ... with a folder manifest            " "volume      FOLD01"
[ -f "$TMP/folder/manifest.json" ] && ok "folder compatibility writes a manifest   " \
                                      || bad "folder compatibility writes a manifest   " "(absent)"

{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/new-at-startup.tap rw\n' "$TMP"; } > "$TMP/new.sim"
out=$("$SIM36" -c "$TMP/new.sim" -s "$TMP/mounted.sim" 2>&1)
check "a missing writable attach creates TAP   " "tape drive: $TMP/new-at-startup.tap"
[ -f "$TMP/new-at-startup.tap" ] && ok "startup attach created the TAP file       " \
                                      || bad "startup attach created the TAP file       " "(absent)"

{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/missing-ro.tap ro\n' "$TMP"; } > "$TMP/missing-ro.sim"
out=$("$SIM36" -c "$TMP/missing-ro.sim" -s "$TMP/mounted.sim" 2>&1 || true)
check "a missing read-only attach is refused   " "cannot be used as a tape"

printf 'not a tap' > "$TMP/bad.tap"
{ cat "$TMP/default-machine.sim"; printf 'attach tape0 %s/bad.tap ro\n' "$TMP"; } > "$TMP/bad.sim"
out=$("$SIM36" -c "$TMP/bad.sim" -s "$TMP/mounted.sim" 2>&1 || true)
check "malformed existing TAP is refused       " "cannot be used as a tape"

# ---------------------------------------------------------------------------
# 4. The default machine has no tape, so the drive is empty and nothing changed.
# ---------------------------------------------------------------------------
echo "-- the default machine ships with an empty drive --"
printf 'ipl pause\ntape\nquit\n' > "$TMP/ship.sim"
out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/ship.sim" 2>&1)
check "the default machine leaves the drive empty" "tape drive: EMPTY"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
