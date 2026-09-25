#!/bin/sh
# COMPRESS and CONDENSE both cross the configured-TU/System-Request-TU alias
# boundary after STOP SYSTEM.  Enter each real SSP 7.5 procedure and require
# its first procedure panel: the historical failure left the MAIN command
# input parked forever and never launched either real procedure.
set -eu
cd "$(dirname "$0")/.."
. test/gate-common.sh

run_maintenance_command() {
    command_name=$1
    port_base=$2
    S36_PORT_BASE=$port_base \
    S36_LIST_STATIONS=0 \
    S36_LIST_STATION=W1 \
    S36_LIST_STOP_SYSTEM=1 \
    S36_LIST_COMMAND=$command_name \
    S36_LIST_EXPECT_SCREEN="$command_name procedure is running" \
    S36_LIST_SETTLE=0.2 \
    S36_LIST_REJECT='CHECK [|storage protection' \
    python3 test/list-all-regression.py
}

run_maintenance_command COMPRESS 25100
run_maintenance_command CONDENSE 25120
