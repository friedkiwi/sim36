# Checkpoint 3 — MSP interpreter

Tag: `cp-3`

## What was ported

- **Decoder** (`src/Processors/InstructionSet.*`): the mode-nibble length
  rule, both SVC op codes (F4 and the 5.1 media's FC), F1/F2 as backward and
  forward JC, the three operation families, and the disassembly text
  (`LA     q=10 $1769`, `12(XR1)`, `0F(ARR)`, `.byte` for unassigned
  encodings).
- **SVC table** (`ControlStorage/SvcTable.*`): rejected set, dispatch
  classes, and the inline parameter counts the decoder needs (0F and 14 are
  3, 1E/08/05 corrected as the reference records).
- **Execution** (`MainStorageProcessor.*`): all 28 instructions with the
  reference's semantics: right-justified multi-byte operands processed right
  to left; the PSR set/reset rules per group; binary overflow reset at the
  start of ALC; SLC and S status from the carry out of the complement add;
  SLI's before-value compare; sticky test-false cleared only by a BC/JC that
  tests it; zoned decimal with F/D sign zones and ARR disturbance; SRC's
  two-nibble Q; ED and ITC; the register selector table including the
  three-byte A0-A3 forms and the IAR-selecting branch; LA's literal
  prefix catenation; BC/JC/LPMR/XFER/SVC/LA operands kept logical; LPMR's
  privilege and Q rules; per-byte operand translation across ATR
  boundaries; the fetch under PIAR.  The core is exception-free: a bounds
  or protection fault abandons the instruction at the faulting access and is
  dispositioned once at the end of the step (a program check and stop, or
  the level 5 interrupt to the control processor).
- **Breakpoints, member breakpoints, patch-on-reach, the member filters,
  the flow and isn traces**, and the refusal path (`SVC nn at aaaa refused
  by the ... control storage processor:` with the CSP's reason).
- **Monitor**: `selftest` with all 51 vectors (also callable without a
  volume through `runSelfTest`), `dis`, `step` (bounded loop; the host
  event pump at preemption points is milestone 4), `break`, `watch`,
  `poke`, `patch`, `findmem`, `addrmap`, `trace member`, `break member`,
  and `loadfile` setting the IAR as the reference does.
- Two gate scripts: `test/decode-differential.sh` (five IPL members loaded
  from the user-supplied volume through both emulators' `load` and `dis`)
  and `test/msp-parity.sh` (the debugger surface and a phase 1 trace).

## Gate

### selftest, both compilers

```
$ ./build/gcc10/sim36_tests | tail -2       # includes "msp: every manual vector passes"
[doctest] assertions: 1278 | 1278 passed | 0 failed |
[doctest] Status: SUCCESS!
$ ./build/linux-make/sim36_tests | tail -2  # g++ 13
[doctest] assertions: 1278 | 1278 passed | 0 failed |
```

On a constructed machine `selftest` prints the reference's 51 lines and
`51 passed, 0 failed`; the transcript is part of `test/msp-parity.sh` and
is identical on both emulators.  MSVC runs the same unit test in CI.

### Decode differential

```
$ SIM36_VOLUME=<image> SIM36_REFERENCE="mono .../reference.exe" test/decode-differential.sh
  #MSIPL     951 instructions  0 differ
  #MSTWA    2955 instructions  0 differ
  #MSCPR     442 instructions  0 differ
  #SVTUB     738 instructions  0 differ
  #MSNIP    4698 instructions  0 differ
5 members, 9784 instructions, 0 differences
```

These are the instruction counts the reference's own README quotes.  The
members are read from the volume by name through `load #LIBRARY <name>`;
an instruction counts only when it lies wholly inside the member.

### MSP parity and the phase 1 trace

```
$ SIM36_VOLUME=<image> SIM36_REFERENCE=... test/msp-parity.sh
diffrun: comparing from 'sim36> selftest' onward
identical  /tmp/sim36-msp.RS9iGf/msp.sim
diffrun: comparing from 'sim36> ipl pause' onward
diffrun: ignoring lines matching /^csp   main storage IPL is not ported yet/
diffrun: ignoring lines matching /^src   SRC posted: 0000  running normally/
identical  /tmp/sim36-msp.RS9iGf/err1.sim  ... err9.sim
```

The trace window is **not the 5,000 instructions the milestone asks
for**.  Phase 1's ninth instruction is `SVC 0F` (the direct-area read), and
every later instruction depends on what the control storage processor
returns.  SIM/36 refuses that call by name until milestones 4 and 5 port
the supervisor, so the identical window today is instructions 1-9 of phase
1 (verified by hand over `step 5000`: the reference services the call and
continues; SIM/36 stops with `SVC 0F at 10FB refused by the advanced36
control storage processor`).  The 5,000-instruction comparison is carried
forward as the first gate of milestone 5.

### ctest

```
$ (cd build/gcc10 && ctest)
100% tests passed, 0 tests failed out of 8
```

## Reference drift recorded

- The reference's fast-tier runner expects `47 passed, 0 failed` from
  `selftest` while the command prints `51 passed, 0 failed`; SIM/36 gates
  on 51.
- The reference's decode-differential script extracts members by arithmetic
  from a volume-specific member base and compares against a Python
  disassembler; SIM/36's version loads the members by name through both
  emulators and compares their `dis` output, which is what the port brief
  asks for and needs no second tool.

## Divergences from the reference

| Behaviour | Reference | SIM/36 | Cause |
|---|---|---|---|
| Any SVC | serviced | refused with `SVC nn: the control storage processor is not ported yet (milestone 4)` | milestone 4/5 |
| `step` at a preemption point | pumps live monitor work and host events | bounded instruction loop only | milestones 4 and 6 |
| `break list`, `watch` reports | name the member at the IAR once the loader attributes one | no member suffix (identical before any member is attributed) | milestone 5 |
| `breakm` | member-relative breakpoints through the loader | refused by name | milestone 5 |
| A path whose parent directory is missing | `Could not find a part of the path "..."` (an IOException the entry point does not catch) | same wording from a command file; `not found: <path>` at top level | the reference's top-level crash is not reproduced |

## Reference bugs suspected and left in place

- `Effective()` resolves both operands for read before execution, so a
  write-only fault on a protected page is reported as `(read)`.  Kept; the
  unit test asserts the reference wording.
- `poke` accepts a hex address above 16 bits and writes there; `break`
  refuses one.  Kept.

## Open items carried forward

- Milestone 4: control-storage IPL, storage SVCs, the SVC 40 path, the
  step loop's host-event pump; then the phase 1 trace window extends past
  instruction 9.
- Milestone 5: `breakm`, member attribution in `watch`, `break list`,
  `whereis`.
