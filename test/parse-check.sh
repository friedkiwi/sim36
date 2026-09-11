#!/bin/sh
# Every command file under test/ must lex and name only registered commands.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim36=${SIM36:-$1}
exec "$sim36" --check "$here"/test/*.sim
