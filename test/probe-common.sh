# Shared preamble for the probe gates: a command-file template (test/<name>.sim.in)
# is instantiated into $TMP with @HERE@, @VOLUME@ and @TMP@ substituted, then run.
. test/gate-common.sh
here=$ROOT
instantiate() {   # instantiate <name>  -> $TMP/<name>.sim
  sed "s#@HERE@#$here#g; s#@VOLUME@#$SIM36_VOLUME#g; s#@TMP@#$TMP#g" "test/$1.sim.in" > "$TMP/$1.sim"
}
run() {           # run <name> -> transcript on stdout
  instantiate "$1"
  "$SIM36" -c "$TMP/$1.sim" 2>&1
}
checkin() {       # checkin <name> <fixed-string pattern> <text>
  if printf '%s\n' "$3" | grep -qF -- "$2"; then
    echo "  PASS  $1"; pass=$((pass + 1))
  else
    echo "  FAIL  $1 (looked for: $2)"; fail=$((fail + 1))
  fi
}
