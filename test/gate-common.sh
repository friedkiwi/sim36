# Shared preamble for the gates that need the user-supplied volume.
#
# SIM36         the sim36 executable (default: build/linux-make/sim36)
# SIM36_VOLUME  a System/36 volume image; unset -> the gate is skipped
#
# Nothing from the volume is stored in the repository: the expected values
# below are byte offsets and trace texts, computed against the volume the
# user supplies at run time.
SIM36=${SIM36:-build/linux-make/sim36}
if [ -z "${SIM36_VOLUME:-}" ]; then
  echo "SKIP: no volume (set SIM36_VOLUME to a System/36 volume image)"
  exit 77
fi
if [ ! -x "$SIM36" ]; then
  echo "SKIP: no sim36 executable at $SIM36 (set SIM36)"
  exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
check() {   # check <name> <fixed-string pattern>
  if echo "$out" | grep -qF -- "$2"; then
    echo "  $1 PASS"; pass=$((pass + 1))
  else
    echo "  $1 FAIL  (looked for: $2)"; fail=$((fail + 1))
  fi
}
