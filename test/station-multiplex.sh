#!/bin/sh
# The station multiplexer: one port, a menu, a handover, and a
# lifetime that is longer than the machine's.
# docs/s36/station-multiplexer-design-2026-09-08.md
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here"
. test/probe-common.sh
instantiate default-machine
defaults=$(cd "$here" && printf 'save config stdout\nquit\n' | "$SIM36" -c "$TMP/default-machine.sim" 2>&1)
out=$(cd "$here" && python3 test/station-multiplex.py 2>&1)

check_absent() {
    if printf '%s\n' "$out" | grep -qF "$2"; then
        echo "  FAIL  $1   (unexpected: $2)"; fail=$((fail + 1))
    else
        echo "  PASS  $1"; pass=$((pass + 1))
    fi
}
check_default() {
    if printf '%s\n' "$defaults" | grep -qF "$2"; then
        echo "  PASS  $1"; pass=$((pass + 1))
    else
        echo "  FAIL  $1   (looked for: $2)"; fail=$((fail + 1))
    fi
}

# --- shipped default -------------------------------------------------------
check_default "the multiplexer is the default display transport" \
      "set terminal multiplex on"
check_default "and its endpoint has a default so it needs no second command" \
      "set terminal multiplex listen 127.0.0.1:2300"

# --- one listener, and it says what it took over ---------------------------
check "one listener is announced, and it says how many stations it serves" \
      "terminal multiplex listening on 127.0.0.1:3900; 6 display stations available"
check "a per-station listener is reported as disabled, not silently dropped" \
      "station 0.1: per-station listener disabled (terminal multiplex is on)"

# --- the menu --------------------------------------------------------------
check "the title is centred on row 1" \
      "                                     SIM/36"
check "the volume is shown by basename, with its size and its mode" \
      "Drive 1: as36.img  200M overlay"
check "the prompt defaults to the next available station and names the required notation" \
      "Connect to workstation . . .  0.0      (port.address)"
check "the client finds the prompt as a real 5250 input field" \
      "Connect to workstation   '0.0   '"
check "a running machine says so, at the bottom right" \
      "Machine status: running"

# --- selection and handover ------------------------------------------------
check "typing 0.1 lands the client on station 0.1" \
      "0.1  device 11  mux 127.0.0.1:3900   attached"
check "and the console is untouched by any of it" \
      "0.0  device 11  127.0.0.1:0          attached  (console)"

# --- the direct-drop path --------------------------------------------------
check "a DEVNAME of 0.2 never sees a menu" \
      "phase 1 direct-drop client saw a menu: no"
check "and lands straight on station 0.2" \
      "0.2  device 11  mux 127.0.0.1:3900   attached"
check "the station records the device name the client arrived with" \
      "devname 0.2"

# --- console selection and refusals ---------------------------------------
check_absent "W1 attaches without a confirmation prompt" \
      "Connecting to system console - continue?"
check "selecting W1 attaches it to the real console backend" \
      "=== phase 1 attached system console ==="
check_absent "the obsolete clipped console-refusal message is gone" \
      "driven from the monitor, not over telnet"
check "an attached W1 is not also echoed through the stdio console" \
      "phase 1 attached W1 stdio echo: suppressed"
check "stdio console output resumes after the W1 client disconnects" \
      "phase 1 released W1 stdio echo: resumed"
check "a station that already has a client is refused by name" \
      "0.1 already has a client attached"
check "and the default moves on to the next station that is actually free" \
      "Connect to workstation . . .  0.0      (port.address)"

# --- it scales past the seven-station default ------------------------------
check "a ten-station, two-controller machine is served by one listener" \
      "terminal multiplex listening on 127.0.0.1:3910; 9 display stations available"
check "the hint states the only accepted notation" \
      "(port.address)"
check "a second controller is selected by its explicit address" \
      "1.0  device 11  mux 127.0.0.1:3910   attached"

# --- the lifetime is the setting's, not the machine's ----------------------
check "the menu paints with no machine constructed at all" \
      "Machine status: stopped"
check "a client can take a station before there is a machine to take it from" \
      "Attached to 0.1. Waiting for IPL to construct the machine."
check "the prefilled station is marked modified for ordinary 5250 clients" \
      "phase 3 prefilled selection MDT: on"
check "an IBM data-stream error does not start a selector repaint loop" \
      "phase 3 output-error response caused selector repaint: no"
check "IPL construction does not hang up on it" \
      "phase 3 socket survived IPL construction"
check "and it is on the station it picked, in the machine that came after" \
      "0.1  device 11  mux 127.0.0.1:3930   attached"
# The point of all of it: a station attached before the IPL is present when the
# guest looks for it.  This line is the GUEST's own bind, not the host's.
check "so the guest binds it during the IPL, as an already-present station" \
      "station 0.1: session bound to TU"
check "the normal completion message is concise" \
      "station 0.1: bind/powerOn complete for TU"
check_absent "normal station messages do not expose the TFRM36 implementation path" \
      "TFRM36 action"

# --- preconnected attended system console ---------------------------------
check "W1 can be selected and parked before machine construction" \
      "phase 4 W1 parked before IPL: yes"
check "that same socket receives the real attended SIGN ON panel after IPL" \
      "phase 4 preconnected W1 received attended SIGN ON: yes"
check "Overrides Y is accepted through the TN5250 console" \
      "phase 4 Overrides Y reached date/time confirmation: yes"
check "a bare confirmation Enter retains fields and opens IPL OVERRIDES MENU" \
      "phase 4 bare confirmation Enter reached IPL OVERRIDES MENU: yes"
check "override option 2 displays the programs selected for IPL" \
      "phase 4 option 2 displayed the IPL programs: yes"
check "the programs panel leaves the terminal keyboard usable" \
      "phase 4 programs panel left keyboard usable: yes"
check "accepting the programs and exiting overrides completes attended IPL" \
      "phase 4 option 1 completed attended IPL: yes"
check "Cmd3 after the completed IPL also reaches SSP" \
      "phase 4 Cmd3 received an SSP response: yes"

# --- preconnected unattended system console -------------------------------
check "the multiplexer paints nothing over an unattended W1 once constructed" \
      "phase 5 W1 multiplexer panel after construction: absent"
check "the guest itself drives the unattended W1 after construction" \
      "role console  tub 00"
check "W1 handoff does not fabricate a terminal response for the guest" \
      "in 0 record(s) 0 byte(s), 0 waiting; 1 session(s)"

# --- printer topology prefill ---------------------------------------------
check "the shipped topology initially prefills W1" \
      "phase 6 first prefilled station: 0.0"
check "the next selector skips attached W1 and printer 0.1" \
      "phase 6 second prefilled station after W1 and printer: 0.2"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
