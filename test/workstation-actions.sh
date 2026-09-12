#!/bin/sh
# The IOB-command to action-code mapping, as a pass/fail test.
#
# What is asserted here is that the model carries IBM's THREE decoded tables
# unchanged - NuActiveCtl::dcdwscf's unit-address routing, NuActiveCtl::wscmd's
# command switch, and NuWsIoAction::executeRequest's jump table - and that it
# refuses everything outside them.
#
# > `wsioch` is not evidence. It is the model's only caller and it is ours, so
# > agreement between them is a tautology - see docs/s36/device-io.md. What this
# > suite is for is regression: the tables were read once, out of SLIC, and a
# > later edit that quietly changed one of them has to fail something.
#
# The one assertion here that is more than a regression guard is the
# cross-check: `wscmd` names an action code and `executeRequest`'s jump table
# names an operation for that code, and those are two independent decodes from
# two different places in the segment. The model compares them on every request
# and refuses a disagreement, so "they agree" is checked by the machine rather
# than by a human reading two tables.
set -e
cd "$(dirname "$0")/.."
. test/probe-common.sh

ln -s "$SIM36_VOLUME" "$TMP/as36.img"

cat > "$TMP/actions.sim" <<EOF
# Ports nothing else in this suite uses, so a developer with the emulator
# already running does not get a bind failure.
set machine model advanced36
set machine memory 512K
set machine ipl-type unattend
set machine ipl-source disk
attach disk0 as36.img ro
set station 0.0 role console
set station 0.0 device-code 11
set station 0.0 listen 127.0.0.1:12480
set station 0.1 role display
set station 0.1 device-code 11
set station 0.1 listen 127.0.0.1:12481
set station 0.4 role printer
set station 0.4 device-code PB
set station 0.4 listen 127.0.0.1:12484
ipl pause
boot
trace ws
wsioch 43 27 8
wsioch 43 A7 8
wsioch 42 40 8 04
wsioch 43 40 8
wsioch 43 C3 8
# RFC-1205 response body: two-byte cursor, then AID (Enter).  The previous
# F1C1C2 vector put the AID first, contrary to NuDsp5250::handleLowLevelAid.
wsinput 0.0 00 0000F1
wsioch 43 22 8
wsioch 43 62 8
wsioch 43 47 8 04
wsioch 43 21 8
wsioch 43 41 8
wsioch 43 FF 8
wsioch 43 27 8 FF
wsoutput 0.0 wtd
wswrite 0.0 C8C5D3D3D6
wsoutput 0.0 slic-display
wswrite 0.0 C8C5D3D3D6
wsoutput 0.0 passthrough
quit
EOF

"$SIM36" -c "$TMP/actions.sim" > "$TMP/out" 2>&1

check() {   # check <name> <fixed-string pattern>  (against $TMP/out)
    if grep -qF -- "$2" "$TMP/out"; then
        echo "  PASS  $1"
        pass=$((pass + 1))
    else
        echo "  FAIL  $1   (looked for: $2)"
        fail=$((fail + 1))
    fi
}


# --- NuActiveCtl::wscmd, arm by arm. The action code in each line is the one
#     wscmd loads into r4 and the NuWsIoAction constructor stores at +0x80.
check "27 -> action 4 put                    (wscmd c18bec40)" \
      "command 27 -> wscmd c18bec40 -> action code 4 -> Put"
check "A7 -> action 5 putWithInvite          (wscmd c18bec68)" \
      "command A7 -> wscmd c18bec68 -> action code 5 -> PutWithInvite"
check "40 -> action 12 clear                 (wscmd c18bed48)" \
      "command 40 -> wscmd c18bed48 -> action code 12 -> Clear"
check "C3 -> action 14 cancelInvite          (wscmd c18bec1c)" \
      "command C3 -> wscmd c18bec1c -> action code 14 -> CancelInvite"
check "62 -> action 7 readScreen             (wscmd c18bec8c)" \
      "command 62 -> wscmd c18bec8c -> action code 7 -> ReadScreen"
check "47 -> action 13 getPrinterStatus      (wscmd c18bed80)" \
      "command 47 -> wscmd c18bed80 -> action code 13 -> GetPrinterStatus"

# --- What the seam can actually carry out.
check "put sends the data stream to the station" \
      "put: 8 bytes from guest 020700 to station 0.0"
check "put with invite sends it and conditions the station" \
      "put with invite: 8 bytes from guest 020700 to station 0.0"
check "clear on a printer is end of job (RFC 2877 10.3)" \
      "Clear Printer -> 0.4 end of job"
check "clear on a DISPLAY is refused, not faked" \
      "Nudev5250::clear is a screen operation and IWorkStationBackend has no equivalent"
check "cancel invite stops the station reporting input" \
      "Cancel Invite -> 0.0 will not report input"

# --- What is decoded but has no host operation. Refusing beats inventing.
check "readScreen is decoded and refused rather than answered" \
      "action ReadScreen is decoded but not implemented"

# --- The read-back a filled sign-on screen needs. 22 dispatches to action 6.
#     The IOB and its ACE remain pending.  A raw TN5250 reply must never be
#     exposed as the guest result: NuDsp5250 parses cursor/AID/orders first.
check "22 -> action 6 readInputFields          (wscmd c18beb58)" \
      "command 22 -> wscmd c18beb58 -> action code 6 -> ReadInputFields"
check "readInputFields retains the guest IOB asynchronously" \
      "Read Input Fields PENDING: controller retained IOB 020600"
if grep -qF "COMPATIBILITY PASS-THROUGH" "$TMP/out"; then
    echo "  FAIL  raw TN5250 input was copied into guest storage"
    fail=$((fail + 1))
else
    echo "  PASS  raw TN5250 input is not exposed as a guest read result"
    pass=$((pass + 1))
fi

# --- SA21-9436 chapter 11's codes that the decoded dispatch does NOT contain.
#     21 Output Data and 41 Get Printer Status are in the manual's table of
#     contents and in no arm of wscmd; the operations are there under 27 and 47.
#     Mapping one onto the other on a one-nibble resemblance is exactly the kind
#     of guess this project keeps having to retract, so both are refused.
check "21, SA21-9436's Output Data, is REFUSED" \
      "command 21 (unknown) REFUSED"
check "41, SA21-9436's Get Printer Status, is REFUSED" \
      "command 41 (unknown) REFUSED"
check "the refusal names the decoded set rather than just failing" \
      "02 12 22 27 32 33 40 42 43 47 62 A7 C3"

# --- Invite is a unit ADDRESS, not a command. dcdwscf c18be7c8 tests wscf+1
#     against FF and branches to NuActiveCtl::invit before the command byte is
#     read, so FF in the command position is nothing and FF in the unit address
#     is an invite whatever the command byte says.
check "FF in the COMMAND byte is not a command" \
      "command FF (unknown) REFUSED"
check "FF in the UNIT ADDRESS is the invite, via dcdwscf" \
      "unit address FF -> NuActiveCtl::invit (dcdwscf c18be7c8)"
check "invite reaches every display slot and no printer" \
      "2 display slot(s) invited"

# --- The standalone WSDM compatibility adapter is a monitor-selected runtime
# mode, never a process option and never the default. It wraps raw EBCDIC text
# but preserves the original guest request accounting.
check "wtd mode wraps raw WSDM text in 5250 orders" \
      "WSDM text compatibility adapter wrapped 5 raw byte(s) as a 14-byte 5250 Write-To-Display"
check "output mode returns to diagnostic pass-through" \
      "station 0.0: output mode passthrough"
check "experimental SLIC renderer is selectable through the monitor" \
      "experimental SLIC Display::writeOnlyMessage rendered 5 WSDM byte(s) as 14 5250 byte(s)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
