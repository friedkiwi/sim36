#!/bin/sh
# Run the reported SSP 7.5 HISTORY LIST selection with the shipped PB-printer
# topology.  Besides the older DD1OP/DD2OP transfer coverage, this reaches the
# SLIC General Post queue-30 completion used while the report is produced.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

S36_PORT_BASE=23500 \
S36_LIST_TRACE_SPEC='ws' \
S36_LIST_CONFIG='default-printer-machine.sim.in' \
S36_LIST_STATIONS='0,2' \
S36_LIST_COMMAND='HISTORY LIST,ALL,ALLWS,ALLENTS,ALLDAYS,000000,235959,SYSTEM,NOERASE' \
S36_LIST_STATION='W1' \
S36_LIST_EXPECT_SCREEN='' \
S36_LIST_EXPECT_MONITOR='printer 0.1:' \
S36_LIST_REJECT='SYS-0392|SYS-1887' \
S36_LIST_SETTLE=30 \
S36_LIST_TIMEOUT=360 \
python3 test/list-all-regression.py
