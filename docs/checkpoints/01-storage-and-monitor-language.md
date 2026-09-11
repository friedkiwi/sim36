# Checkpoint 1 — storage foundation and monitor language

Tag: `cp-1`

## What was ported

- **Storage**: EBCDIC code pages 500 (the volume) and 037 (the tape
  manifest) as generated tables with UTF-8 decode and substitute-on-encode
  (`src/Storage/Ebcdic.*`, generator `tools/gen-ebcdic.py`); big-endian
  byte-view helpers (`ByteOrder.h`); the volume image backend with
  read-write, read-only and overlay modes and the `.lock` sidecar for
  read-write attaches (`DiskBackend.*`); both VTOCs located through VOL1
  with the reference's scan-and-fallback rule (`Vtoc.*`).  JSON is provided
  by nlohmann-json; the reference's own small reader/writer is not ported as
  code (its output formatting will be matched when the tape manifest is
  ported in milestone 7).  Main storage is deferred to the machine state in
  milestone 4, where the reference keeps it.
- **Configuration**: the definition with every knob and default, the
  legacy INI reader with all of its diagnostics, validation, the model table
  and the IPL-source table, and SSP's device-code table (`src/Devices`).
- **Monitor**: the tracer with its flag parser and the reference's flags
  rendering; the human and replay configuration renderers; the complete
  pre-IPL command surface (`show config|status|terminal`, `save config`,
  `get terminal`, `set machine|station|terminal|disk0|diskette0|tape0`,
  `remove station`, `attach`/`detach`, `reset`, `trace`,
  `listener-auto-signon`, `stations`) with the reference's messages; `ipl`
  validates the definition exactly as the reference does and then refuses
  construction by name.
- **Host**: the BSD/Winsock shim and the listener half of the station
  backends and the station multiplexer (bind and listen only; sessions come
  with milestone 6), so that the operator-visible endpoint state is real.
- `sim36 --check FILE...`: a SIM/36 addition that lexes command files and
  looks each verb up in the registry, following `do`, without executing.
- `tools/diffrun.sh`: the differential harness.  It runs both emulators with
  `-c test/diffrun-base.sim` (multiplexer off, so no port is bound) and
  `-s <file>`, normalises the prompt, product banner, temporary paths and
  timestamps, and diffs.  The reference is named through `SIM36_REFERENCE`
  (a command prefix) so that nothing in the repository names it.

## Gate

### Every reference command file parses

```
$ sim36 --check <reference>/etc/*.sim <reference>/test/*.sim
<reference>/test/cpsc-first-worker-slot-consumer.sim:9: unknown command 'cpu'
<reference>/test/cptc-8100-wddq-trace.sim:17: unknown command 'cpu'
<reference>/test/power-command-removed.sim:1: unknown command 'power'
<reference>/test/tfrm36-configured-station-cpsi-trace.sim:19: unknown command 'regs'
<reference>/test/tn5250-w7-post-cpon-frontier.sim:9: unknown command 'tasks'
```

All 199 files lex; the five reports above are verbs the reference itself
rejects with `unknown command 'cpu'|'regs'|'tasks' - try help` (verified by
running it), and `power-command-removed.sim` exists to prove `power` is
refused.  The 14 probe command files in the reference's top directory use
the removed `run` verb and are rejected by both emulators alike.

### show config / save config stdout identical

```
$ SIM36_REFERENCE="mono .../reference.exe" tools/diffrun.sh test/config-*.sim test/refused/*.sim
identical  test/config-default.sim
identical  test/config-errors-11.sim
identical  test/config-errors-12.sim
identical  test/config-errors-13.sim
identical  test/config-errors-2.sim
identical  test/config-errors-3.sim
identical  test/config-errors-4.sim
identical  test/config-errors-5.sim
identical  test/config-errors-6.sim
identical  test/config-errors-8.sim
identical  test/config-errors-9.sim
identical  test/config-errors.sim
identical  test/config-legacy-listeners.sim
identical  test/config-multiplex.sim
identical  test/config-nested.sim
identical  test/refused/power-removed.sim
identical  test/refused/security-removed.sim
```

The three representative definitions are the built-in one
(`config-default.sim`), the per-station listener topology with a printer
(`config-legacy-listeners.sim`) and the multiplexer on a private endpoint
(`config-multiplex.sim`); each prints `show config` and `save config
stdout`.  `test/config-volume.sh` repeats them with a user-supplied volume
attached in overlay, read-only and read-write mode plus diskette and tape:

```
$ SIM36_VOLUME=<image> SIM36_REFERENCE=... test/config-volume.sh
identical  /tmp/sim36-config-volume.Cz40qn/volume.sim
```

A legacy INI startup file (`-c <reference>/etc/*.conf`) produced identical
output on both, including the `not found: <full path>` exit when the file's
volume is absent.

### EBCDIC round trip and the rest of ctest

```
$ ./build/gcc10/sim36_tests | tail -3
[doctest] test cases:   21 |   21 passed | 0 failed | 0 skipped
[doctest] assertions: 1121 | 1121 passed | 0 failed |
[doctest] Status: SUCCESS!
$ (cd build/gcc10 && ctest)
100% tests passed, 0 tests failed out of 5
```

Both g++ 10 and g++ 13 build warning-free.  `config_parity` and
`config_volume` skip (and pass) when `SIM36_REFERENCE` or `SIM36_VOLUME`
is unset.

### MSVC

The checkpoint-0 Windows job failed at link: vcpkg built replxx with the
v143 toolset while the project used v142, and a v143 static library
references STL helpers the v142 runtime lacks.  Fixed by an overlay triplet
`triplets/x64-windows-static-v142.cmake` that pins `VCPKG_PLATFORM_TOOLSET`
to v142 for the ports as well.  The CI result for this checkpoint is
recorded in the next report.

## Divergences from the reference

| Behaviour | Reference | SIM/36 | Cause |
|---|---|---|---|
| `ipl` with a valid definition | constructs the machine | `ipl: machine construction is not ported yet (milestone 4)` | not yet ported; the validation errors before that point are identical |
| `snapshot`, `panic` | work before IPL | refused with `not ported yet (milestone 7)` | not yet ported |
| Listeners | accept and negotiate clients | bind and listen only; a connecting client waits in the backlog | host sessions are milestone 6 |
| Unknown-option text | `unknown option --x` | cxxopts' wording for a bad flag; a stray positional argument uses the reference wording | dependency |
| `sim36 --check` | absent | present | SIM/36 addition for the parse gate; not a monitor command |

## Reference bugs suspected and left in place

- `set machine memory 5000K` is accepted silently and only refused at
  `ipl`, after the volume check, so a definition with no volume reports the
  volume before the storage ceiling.  Kept.
- Windows: the reference's listener comment says SO_REUSEADDR keeps a live
  port conflict visible, then documents that it does not under Mono.  SIM/36
  sets SO_REUSEADDR the same way; behaviour under a live conflict is
  platform-dependent on both.

## Open items carried forward

- Milestone 2: `#LIBRARY` location, boot record, library directory walks,
  `vtoc`, `lib`, `sector`, `dump`, and the fixed-disk device with IOB decode
  and the `A0`/`A4` reads.  These need a constructed machine in the
  reference (`Lifecycle.Machine`), so milestone 2 will also carry the
  minimum of `Machine` needed to host them.
- The tape manifest's JSON formatting must match the reference's writer
  byte for byte (milestone 7).
- Confirm the MSVC CI job is green with the pinned toolset.
