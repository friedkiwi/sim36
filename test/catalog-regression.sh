#!/bin/sh
# The CATALOG help form leaves optional fields as null-filled display cells.
# A Read-MDT client omits those unchanged fields; controller expansion must
# return blanks rather than exposing nulls to the utility statement parser.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

S36_PORT_BASE=23800 \
S36_LIST_CATALOG_MENU=1 \
S36_LIST_STATION='W1' \
S36_LIST_EXPECT_SCREEN='CATALOG PROCEDURE' \
S36_LIST_EXPECT_MONITOR='' \
S36_LIST_REJECT='SYS-4109' \
S36_LIST_SETTLE=0.2 \
S36_LIST_CATALOG_FORM=1 \
S36_LIST_CATALOG_PAGE_TARGET='#TUITN' \
python3 test/list-all-regression.py
