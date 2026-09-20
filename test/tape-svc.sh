#!/bin/sh
# A guest tape I/O round-trips through the backend via the REAL device-SVC
# path. This is the tape analogue of workstation-seam.sh: it does not test the
# storage seam in isolation (tape.sh does that), it proves that an SVC 46
# issued through the control processor reaches the virtual tape, decodes the
# tape IOB, drives the tape backend, and posts the completion the guest reads
# back.
#
# The path exercised is the real one: an IOB in guest storage, XR1, SVC 46
# through the control processor, the virtual tape, the folder backend. The
# monitor's `tapesvc` diagnostic builds the IOBs and issues the SVCs;
# positioning between data ops uses the backend's own REWIND/SPACE (the label
# layer's operations, not SVC 46 opcodes).
set -u
cd "$(dirname "$0")/.."
. test/probe-common.sh

instantiate default-machine
cat > "$TMP/tsvc.sim" <<EOF
ipl pause
boot
tapesvc $TMP/tape
quit
EOF

out=$("$SIM36" -c "$TMP/default-machine.sim" -s "$TMP/tsvc.sim" 2>&1)

echo "-- SVC 46 tape I/O through the control processor --"
# The drive is mounted and the SVC actually reaches it.
check "the cartridge mounts on the drive     " "mount the cartridge on the drive           PASS"
check "SVC 46 read is dispatched to the tape  " "SVC 46 (Delayed) iob 000600 -> completed"
check "native command 01 activates the drive   " "native command 01 activates the loaded tape PASS"
check "native command 02 establishes a session " "native command 02/03 establishes its session without movement PASS"
check "native command 13 reads the volume label" "native command 13 rewinds and reads VOL1"
# A read moves a real block into guest storage and posts complete.
check "read posts complete (ECM 0x40)        " "SVC 46 read posts complete (iob+0x06 bit 0x40) PASS"
check "the block lands in guest storage       " "the block reached guest storage (LastRead 80 bytes) PASS"
check "it is the EBCDIC VOL1 label            " "it is the EBCDIC VOL1 label in the guest buffer PASS"
check "native command 16 locates the dataset  " "native command 16 finds HDR1 and positions at the data file PASS"
check "the selected dataset is readable       " "the next guest read returns the selected dataset's first block PASS"
# A read on the tape mark is a distinct, non-success completion.
check "the tape mark posts non-success        " "SVC 46 read on the tape mark posts non-success PASS"
check "dataset-not-found has guest status     " "native command 16 reports the #CATP dataset-not-found status PASS"
# A write through SVC 46 reaches the backend, and reads back verbatim.
check "write posts complete                   " "SVC 46 write posts complete                PASS"
check "the written record reads back verbatim " "SVC 46 read back returns the written record verbatim PASS"
check "an overlength record is not truncated    " "an overlength block is rejected without changing guest data PASS"
check "the rejected record remains retryable   " "and remains positioned for a retry"
# Status edges: a bad command is refused, an empty drive answers not-ready.
check "an invalid command is refused          " "SVC 46 with an invalid command is refused  PASS"
check "native command 12 initializes the tape " "native command 12 writes VOL1, two marks, and rewinds PASS"
check "the initialized stream is readable     " "the initialized logical stream is readable PASS"
check "native command 14 writes header labels" "native command 14 replaces the terminal mark with four header labels and a mark PASS"
check "the guest labels retain their blocks   " "the four guest labels read back with their block boundaries PASS"
check "native command 19 closes the data set  " "native command 19 closes data and writes the trailer-label file PASS"
check "the trailer layout round-trips         " "the data and four trailer labels retain their tape-file boundaries PASS"
check "native command 1B finalizes the volume" "native command 1B writes the second terminal mark PASS"
check "the volume has two terminal marks      " "the finalized volume ends in two consecutive marks PASS"
check "native command 27 unloads the tape     " "native command 27 unloads and flushes the tape PASS"
check "an empty drive answers not-ready       " "SVC 46 read on an empty drive answers not-ready PASS"
# The whole native tally, so a regression points at itself.
check "the tapesvc round-trip is all green    " "tape SVC: 28 passed, 0 failed"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
