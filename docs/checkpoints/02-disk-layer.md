# Checkpoint 2 — disk layer

Tag: `cp-2`

## What was ported

- **Storage**: the library directory walk (51-byte entries, five to a
  sector, member sectors relative to the extent).
- **Machine**: the MSP register file with the PSR load rules, the PMR
  projected from the PACT bytes and the CMR/PCSP shared bit; the machine
  state with main storage, the 128 ATRs, exception-free bounds-checked
  access, watchpoints, the observer hook, untranslated and translated
  address resolution, page-by-page translated extents, the SRC posting and
  check capture; the deterministic scheduler; the `Machine` object that
  constructs state, volume backend, device set and control storage
  processor from a validated definition, reads both VTOCs and resolves
  names with or without a leading `#`.
- **Devices**: the fixed disk with the IOB decode (command at +0A,
  modifier at +0B, count-1 at +16, 1-based sector at +19, work sector at
  +23, buffer at +13 with the translated flag), the `A0`/`A4` immediate
  completions, `A1` reads into real and task-translated buffers, `A2`
  writes with the fill and wrap modifiers, and `A3` scans with the three
  relations; the ECM post; the ACE layout and build; the request block
  accessors; a device set holding the disk.
- **Control storage processor**: the interface (dispatch classes, the SVC
  request, the transient-area contract) and the Advanced/36 shell with the
  stage A bring-up line.  Stage B, the main storage IPL, is refused by name.
- **Monitor**: `vtoc`, `lib`, `sector`, `dump`, `boot`, `load`,
  `loadfile`, `set <register>`, `show cpu|storage|atr|csp`, post-IPL
  `trace`, `ipl [pause]` (construction, the reference's two lines, then
  the refusal of continuous execution), the definition latch and `reset`
  release, and the reference's error wording for bad addresses, sectors
  and hex.
- `tools/diffrun.sh` gained `--from <command>` and `--ignore <regex>`, both
  announced in its output, and refuses to call an empty transcript
  identical.
- The Windows toolset pin from checkpoint 1 is confirmed: the v142 static
  build links, and `dumpbin /dependents` lists only `WS2_32.dll` and
  `KERNEL32.dll`.

## Gate

`test/disk-layer.sh` (a `ctest` entry that skips without `SIM36_VOLUME`
and `SIM36_REFERENCE`) runs one clean command file and nine error files
through both emulators on the user-supplied volume.  The clean file covers
`show storage`, `vtoc system`, `vtoc user`, `vtoc`, `lib #RPGLIB` three
ways, `boot`, `sector` over ten sector addresses, `show csp`, `show atr`,
register sets and `show cpu`, `load #RPGLIB #AU002`, `dump` over ten
addresses (including `xr1` and the top of storage), the `load` refusals,
`reset --yes` and `show status`.  The error files cover an out-of-volume
sector, a dump past storage, unparsable hex, a latched-definition change,
a re-attach after IPL, a plain `reset` from a file, a bad `vtoc` argument
and a bare `load`.

```
$ SIM36_VOLUME=<image> SIM36_REFERENCE="mono .../reference.exe" test/disk-layer.sh
diffrun: comparing from 'sim36> show storage' onward
diffrun: ignoring lines matching /^csp   main storage IPL is not ported yet/
identical  /tmp/sim36-disk-layer.ukKs2s/disk.sim
diffrun: comparing from 'sim36> ipl pause' onward
diffrun: ignoring lines matching /^csp   main storage IPL is not ported yet/
diffrun: ignoring lines matching /^src   SRC posted: 0000  running normally/
identical  /tmp/sim36-disk-layer.ukKs2s/err1.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err2.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err3.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err4.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err5.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err6.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err7.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err8.sim
identical  /tmp/sim36-disk-layer.ukKs2s/err9.sim
```

The two exclusions are the milestone 4 boundary and nothing else: the
reference prints `src   SRC posted: 0000  running normally` at the end of
its control-storage IPL, where SIM/36 prints one line saying that stage is
not ported.  A hand diff of the complete transcripts (no `--from`, no
`--ignore`) showed exactly those lines as the only difference.  The clean
transcript is 556 lines and echoes all 48 commands.

Disk unit tests run over synthetic volumes the tests build themselves:

```
$ ./build/gcc10/sim36_tests | tail -3
[doctest] test cases:   36 |   36 passed | 0 failed | 0 skipped
[doctest] assertions: 1238 | 1238 passed | 0 failed |
[doctest] Status: SUCCESS!
$ (cd build/gcc10 && ctest)
100% tests passed, 0 tests failed out of 6
```

Both g++ 10 and g++ 13 build warning-free.

### Checkpoint 1 CI result (carried forward)

```
windows-msvc142-static  success
linux-gcc10             success
  Image has the following dependencies:
    WS2_32.dll
    KERNEL32.dll
```

## Divergences from the reference

| Behaviour | Reference | SIM/36 | Cause |
|---|---|---|---|
| `ipl pause` | performs the control-storage IPL and posts SRC 0000 | prints `csp   main storage IPL is not ported yet (milestone 4): ...` and leaves storage untouched | milestone 4 |
| `boot` | resets (including the control-storage IPL) then reads the boot record | the same, with the refusal line above in place of the IPL | milestone 4 |
| `show cpu` after `ipl pause` | IAR 1000 from the posted task | IAR 0000 | milestone 4; the gate sets the registers explicitly first |
| `ipl` (no pause) | starts execution on the guest thread | prints the two IPL lines, then `start: continuous execution is not ported yet (milestone 4)` | milestone 4 |
| `diskread` | issues SVC 40 through the CSP | refused by name | milestone 4 |
| `stations`, `listener-auto-signon` after IPL | machine views | refused by name | milestones 4 and 6 |
| Every other machine-lifecycle command (`step`, `dis`, `selftest`, `watch`, `break`, ...) | works | refused with the milestone that ports it | not yet ported |

## Reference bugs suspected and left in place

- `lib <name> <count>` parses the count with `int.Parse` but never checks
  it is positive; a negative count lists nothing.  Kept.
- `boot` and `ipl pause` reset the machine, but `boot` does not print the
  `IPL started` line, so a `boot` after `ipl pause` silently re-runs the
  control-storage IPL.  Kept; both emulators behave the same.

## Open items carried forward

- Milestone 3: the MSP decoder and the 28 instructions, `selftest`, `dis`,
  `step`, the decode differential.
- Milestone 4: the control-storage IPL (low storage, UDT walk, phase 1
  load, the initial task), the storage SVCs, the SVC 40 path for
  `diskread`, and the two gate exclusions above become unnecessary.
