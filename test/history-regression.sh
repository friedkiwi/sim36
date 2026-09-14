#!/bin/sh
# The full HISTORY selection used to lose DD1OP's initialized program request
# area during its no-return transfer to DD2OP, which surfaced as SYS-0392.
# Once the list is displayed, replay the reported Cmd3, Enter, Cmd7 sequence;
# its native continuation must retain the old request frame used by MAP.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

S36_PORT_BASE=23500 \
S36_LIST_TRACE=1 \
S36_LIST_TRACE_MEMBER=DD2OP \
S36_LIST_COMMAND='HISTORY CRT,USER,ALLWS,ALLENTS,ALLDAYS,000000,235959,SYSTEM,NOERASE' \
S36_LIST_STATION='W1' \
S36_LIST_EXPECT_SCREEN='HISTORY SCROLL' \
S36_LIST_EXPECT_MONITOR='' \
S36_LIST_REJECT='SYS-0392' \
S36_LIST_SETTLE=0.2 \
S36_LIST_KEYS='Cmd3,Enter,Cmd7' \
python3 test/list-all-regression.py
