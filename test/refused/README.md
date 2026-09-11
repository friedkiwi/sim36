Command files here are expected to be REFUSED with a specific message: they
name commands the monitor deliberately removed.  They are part of the parity
set (`test/config-parity.sh`) but excluded from the syntax check
(`test/parse-check.sh`), which asserts that every file in `test/` proper
names only registered commands.
