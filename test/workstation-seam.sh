#!/bin/sh
# The work station seam, as a pass/fail test.
#
# Everything here runs with no client attached, on purpose: what is being
# checked is that the guest's request reaches the backend and that the backend
# accounts honestly for what it could and could not do with it. Whether a
# 5250 client renders the bytes is a separate question, answered by attaching
# one - see docs/s36/workstation-backend.md.
#
# The path exercised is the real one: an IOB in guest storage, XR1, SVC 43
# through the control processor, VirtualWorkstation, WorkstationBackend.
set -e
# The reference's copy of this suite issues its first machine command before
# any machine exists and is refused; `ipl pause` constructs it first here.

cd "$(dirname "$0")/.."
. test/probe-common.sh


cat > "$TMP/ws.sim" <<'EOF'
ipl pause
boot
trace ws
wswrite console demo
wsinput console 00 00c3f1
wsread console
wsread console
wsinvite 0.1
stations
quit
EOF

instantiate default-machine
out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/ws.sim" 2>&1)


# The command reaches the device: SVC 43 is dispatched, the IOB address arrives
# in XR1 rather than as zero, and the model carries the request out.
#
# This used to assert "rejected", with a comment saying the command failed
# because nothing was attached to the slot. That reading was wrong, and the
# printer work found out why: the harness was leaving the IOB's class byte at
# +0x0A zero, so `wsprintr` refused every request through `wsifptr` before it
# ever looked at the command, and nothing reached a backend at all. The monitor
# reported that as "no session attached", which is what hid it. The harness now
# presents `C0` - the value `wsopend` finds on entry - and the request goes
# through, with the *backend* being the thing that has nowhere to put the bytes.
check "SVC 43 reaches the device through the control processor" \
      "SVC 43 (Delayed) iob 030000 -> completed"

# ... and the backend is asked for the bytes, and says it had nowhere to put
# them.  A station nobody has attached to is not a device error.
# Reference drift: the suite's original check expected `108 byte(s) dropped,
# no session attached`, but the console is an intrinsic attachment (it is
# present without a socket), so the record is accepted and accounted, not
# dropped.  The reference prints the accounting line below and fails its own
# check; SIM/36 asserts the behaviour it actually shares with the reference.
check "the data stream reaches the console backend and is accounted" \
      "out 1 record(s) 108 byte(s), 0 dropped; in 1 record(s) 3 byte(s)"

# The inbound half delivers a record to the guest side unchanged.
# The opcode and flags no longer cross the seam - they are RFC 1205 transport
# and the backend derives them - so what the model receives is the data stream
# alone. docs/s36/workstation-backend.md
check "an injected record is delivered to the device model" \
      "3 byte(s) of input data stream"
check "the queue drains once" \
      "nothing waiting"

# Sending with no session is refused rather than swallowed.
check "an invite with no session is dropped, not lost silently" \
      "invite dropped"

# The monitor reports the state the seam introduced.
check "stations reports the endpoint" "127.0.0.1:2300"
check "stations reports output accounting" "out 0 record(s) 0 byte(s), 1 dropped"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
