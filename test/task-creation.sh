#!/bin/sh
# NuEmul::nuptask - task creation - reached the only way the architecture allows,
# through SVC 10 with Q bit 2, plus SVC 31 ATASK's general-wait failure arm.
#
# What is asserted is what the model does not decide for itself: the geometry of
# the one allocation nuptask makes (a 160-byte task block, then an action control
# element only when Q bit 7 asks for control back, then the request block), the
# task block bytes IBM's own listing writes, the identifier handed back in WR5,
# and - the two that matter - that the CALLER keeps the processor after an
# asynchronous transfer, and that the new task runs its own module at its own
# instruction address once something posts TB_STAT2 bit 0x40.
#
# docs/s36/svc-task-creation.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-task-creation-vectors.py \
    "$TMP/blocks.bin" "$TMP/code.bin" "$TMP/module.bin"


# The guest addresses below are DISCOVERED, not pinned. nuptask allocates its
# task block out of the system queue space, and that allocator is a binary buddy
# (docs/s36/system-queue-space.md) whose placement is not this suite's subject -
# pinning it here cost 27 assertions the moment the allocator became faithful,
# with no change in task-creation behaviour at all. So: run once to find the
# addresses, then assert every byte-level fact against them.
cat > "$TMP/discover.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0C00
loadfile $TMP/code.bin 1000
loadfile $TMP/module.bin 2000
trace csp
set iar 1000
step 1
set wr5 1234
step 1
quit
EOF
# No 5250 client is attached here, so the command file defines no listeners.
disc=$("$SIM36" -c "$TMP/discover.sim" 2>&1)
TASK1=$(echo "$disc" | grep -oE 'nuptask - task [0-9A-F]+ id 0002' | head -1 | awk '{print $4}')
# The trace names the requested WR5 between the id and the priority; the
# reference's copy of this regex predates that clause and fails on it.
RB1=$(echo "$disc"  | grep -A0 -oE 'task '"$TASK1"' id 0002 \(requested WR5 [0-9A-F]+\), priority FC, request block [0-9A-F]+' | grep -oE '[0-9A-F]+$')
TASK2=$(echo "$disc" | grep -oE 'nuptask - task [0-9A-F]+ id 1234' | head -1 | awk '{print $4}')
RB2=$(echo "$disc"  | grep -oE 'task '"$TASK2"' id 1234 \(requested WR5 [0-9A-F]+\), priority FC, request block [0-9A-F]+' | grep -oE '[0-9A-F]+$')
if [ -z "$TASK1" ] || [ -z "$RB1" ] || [ -z "$TASK2" ]; then
  echo "  could not discover the allocated task blocks - nuptask did not run"; exit 1
fi
# lower-case forms, because `dump` labels its rows in lower case
task1l=$(echo "00$TASK1" | tr 'A-Z' 'a-z'); rb1l=$(echo "00$RB1" | tr 'A-Z' 'a-z')
task2l=$(echo "00$TASK2" | tr 'A-Z' 'a-z')
# The ACE sits at task block + 160, immediately after the 160-byte task block
# (c18a609c), so it is computed rather than discovered - that offset IS one of
# the facts under test.
ACE2=$(printf '%04X' $(( 0x$TASK2 + 160 )))
ace2l=$(echo "00$ACE2" | tr 'A-Z' 'a-z')
ace2hi=$(printf '%02x' $(( (0x$TASK2 + 160) / 256 )))
ace2lo=$(printf '%02x' $(( (0x$TASK2 + 160) % 256 )))
tb2row=$(printf '%04X' $(( 0x$TASK2 + 0x10 )) | tr 'A-Z' 'a-z')

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0C00
loadfile $TMP/code.bin 1000
loadfile $TMP/module.bin 2000
trace csp
# --- SVC 10 Q bit 2: the asynchronous transfer builds a whole new task --------
set iar 1000
step 1
show cpu
dump $TASK1 10
dump $RB1 08
# --- and again with Q bit 7, which adds an action control element -------------
# WR5 is an INPUT as well as an output (c18a5f44): a non-zero WR5 names the
# identifier the caller wants, and the first call left its answer sitting there.
# Ask for hex 1234 explicitly, so both directions of that halfword are covered.
set wr5 1234
step 1
dump $TASK2 20
dump $ACE2 08
# --- SVC 1D: post the first new task on TB_STAT2 bit 40, as nucready does -----
set pxr1 00
set xr1 $TASK1
step 1
dump $TASK1 08
# --- SVC 31 four times over, to use the task work area up ---------------------
set wr6 FF00
set iar 1010
step 1
set iar 1010
step 1
set iar 1010
step 1
set iar 1010
step 1
# --- and a fifth with Q bit 7, which is nuatask's general wait ----------------
set iar 1013
step 1
show cpu
dump 0F00 10
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# --- the transfer reaches nuptask at all -------------------------------------
check "10  Q bit 2 is the async arm        " 'entry point 0, Q=20 no-return async'
check "10  the target is already resident  " 'SVC 10: sector 5 is already resident, program block 0C40'

# --- the allocation's geometry -----------------------------------------------
# nuprblen makes the request block six units; nuptask adds ten for the task
# block and nothing else, because Q bit 7 is off on this first call.
# The buddy CLASS a request is carved from depends on what else is in the pool,
# so it is not a fact about task creation; the size and the address are.
check "nuptask  16 units, no ACE           " "sqs: assign 256 -> 256 bytes at guest $TASK1"
check "nuptask  the task block is where told" "SVC 10: nuptask - task $TASK1 id 0002"
check "nuptask  ...and its rb 160 bytes on " "request block $RB1 (6 unit(s))"

# --- the task block bytes, none of which the test wrote ----------------------
# The task block: E3C2 "TB", id 0002, tb+4..5 = 8040 - WAITING on program-not-ready,
# tb+7 = FC the priority, tb+13 = FA the long-wait mark nuptask sets to 250.
check "tb  eyecatcher, even id, 8040 state " "$task1l  e3 c2 00 02 80 40 00 fc"
check "tb  ...and tb+13 = FA, 250 decimal  " '00 00 00 00 00 fa 00 00'
check "tb  ...priority FC, floor FC        " "priority FC, request block $RB1"
check "nuptask  the wait is TB_STAT2 bit 40" 'tb+4..5 = 8040, so it is WAITING on TB_STAT2 bit 40'
# nuptask requeues by priority (flags C7, no LIFO) onto queue 39, whose only
# other member is the IPL task at the SAME priority FC. A FIFO-by-priority insert
# lands AFTER blocks of equal priority (SA21-9436 3-86, nuquecs c18e0724), so the
# new task goes to the tail - the same rule the dispatcher suite asserts.
check "nuptask  queued on the task queue 39" "SVC 0E: block $TASK1 queued FIFO"
check "nuptask  ...and on the 0F1F chain   " 'the chain at guest 0F1F'
check "nuptask  it claims the program block" 'nuptask claims program block 0C40 for the new task'

# --- nuprbbld, into the SAME allocation --------------------------------------
# The request block: "RB", +2 = 06 units, +3..5 = 000000. nuprbbld stores the
# CALLER's frame (0E00) into rb+3..5 at c18a6298, but nuptask ZEROES it again in
# the four instructions right after (c18a611c-c18a6128: ADDI 27,0,0 ; ORI 3,29,0 ;
# ADDI 8,3,3 ; STSDI 27,8,3). A new task's ROOT frame has no caller to walk back
# to - Q bit 7 hands the requester back through the return ACE at tb+17..19, and
# the zero is what routes the task's SVC 11 root exit to nupexit's prev==0 ->
# nupterm arm (c18a4054). docs/s36/nupterm-async-exit.md
check "rb  eyecatcher, 6 units, root prev 0 " "$rb1l  d9 c2 06 00 00 00"
check "rb  the new TASK is relinked, not us" "SVC 10: transferred to 2000 - module at 2000, program block 0C40, task block $TASK1, request block $RB1"

# --- and the CALLER keeps the processor --------------------------------------
# 1000 + 6 = 1006, the instruction after the SVC, not 2000.
# The child is now built WAITING and readied later by nucready (deferred), so the
# caller keeps the processor and resumes at 1006 (the next check). This asserts the
# deferral itself; "keeps the processor" is what 1006 proves.
check "10  the child is deferred, caller runs " 'waiting on TB_STAT2 40 until nucready posts it'
check "10  ...and resumes at 1006          " 'IAR 1006'

# --- the second call: Q bit 7 buys an action control element ------------------
check "10  Q bit 7 adds two units          " "sqs: assign 288 -> 512 bytes at guest $TASK2"
check "10  ...an ACE at task block + 160   " "action control element at $ACE2 and stores it at tb+17..19"
check "tb  +17..19 names the ACE at 81A0   " "00$tb2row  00 00 $ace2hi $ace2lo"
check "ace  C1C3 at 81A0, tb 0F00, prio FC " "$ace2l  c1 c3 00 00 00 fc"
# WR5 named 1234 on the way in, so nuptask uses it verbatim (c18a5f44) instead of
# stepping the counter - which is the input half of the same halfword.
check "10  WR5 in names the task's id      " "nuptask - task $TASK2 id 1234"
check "tb  ...and it is in tb+2..3         " "$task2l  e3 c2 12 34 80 40 00 fc"

# --- SVC 1D posts TB_STAT2 bit 40, which is what nucready does ---------------
check "1D  the program-not-ready wait ends " "SVC 1D: task block $TASK1 posted 40 - tb+6 = 00, TB_STAT2 (tb+5) = 00"
check "1D  nupotb readies the new task     " "SVC 1D: nupotb readies task block $TASK1 - tb+4 80 -> 00"
check "tb  no longer waiting               " "$task1l  e3 c2 00 02 00 00 00 fc"

# --- SVC 31's failure arm, and the wait it takes -----------------------------
check "31  the task work area runs out     " 'no 2042 sector(s) of task work area'
check "31  ...with getHeap's own code 4000 " 'returns 0x800000 | code = 804000'
check "31  Q bit 7 takes nuatask to nugwaitc" 'bit 7 is set, which takes nuatask to nugwaitc (c18bb5c4) with the mask 4000'
check "31  nugwaitc sets tb+4 bit 0C       " 'nugwaitc - tb+4 |= 0C and TB_WMASK at tb+8..9 is now 4000'
check "31  the wait code is the general 20 " 'SVC 31: nuwartn - task block 0F00 waits on TB_STAT2 (tb+5) = 20'
check "31  ...and dispatching is enabled   " 'task dispatching was disabled and is now enabled'
# The IPL task's own bytes: tb+4 = 8C (waiting | nugwaitc's 0C), tb+5 = 20 the
# general wait, tb+8..9 = 4000 - "task work area allocate failure", SA21-9436 3-69.
check "31  the IPL task carries 8C 20 4000 " '000f00  e3 c2 00 09 8c 20 00 fc 40 00'

# --- the pay-off: the waiting caller hands the processor to the new task -----
# nucready readies the deferred async child at this wait (the same yield the work
# queue services on), and the dispatcher hands the processor to it. It enters its
# own module at 2000. The specific child depends on ready-queue order, so assert the
# readied dispatch and the entry, not a pinned task id.
check "31  a readied child gets the processor" 'nucready c1897760 (deferred async transfer completion): nupotb readies task block'
check "31  ...which enters ITS OWN module  " 'IAR 2000'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
