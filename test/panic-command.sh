#!/bin/sh
# The panic command is interactive host control: it captures first and then
# terminates the command loop without consuming another monitor command.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh

# A configurable (not constructed) session: no volume is needed for this half,
# but the volume gate is shared so the whole gate skips together.
out=$(printf '%s\n' \
    panic \
    'W2 stopped accepting input after MAIN option 1' \
    'IPL unattended, connect W2, sign on, enter 1' \
    help |
    TMPDIR="$TMP" "$SIM36" -c test/advanced36-nolisten.sim 2>&1)

printf '%s\n' "$out" | grep -qF 'What happened? '
printf '%s\n' "$out" | grep -qF 'How can it be reproduced? '
printf '%s\n' "$out" | grep -qF 'panic dump created: '

dump=$(printf '%s\n' "$out" | sed -n 's/.*panic dump created: //p')
if [ -z "$dump" ] || [ ! -f "$dump" ]; then
    echo "panic command did not leave the reported dump file" >&2
    exit 1
fi

[ "$(stat -c %a "$dump")" = 600 ] || {
    echo "panic dump is not owner-only" >&2
    exit 1
}
unzip -t "$dump" >/dev/null
entries=$(unzip -Z1 "$dump")
for required in README.txt operator/what-happened.txt \
                operator/how-to-reproduce.txt config/config.sim \
                stations/chassis-listeners.txt; do
    printf '%s\n' "$entries" | grep -qxF "$required" || {
        echo "panic dump is missing $required" >&2
        exit 1
    }
done
[ "$(unzip -p "$dump" operator/what-happened.txt)" = \
   'W2 stopped accepting input after MAIN option 1' ]
[ "$(unzip -p "$dump" operator/how-to-reproduce.txt)" = \
   'IPL unattended, connect W2, sign on, enter 1' ]
# Configuration may name mounted media so a report can be reproduced; mounted
# images, tape members and overlay sectors must never be archived. Bounded
# last-read operation buffers are intentionally allowed under io/.
if printf '%s\n' "$entries" | grep -Eq '(^|/)(media|overlay)(/|$)|disk[0-9]+\.img$|diskette[0-9]+\.img$'; then
    echo "panic dump contains media payload" >&2
    exit 1
fi

# `panic` owns termination. The line after its two answers must not return to
# command dispatch (and therefore must not print registry-generated help).
if printf '%s\n' "$out" | grep -q '^session:$'; then
    echo "panic returned to the monitor command loop" >&2
    exit 1
fi

# A constructed machine contributes the volatile evidence that motivated this
# command. Plain IPL also verifies that panic can join the live guest thread;
# the exact instruction boundary is intentionally not asserted.
instantiate default-machine
live_out=$(printf '%s\n' \
    ipl \
    panic \
    'machine-state capture test' \
    'IPL, then panic' |
    TMPDIR="$TMP" "$SIM36" -c "$TMP/default-machine.sim" 2>&1)
live_dump=$(printf '%s\n' "$live_out" | sed -n 's/.*panic dump created: //p')
if [ -z "$live_dump" ] || [ ! -f "$live_dump" ]; then
    echo "panic did not create a dump for a constructed machine" >&2
    exit 1
fi
live_entries=$(unzip -Z1 "$live_dump")
for required in runtime/main-storage.bin runtime/runtime.bin \
                runtime/summary.txt runtime/tasks.txt runtime/csp-state.txt \
                runtime/device-state.txt runtime/scheduler.txt \
                io/fixed-disk-last-read.bin io/diskette-last-read.bin \
                io/tape-last-read.bin \
                stations/0_0/console-screen-ebcdic.bin \
                stations/0_0/console-fields.txt \
                stations/0_0/terminal-queues.txt; do
    printf '%s\n' "$live_entries" | grep -qxF "$required" || {
        echo "constructed-machine panic dump is missing $required" >&2
        exit 1
    }
done
# The archive carries the whole host backing store, which for the Advanced/36
# is the 16 MB dump-sized space, not the 1 MB SSP reports as installed.  (The
# reference's copy of this gate still expects 1 MB and fails against the
# reference itself; the value below is what the reference actually writes.)
[ "$(unzip -p "$live_dump" runtime/main-storage.bin | wc -c)" -eq 16777216 ] || {
    echo "panic dump did not contain the complete backing main storage" >&2
    exit 1
}
[ "$(unzip -p "$live_dump" stations/0_0/console-screen-ebcdic.bin | wc -c)" -eq 1920 ] || {
    echo "panic dump did not contain the complete console screen" >&2
    exit 1
}

echo "panic command prompt/capture/exit PASS"
