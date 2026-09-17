#!/bin/sh
# Print the master configuration record from CNFIGSSP on W1 with the shipped
# PB-printer topology and require the whole report to reach the printer:
# the report text, its closing form feed and the spool writer's own end of
# entry.  This covers the print open through SPALC/SPQMG (a translated
# assign that grows the job region), the resident print-put routine, the
# deferred printer completion the spool writer waits for, and the writer's
# general-post event handling at end of job.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

S36_PORT_BASE=23800 \
S36_LIST_CONFIG='default-printer-machine.sim.in' \
S36_LIST_STATIONS='0' \
S36_LIST_COMMAND='CNFIGSSP' \
S36_LIST_STATION='W1' \
S36_LIST_EXPECT_SCREEN='CONFIGURATION' \
S36_LIST_CNFIG_PRINT=1 \
S36_LIST_EXPECT_PRINTER_DRAIN='0.1' \
S36_LIST_EXPECT_PRINTER_TEXT='S/36 CONFIGURATION' \
S36_LIST_SETTLE=1 \
S36_LIST_TIMEOUT=240 \
S36_LIST_DRAIN_TIMEOUT=120 \
python3 test/list-all-regression.py
