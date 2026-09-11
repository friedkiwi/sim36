#!/bin/sh
# SVC 20 Specific Resource Dequeue and SVC 21 Resource Enqueue/Dequeue.
#
# Two runs. The first drives one supervisor call at a time from the monitor and
# asserts on the BYTES the machine leaves in guest storage - the allocation
# queue element it built, the resource queue header it threaded, and the two
# chains it hung the element on. The second is a three-task round trip: A takes
# the resource exclusively, B asks for it and waits, C posts A back, A gives the
# resource up, and the assertion is that B is POSTED and resumes past its own
# supervisor call owning the resource.
#
# Nothing here asserts a value the model chose. The AQE offsets come from SLIC,
# the share-level arithmetic comes from SA21-9436 3-107's own table, and the
# queue header address 0C03 is the manual's own worked example on 3-108.
#
# docs/s36/svc-resource-allocation.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-resource-vectors.py "$TMP/blocks.bin" "$TMP/code.bin"


cat > "$TMP/one.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0B00
loadfile $TMP/code.bin 1000
trace csp
# --- an exclusive enqueue on an empty queue succeeds --------------------------
set xr2 0C03
set iar 1000
step 1
show cpu
dump FFF0 16
dump 0C01 3
dump 0F35 3
# --- the same task enqueueing again gets the SAME element (3-106) -------------
set xr2 0C03
set iar 1004
step 1
dump FFF0 16
# --- ...and a different level just changes the level in it -------------------
set xr2 0C03
set iar 1008
step 1
dump FFF0 16
# --- dequeue: off both queues, and the header is empty again -----------------
set xr2 0C03
set iar 100C
step 1
show cpu
dump 0C01 3
dump 0F35 3
# --- dequeue again, with nothing queued --------------------------------------
set xr2 0C03
set iar 1010
step 1
show cpu
# --- the device bit on a plain header is the architected enqueue (nursenqc
# c18929ac -> nursenqc2 c1892e38, bit stripped at c1892ed0); dequeue it again
# so the queue is empty for what follows ------------------------------------
set xr2 0C03
set iar 1014
step 1
set xr2 0C03
set iar 100C
step 1
# --- the device bit with a DA eyecatcher AT XR2 (type 01, the host jump
# table) is the refusal; a refused call stops the processor, so restart it ---
poke 0C60 C4 C1 01 80 00 00 00 00 00 00 00 00 00 00 00 00
set xr2 0C60
set iar 1030
step 1
set xr2 0C03
set iar 1018
step 1
# 1018 (Q bit 2, critical system resource) now ENQUEUES rather than refusing. On
# this uncontended queue nuprtup finds no owner above the caller, so no priority
# is raised. Dequeue the element it left (100C is the matching dequeue) so the
# nested tests below see an empty 0C03, the way the old no-op refusal left it.
set iar 100C
step 1
# --- a nested enqueue owned by a JCB, then a second nested over it ------------
set xr1 0B00
set xr2 0C03
set iar 101C
step 1
dump 0BD3 3
set xr1 0B00
set xr2 0C03
set iar 1020
step 1
dump 0C01 3
# --- SVC 20 rebuilds the job's active share levels ---------------------------
set xr1 0B00
set xr2 0C03
set iar 1024
step 1
# --- and refuses when the job holds nothing for that resource ----------------
set xr1 0C40
set xr2 0C03
set iar 1027
step 1
quit
EOF

cat > "$TMP/two.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0B00
loadfile $TMP/code.bin 1000
trace csp
set xr1 0C40
set iar 1040
step 1
set xr1 0D00
set iar 1044
step 1
# --- A takes the resource, exclusively ---------------------------------------
set xr2 0C03
set iar 1048
step 1
show cpu
# --- A waits, and the processor goes to B ------------------------------------
step 1
show cpu
# --- B asks for the same resource and cannot have it: it waits ---------------
set xr2 0C03
step 1
show cpu
dump 0C40 10
# --- C posts A, and A comes back ---------------------------------------------
step 1
show cpu
# --- A gives the resource up. B can now share, so B is posted ----------------
set xr2 0C03
step 1
dump 0C40 10
# --- A waits again; B resumes PAST its own supervisor call, owning it --------
step 1
show cpu
# --- B dequeues, and finally everything waits --------------------------------
set xr2 0C03
step 1
show cpu
step 1
step 1
quit
EOF

cat > "$TMP/term.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/blocks.bin 0B00
loadfile $TMP/code.bin 1000
trace csp
# A owns the manual's resource, then ends at its root.  nupterm must use the
# ordinary dequeue path: both chains become empty even though this focused
# vector has no transfer table and cannot run nupterm's later slot-4 continuation.
set xr2 0C03
set iar 1000
step 1
set iar 102A
step 1
dump 0C01 3
dump 0F35 3
quit
EOF

out=$("$SIM36" -s "$TMP/one.sim" 2>&1)
out="$out
$("$SIM36" -s "$TMP/two.sim" 2>&1)
$("$SIM36" -s "$TMP/term.sim" 2>&1)"
[ -n "$VERBOSE" ] && echo "$out"


echo "$out" > "$TMP/out.txt"

# --- the element itself ------------------------------------------------------
# c1 d8 'AQ'; +2..4 resource chain 000000, the only element; +5..7 owner 000F00,
# the IPL task block; +8 active 84 = owner(80) + stored level 4, which is
# architected level 3; +9..11 owner chain; +12 requested 84; +13..15 000C03.
check "21  the element carries the AQ eyecatcher" '00fff0  c1 d8 00 00 00 00 0f 00 84 00'
check "21  ...and its owner, level and resource  " '00fff0  c1 d8 00 00 00 00 0f 00 84 00 00 00 84 00 0c 03'
check "21  the resource queue header points at it" '000c01  00 ff f0'
check "21  ...and so does the task block, tb+53   " '000f35  00 ff f0'
check "21  an uncontended enqueue is Equal        " 'owns resource queue 000C03 - Equal'

# --- 3-106: one element per task per resource --------------------------------
check "21  a second enqueue reuses the element    " 'the caller already holds AQE 00FFF0'
check "21  level 0 replaces level 3 in it         " '00fff0  c1 d8 00 00 00 00 0f 00 81 00'

# --- dequeue -----------------------------------------------------------------
check "21  dequeue frees the element, 16 bytes    " 'AQE 00FFF0 dequeued from resource queue 000C03'
check "21  ...the header is empty again           " '000c01  00 00 00'
check "21  ...and so is the task block chain      " '000f35  00 00 00'
check "21  dequeue is Equal when it found one     " "dequeue removed the caller's element - condition 01"
check "21  ...and nonequal when it did not        " 'found no element for this caller - condition 02'

# --- the device bit: dispatch on the eyecatcher at XR2 (nursenqc c1892998) ----
check "21  device bit on a plain header proceeds  " 'has the device bit (0x04) but the halfword at XR2 000C03 is 0000, neither PU (D7E4) nor DA (C4C1): nursenqc c18929ac -> nursenqc2 c1892e38'
check "21  ...as an architected enqueue           " 'SVC 21: enqueue on resource queue 000C03 - inline 1 80 (level 0), Q 00, owner task block 000F00'
check "21  device bit with DA at XR2 refuses      " 'the halfword at XR2 000C60 is C4C1 ("DA"): nursenqc c18929e4 switches on DA type 01'
check "21  critical system resource now enqueues   " 'inline 1 83 (level 3), Q 20, owner task block 000F00'

# --- nesting and SVC 20 ------------------------------------------------------
check "20  the JCB AQE queue is jcb+211..213      " '000bd3  00 ff 70'
check "21  a nested enqueue displaces the first   " 'takes the head of the queue'
check "20  it walks the job's chain               " 'rebuild the active share levels of JCB 000B00'
# nurscomp takes its FLAGS from the element's own requested byte, so the 0x80
# the evaluation walk left there comes back as the ownership bit - which is why
# both elements end 94 and the manual can say the LAST one is the owner.
check "20  ...rewrites the element it nested over " 'SVC 20: AQE 00FF70 active level 34 -> 94'
check "20  ...and makes the LAST one the owner    " 'SVC 20: 2 element(s) rebuilt; AQE 00FFD0 is now the owner (94)'
check "20  a job with nothing queued is refused   " 'holds no allocation queue element for resource'
check "20  ...naming nuersvc, not guessing        " 'NuEmul::nuersvc(90, 0) here (c18928f8)'

# --- the round trip ----------------------------------------------------------
check "21  A takes it exclusively                 " 'SVC 21: AQE 00FFF0 owns resource queue 000C03 - Equal'
check "21  B cannot share and waits on 10         " 'cannot share - the caller waits on TB_STAT2 10, resource enqueue'
check "21  ...which is the resource enqueue wait  " 'SVC 21: nuwartn - task block 0C40 waits on TB_STAT2 (tb+5) = 10'
check "21  ...and B leaves the ready list         " 'SVC 21: dispatch 0C40 -> 0D00'
# B's task block: TB, ID 0002, tb+4 = 80 waiting, TB_STAT2 = 10 resource enqueue.
check "21  B's task block shows the wait          " '000c40  e3 c2 00 02 80 10 00 40 00 00'
check "21  A's dequeue hands the resource to B    " 'can share now - task block 0C40 is posted'
check "21  ...through nupotcb(tb, 16)             " 'nupotcb(tb, 16) c18936e4'
check "21  B is readied by that post              " 'SVC 21: task block 0C40 posted 10'
check "21  ...and its wait bits are gone          " '000c40  e3 c2 00 02 00 00 00 40 00 00'
# The proof that a waiter resumes PAST its call: B's SVC 21 is at 1100 and is
# four bytes long, so B comes back at 1104 - it does not re-issue the enqueue.
check "21  B resumes past its own call, at 1104   " 'IAR 1104'
check "21  ...and its dequeue then succeeds       " "SVC 21: dequeue removed the caller's element"
check "--  everything waits -> the MSP halts      " 'the ready list (system queue 40) is empty'
check "--  ...at nudspchA's own no-task exit      " "no-task exit at c180e04c"

# --- nupterm cleanup ---------------------------------------------------------
check "11  nupterm walks the task-owned AQE queue " 'SVC 11: nupterm releases task 0F00 AQE 00FFF0 from resource queue 000C03'
check "11  ...and releases its allocation element " 'SVC 11: nupterm resource cleanup released 1 AQE(s) for task 0F00; tb+53..55=000000'
check "11  ...leaving the resource queue empty     " '000c01  00 00 00'
check "11  ...through ordinary AQE dequeue         " 'AQE 00FFF0 dequeued from resource queue 000C03 and owner queue 000F35, 16 bytes freed'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
