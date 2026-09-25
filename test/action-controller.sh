#!/bin/sh
# SVC 0B, Post Action Controller Status Word, as a queue-and-drain model.
#
# nusvc (c18e3848) forks on the inline mask: 0x79 is a guest bit, everything
# else goes through NuEmul::nuset (c18d1750), which test-and-sets a status bit
# and queues a NuSetAction that the host action scheduler runs later through
# NuSetAction::executeRequest (c18ab2f0).  The SVC returns for EVERY mask; SLIC
# never fails it.  What is asserted here:
#
#   - no mask stops the machine at the SVC any more (the old model refused
#     0x01/0x17/0x19/0x1B/0x6B/default with "SVC not serviced");
#   - the modelled arms (0x79 guest bit, 0x11/0x65 dispatcher, 0x2D wsdvcsr
#     survey) trace exactly what they traced before;
#   - 0x29 is dispatched as 0x2D and labelled an EMULATOR DECISION;
#   - the unmodelled arms are recorded as UNDISCHARGED with the issuing IAR;
#   - the machine stops only at nudspchA's no-task exit, and THAT stop lists
#     the undischarged posts, mask / routine / count / last IAR;
#   - the `actions` monitor command prints the same coverage.
#
# docs/s36/svc-task-event-queue.md §0B, docs/s36/svc-audit/queueing.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

end=$(python3 test/build-action-vectors.py "$TMP/code.bin")


cat > "$TMP/run.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/code.bin 1000
trace csp
set iar 1000
actions
step 12
actions
step 1
show cpu
actions
quit
EOF

out=$("$SIM36" -s "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"

checknot() {
  if echo "$out" | grep -qF "$2"; then
    echo "  $1 FAIL  (must not appear: $2)"; fail=$((fail + 1))
  else
    echo "  $1 PASS"; pass=$((pass + 1))
  fi
}

# --- before anything is posted ----------------------------------------------
check "--  empty report before the first post   " 'no SVC 0B post has been issued'

# --- the SVC never refuses ---------------------------------------------------
checknot "0B  no mask is refused at the SVC        " 'SVC not serviced'
checknot "0B  ...and none returns false            " 'handler returned false'

# --- the modelled arms, traced as before ------------------------------------
check "79  guest 08B4 bit 5                      " 'SVC 0B: mask 79 -> guest 08B4 |= 04'
check "11  posts nudspchA                        " 'SVC 0B: mask 11 posts nudspchA (task dispatcher); the post-SVC DispatchIfRequested honours it'
check "65  posts nudspchA                        " 'SVC 0B: mask 65 posts nudspchA (task dispatcher); the post-SVC DispatchIfRequested honours it'
check "2D  wsdvcsr survey                        " 'SVC 0B: mask 2D schedules NuActiveCtl::wsdvcsr via nuset/NuSetAction (c18ab474)'
check "2D  ...survey runs                        " 'SVC 0B mask 2D: wsentry scan survey'

# --- nuset: the queue, then the drain ----------------------------------------
check "11  nuset queues a NuSetAction            " 'SVC 0B: mask 11 -> nuset sets NuEmul+1436 bit and queues NuSetAction(11) via newWork'
check "65  ...in the high word for mask >= 40    " 'SVC 0B: mask 65 -> nuset sets NuEmul+1440 bit and queues NuSetAction(65) via newWork'

# --- R3's 29 is 2D, and says it is a decision ------------------------------
check "29  dispatched as 2D                      " "SVC 0B: mask 29 is SSP R3's equate of 7.5's 2D"
check "29  ...labelled EMULATOR DECISION         " 'EMULATOR DECISION beyond SLIC: dispatched as 2D'

# --- the unmodelled arms are recorded, not run ------------------------------
check "01  nutislih undischarged                 " 'SVC 0B: mask 01 names NutiTimer::nutislih (timer SLIH), a host action-scheduler routine this emulator does not model; the post is recorded as UNDISCHARGED (issued at IAR 1014'
check "17  wsentry undischarged                  " 'SVC 0B: mask 17 names NuActiveCtl::wsentry (work-station entry)'
check "19  wsentry undischarged                  " 'SVC 0B: mask 19 names NuActiveCtl::wsentry (work-station entry)'
check "1B  nuerio(4F) undischarged               " 'SVC 0B: mask 1B names NuEmul::nuerio(0x4F) (error I/O)'
check "6B  nucready undischarged                 " 'SVC 0B: mask 6B names NuEmul::nucready (make control ready)'
check "33  default arm nuerio undischarged       " 'SVC 0B: mask 33 names NuEmul::nuerio (error I/O, executeRequest default arm)'
# 0x01 is posted twice; the second post is a new NuSetAction because the drain
# ran (and cleared the bit) inside the first SVC.  Counted, not coalesced.
check "01  second post counted                   " 'task 0F00, SVC 0B; 2 seen, 2 undischarged'

# --- the report after twelve posts -------------------------------------------
check "--  status words are clear after drain    " 'NuEmul+1436 (mask < 40) = 0000000000000000, NuEmul+1440 (mask >= 40) = 0000000000000000; 0 NuSetAction(s) queued'
check "--  79 is a guest bit, discharged         " '79      1         0          1            0     1000      0F00'
check "--  2D discharged                         " '2D      1         0          1            0     100C      0F00  NuActiveCtl::wsdvcsr'
check "--  29 reported as the R3 equate          " '29      1         0          1            0     1010      0F00  2D (R3 equate) NuActiveCtl::wsdvcsr'
check "--  01 seen twice, undischarged twice     " '01      2         0          0            2     102C      0F00  NutiTimer::nutislih'
check "--  6B undischarged, high word            " '6B      1         0          0            1     1024      0F00  NuEmul::nucready'

# --- the stop: only at the no-task exit, and it names the posts --------------
check "1E  the wait reaches the no-task exit     " "SVC 1E: task block 0F00 waits on TB_STAT2 02 and no task is ready - nudspchA's no-task exit (c180e04c); undischarged SVC 0B action posts:"
check "1E  ...listing 01 with count and IAR      " ' 01 NutiTimer::nutislih (timer SLIH) x2 (last IAR 102C'
check "1E  ...and 17                             " ' 17 NuActiveCtl::wsentry (work-station entry) x1 (last IAR 1018'
check "1E  ...and 6B                             " ' 6B NuEmul::nucready (make control ready) x1 (last IAR 1024'
check "1E  ...and the default arm                " ' 33 NuEmul::nuerio (error I/O, executeRequest default arm) x1 (last IAR 1028'
check "1E  the trace carries the coverage too    " 'SVC 1E: no task is ready; SVC 0B action-controller coverage at the no-task exit:'
# The MSP is parked past the SVC 1E, where the IPL task would resume if posted.
check "1E  the machine stopped past the SVC 1E   " "IAR $end"
check "--  79 is not an executeRequest arm       " '79      1         0          1            0     1000      0F00  guest 0x08B4 bit 5 (inline in nusvc c18e3848, no NuSetAction)'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
