#!/bin/sh
# Non-media-gated deterministic STARTREK tape builder test.  The public
# sources remain outside the repository; CI or the caller provides FUNLIB_DIR.
set -eu

cd "$(dirname "$0")/.."
if [ -z "${FUNLIB_DIR:-}" ]; then
    echo "SKIP: set FUNLIB_DIR to pinned FUNLIB commit 1d0d2ea221cdb7b22d611d8cef474ebd518fb70f"
    exit 77
fi

tmp=$(mktemp -d)
trap 'find "$tmp" -depth -type f -delete; find "$tmp" -depth -type d -empty -delete' EXIT

python3 tools/build-startrek-tape.py "$tmp/first" --funlib-dir "$FUNLIB_DIR" >"$tmp/first.log"
python3 tools/build-startrek-tape.py "$tmp/second" --funlib-dir "$FUNLIB_DIR" >"$tmp/second.log"
diff -ru "$tmp/first" "$tmp/second"
tools/tape-folder.py verify "$tmp/first" --reject-orphans
tools/tape-folder.py extract "$tmp/first" "$tmp/extract" --file 3 >/dev/null

python3 - "$FUNLIB_DIR" "$tmp/extract/0003/joined.bin" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
source = root / "startrek" if (root / "startrek").is_dir() else root
data = Path(sys.argv[2]).read_bytes()
assert len(data) % 256 == 0
records = [data[offset:offset + 256] for offset in range(0, len(data), 256)]
assert len(records) == 2788
assert all(record[120:] == "S".encode("cp037") * 136 for record in records)
cards = [record[:120].decode("cp037").rstrip() for record in records]

def lines(name):
    return (source / name).read_bytes().decode("utf-8").splitlines()

expected = []
for member_type, member, recl, filename in (
        ("P", "STREK", 120, "STREK.OCL36"),
        ("S", "STREK", 96, "STREK.RPG36"),
        ("S", "STREKFM", 80, "STREKFM.DSPF36")):
    member_lines = lines(filename)
    if filename == "STREKFM.DSPF36" and member_lines[-1] == "// CEND":
        member_lines.pop()
    expected.append("// COPY LIBRARY-%s,NAME-%s,RECL-%03d" %
                    (member_type, member, recl))
    expected.extend(member_lines)
    expected.append("// CEND")
assert cards == expected
PY

first=$(sed -n 's/^media tree SHA-256: //p' "$tmp/first.log")
second=$(sed -n 's/^media tree SHA-256: //p' "$tmp/second.log")
[ -n "$first" ] && [ "$first" = "$second" ]
test "$(find "$tmp/first" -maxdepth 1 -name '*.dat' -type f | wc -l)" -eq 5
echo "PASS: deterministic STARTREK library tape $first"
