#!/bin/sh
# The startup/configuration file and stdin use the same command processor.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh
ok() { echo "  $1 PASS"; pass=$((pass + 1)); }
bad() { echo "  $1 FAIL"; fail=$((fail + 1)); }
has() { if printf '%s\n' "$out" | grep -qF "$2"; then ok "$1"; else bad "$1"; fi; }
volume=$(basename "$SIM36_VOLUME")

instantiate command-file-machine
cp test/command-file-operations.sim "$TMP/command-file-operations.sim"
out=$("$SIM36" -c "$TMP/command-file-machine.sim" 2>&1) || true
has "one command file constructs the machine" "volume $volume:"
volume_count=$(printf '%s\n' "$out" | grep -c "^volume $volume:" || true)
if [ "$volume_count" -eq 1 ]; then
    ok "fixed-disk metadata is reported at attach, not repeated by IPL"
else
    bad "fixed-disk metadata is reported at attach, not repeated by IPL"
fi
has "nested do resolves relative to its file" "ipl pause"
has "definition is replayable" "attach disk0"
has "station IPL ownership is replayable" "set station 0.0 signon-at-ipl on"
has "reset permits reconstruction" "reset"
has "reset reports the resulting operator-visible state" \
    "reset complete; machine stopped and configuration editable"
has "stopped construction state is visible" "machine stopped"
has "reset releases the construction latch" "machine configurable (not constructed)"
count=$(printf '%s\n' "$out" | grep -c 'main storage 1024 KB' || true)
if [ "$count" -eq 2 ]; then ok "reconstructed storage keeps requested size"; else bad "reconstructed storage keeps requested size"; fi

out=$(run config-show-pre-ipl) || true
has "show config works before IPL" "configuration editable"
has "show config includes fixed-disk mode" "disk0"
has "show config reports overlay accurately" "overlay"
has "show config includes the station definition" "0.0 role=console"

save_transcript=$(run config-save-stdout) || true
replay=$(printf '%s\n' "$save_transcript" | grep -E '^(set|attach) ')
out=$replay
if printf '%s\n' "$replay" | awk 'NF && $1 != "set" && $1 != "attach" { exit 1 }'; then
    ok "save config stdout emits commands only"
else
    bad "save config stdout emits commands only"
fi
has "saved configuration preserves overlay mode" "attach disk0 $SIM36_VOLUME overlay"
mux_listen_line=$(printf '%s\n' "$replay" | grep -n '^set terminal multiplex listen ' | cut -d: -f1)
mux_mode_line=$(printf '%s\n' "$replay" | grep -n '^set terminal multiplex off$' | cut -d: -f1)
if [ -n "$mux_listen_line" ] && [ -n "$mux_mode_line" ] && [ "$mux_listen_line" -lt "$mux_mode_line" ]; then
    ok "saved mux endpoint precedes listener activation"
else
    bad "saved mux endpoint precedes listener activation"
fi
station_order=$(printf '%s\n' "$replay" | grep '^set station .* role ' | awk '{print $3}' | tr '\n' ' ')
if [ "$station_order" = "0.0 0.1 0.2 " ]; then
    ok "saved stations have deterministic address order"
else
    bad "saved stations have deterministic address order"
fi

printf '%s\nshow config\nquit\n' "$replay" >"$TMP/replay.sim"
out=$("$SIM36" -c "$TMP/replay.sim" 2>&1) || true
has "stdout configuration is replayable" "memory                 512K"
has "replayed configuration retains overlay" "$SIM36_VOLUME  overlay"
has "replayed configuration retains machine model" "model                  5363"
has "replayed configuration retains CSP type" "csp type               advanced36 (virtual)"

mkdir "$TMP/nested"
printf 'save config saved.sim\nquit\n' >"$TMP/nested/write.sim"
out=$("$SIM36" -c "$TMP/nested/write.sim" 2>&1) || true
if [ -f "$TMP/nested/saved.sim" ]; then
    ok "save config resolves output relative to its command file"
else
    bad "save config resolves output relative to its command file"
fi
if out=$("$SIM36" -c "$TMP/nested/write.sim" 2>&1); then
    bad "batch save refuses to overwrite without confirmation"
else
    ok "batch save refuses to overwrite without confirmation"
fi
has "batch overwrite refusal explains the opt-in" "use --force"
printf 'set machine memory 256K\nsave config saved.sim --force\nquit\n' >"$TMP/nested/force.sim"
out=$("$SIM36" -c "$TMP/nested/force.sim" 2>&1) || true
if grep -q '^set machine memory 256K$' "$TMP/nested/saved.sim"; then
    ok "--force atomically replaces an existing configuration"
else
    bad "--force atomically replaces an existing configuration"
fi

instantiate command-file-error
if out=$("$SIM36" -c "$TMP/command-file-error.sim" 2>&1); then
    bad "batch errors return non-zero"
else
    ok "batch errors return non-zero"
fi
has "batch errors carry filename and line" "command-file-error.sim:2:"

# `reset` while the guest runs on the driver thread is refused; the driver
# arrives with milestone 6, and test/live-monitor.sh carries that check.

if out=$("$SIM36" -c test/refused/power-removed.sim 2>&1); then
    bad "power command is removed, not aliased"
else
    ok "power command is removed, not aliased"
fi
has "removed power command points at the new lifecycle" "unknown command 'power'"

out=$(printf 'help\nexit\n' | "$SIM36" 2>&1) || true
has "registry-generated help exposes configuration saving" "save config stdout|<file> [--force]"
has "registry-generated help exposes coherent execution" "start"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
