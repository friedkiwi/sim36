#!/bin/sh
# The MSP task dispatcher, and the supervisor calls that needed it: 00, 17, 1D,
# 1E, 24, 25 and 30.
#
# Every assertion below is on something the MODEL did not write: the instruction
# address the machine is executing at after a wait, the task block bytes a post
# leaves behind, and the condition code a call returns. The two that matter most
# are the round trip - task A waits, the machine starts executing task B's code
# at ITS OWN saved instruction address, B posts A, and the machine comes back to
# the instruction after A's supervisor call - and the halt at the end, where
# every task is waiting and nudspchA has nothing to select.
#
# docs/s36/task-dispatcher.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-dispatcher-vectors.py "$TMP/blocks.bin" "$TMP/code.bin"


cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0C00
loadfile $TMP/code.bin 1000
trace csp
# --- SVC 24: put task B on the priority queue and the ready list -------------
set xr1 0C00
set iar 1000
step 1
# --- SVC 1E: the IPL task waits, and the machine must go and run B ------------
set iar 1004
step 1
show cpu
# --- B's own SVC 1D posts A back, and the machine must return to A ------------
step 1
show cpu
# --- SVC 24 for the two quick-lock waiters. Both are waiting, so nuprqf10
#     leaves them off the ready list and they go on queue 39 only. -------------
set xr1 0CC0
set iar 1008
step 1
set xr1 0D80
set iar 100C
step 1
# --- SVC 30 QLOCK: the HIGHEST-PRIORITY waiter on this qlock is posted --------
set wr6 1234
set iar 1010
step 1
dump 0CC0 10
dump 0D80 10
# --- SVC 25: D is not in an event wait, so nupotkck returns -------------------
set xr1 0D80
set iar 1013
step 1
# --- SVC 17 on D: nuwasusp refuses, and the wait is DEFERRED into tb+6 --------
set xr1 0D80
set iar 1016
step 1
show cpu
dump 0D80 10
# --- SVC 17 on B: accepted, PSR Equal, and B leaves the ready list ------------
set xr1 0C00
set iar 101A
step 1
show cpu
dump 0C00 10
# --- SVC 00: the general wait mask, and the processor goes to C ---------------
set iar 101E
step 1
show cpu
dump 0F00 10
# --- C waits too, and now nothing at all is ready -----------------------------
step 1
show cpu
quit
EOF

out=$("$SIM36" -s "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# --- SVC 24 ------------------------------------------------------------------
# The priority walk outranks nothing on a queue whose only other member is the
# IPL task at FC, so both inserts land at the tail - which is where a priority
# insert that outranks nothing belongs.
check "24  B is queued on 39 and on 40     " 'SVC 0E: block 0C00 queued FIFO'
check "24  ...at the priority it asked for " 'SVC 24: task block 0C00 priority 40 + 4*0 -> tb+7 = 40'

# --- SVC 1E, the wait itself -------------------------------------------------
check "1E  TB_STAT2 is tb+5, and takes 02  " 'SVC 1E: nuwartn - task block 0F00 waits on TB_STAT2 (tb+5) = 02'
check "1E  the waiter leaves the ready list" 'block 0F00 dequeued'
check "1E  enables dispatching             " 'task dispatching was disabled and is now enabled'
check "1E  the dispatcher picks B          " 'SVC 1E: dispatch 0F00 -> 0C00'
# The proof: the machine is executing B's code, at the instruction address that
# was sitting in B's OWN request block, not anywhere the test set.
check "1E  the MSP runs B at rb+24 = 1100  " 'IAR 1100  ARR 0000  XR1 0F00'

# --- SVC 1D, the post --------------------------------------------------------
check "1D  B posts A                       " 'SVC 1D: task block 0F00 posted 02 - tb+6 = 00, TB_STAT2 (tb+5) = 00'
check "1D  nupotb readies the task         " 'SVC 1D: nupotb readies task block 0F00 - tb+4 80 -> 00'
check "1D  and the dispatcher goes back    " 'SVC 1D: dispatch 0C00 -> 0F00'
# ...to the instruction AFTER A's supervisor call, because nusvc had already
# advanced rb+24 past it. 1004 + 4 = 1008.
check "1D  A resumes past its SVC, at 1008 " 'IAR 1008'

# --- SVC 30 QLOCK ------------------------------------------------------------
check "30  the higher-priority waiter wins " 'SVC 30: qlock 1234 - task block 0CC0, priority 80'
check "30  ...and it is posted 20, general " 'SVC 30: task block 0CC0 posted 20'
# C: eyecatcher, ID 0003, tb+4 = 00 (no longer waiting), TB_STAT2 = 00,
# tb+6 = 00, priority 80, TB_WMASK = 0000 - the quick lock failure bit is gone.
check "30  C is fully released             " '000cc0  e3 c2 00 03 00 00 00 80 00 00'
# D still holds every one of those bits: 80 waiting, 20 general wait, 0002 mask.
check "30  D, lower priority, is untouched " '000d80  e3 c2 00 04 80 20 00 30 00 02'

# --- SVC 25 ------------------------------------------------------------------
check "25  not an event wait -> returns    " 'SVC 25: task block 0D80 is not in an event wait'

# --- SVC 17 ------------------------------------------------------------------
check "17  nuwasusp refuses on tb+14..15   " 'SVC 17: nuwasusp refuses task block 0D80 - the high byte of tb+14..15 is non-zero'
check "17  refused -> PSR High             " 'PSR 04'
check "17  the wait is deferred to tb+6    " 'nutkwt defers the wait into tb+6 = 02'
check "17  D now shows tb+6 = 02           " '000d80  e3 c2 00 04 80 20 02 30 00 02'
check "17  accepted for B                  " 'SVC 17: nuwartn - task block 0C00 waits on TB_STAT2 (tb+5) = 02'
check "17  accepted -> PSR Equal           " 'PSR 01'
check "17  B shows waiting, TB_STAT2 = 02  " '000c00  e3 c2 00 02 80 02 00 40 00 00'

# --- SVC 00 ------------------------------------------------------------------
check "00  TB_WMASK is tb+8..9, ORed in    " 'SVC 00: general wait on 0080 - TB_WMASK at tb+8..9 is now 0080'
check "00  the wait code is 20             " 'SVC 00: nuwartn - task block 0F00 waits on TB_STAT2 (tb+5) = 20'
check "00  A shows waiting, mask 0080      " '000f00  e3 c2 00 09 80 20 00 fc 00 80'
check "00  the processor goes to C at 1200 " 'SVC 00: dispatch 0F00 -> 0CC0'

# --- the no-task exit --------------------------------------------------------
check "--  everything waits -> the MSP halts" 'no task is ready'
check "--  and it names nudspchA's exit    " "nudspchA's no-task exit (c180e04c)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
