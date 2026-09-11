# Checkpoint 5 — the control storage processor's supervisor call families

Tag: `cp-5`

## What was ported

The remaining supervisor call families, by subject, each in its own source
file with a declaration fragment the class body includes:

- **Transfer control, the loader and task termination**
  (`As36Transfer.cpp`): SVC 10 (transfer by address), 14 (array transfer),
  04 (transfer by identifier, with identifier 4 as task termination), the
  transfer body (entry validation, the privilege gate, the resident and
  swapped forms, the program-block hash chain, program block build and
  make-ready with the system-transient owner cache), the request block
  stack (build, chain, free, the callee's register inheritance, the
  translation file per request block, the caller's map table copied into
  the callee, the header MAP list), SVC 11 (main storage exit), 05, 0C and
  0D (fast transfer and fast exit), 22 (dump task with the abnormal
  termination transfer), task-root termination with the dependency scan,
  the queue cleanup, the retained-context arm and the native continuation
  stack, and SVC 52 (the relocating loader with its relocation dictionary).
- **The dispatcher, waits, posts and events** (`As36Dispatch.cpp`): the
  ready queue, the wait return, task readying, next-task selection with the
  console job ordering, the dispatch switch with the preemptive register
  spill, supervisor call re-issue, the level-5 storage protection path, SVC
  00, 01, 02, 03, 08, 0B (the action controller with its queue, drain and
  coverage report), 17, 19, 1A, 1B, 1D, 1E, 20, 21 (allocation queue
  elements, share levels, holder priority), 23, 24, 25, 2B, 2E and 30.
- **Task creation, storage and transients** (`As36TaskCreate.cpp`): the
  task builder (task block, return element and request block as one
  allocation, task identifiers, the measurement block), SVC 31 and 32 with
  the control-block factory and swap areas, 12 and 13 (user area pages), 33,
  34 and 35 (the task work area allocator and work space maintenance), 26
  (prepare print buffer), 36 (SMFC), and the transient bodies reached
  through SVC 50: 03 (M36 transfer), 05 (IPL control), 09 (storage
  cleanup), 0A (native timers), 37 (time of day) and 3E (load control).
- **Monitor**: `whereis`, `tasklist`, `mapstate`, `sqsstate`, `modules`,
  `modstorage`, `residency`, `smf`, `allocchain`, `breakm` (with pending
  breakpoints that arm when the loader attributes the member), `xferid`,
  `xferterm`, `nuptermscan`, `actions`, `timers`, `show ptt`; the driver
  loop services native timers at the idle event wait; member breakpoints
  and the `flow`/`isn` member attribution resolve through the loader.
- Gate scripts brought across from the reference with their vector
  builders: `program-block-svcs`, `work-area-svcs`, `task-creation`,
  `task-dispatcher`, `action-controller`, `resource-svcs`,
  `relocating-loader`, `print-buffer`, `system-queue-space`,
  `system-queue-ownership`, `nupterm-probes`, `timer-transient`,
  `timer-op20`, `timer-op38`, `disk-write-wrap`, `command-files`,
  `ipl-command`; and `test/csp-families.sh`, the transcript differential.

## Gate

All runs use the user-supplied volume (`SIM36_VOLUME`) and, for the
differentials, the reference (`SIM36_REFERENCE`).

### Vector suites

| suite | checks |
|---|---|
| program-block-svcs | 23 passed |
| work-area-svcs | 37 passed |
| task-creation | 34 passed |
| task-dispatcher | 29 passed |
| action-controller | 33 passed |
| resource-svcs | 40 passed |
| relocating-loader | 11 passed |
| print-buffer | 13 passed |
| system-queue-space | 20 passed |
| system-queue-ownership | 9 passed |
| nupterm-probes | 18 passed |
| timer-transient, timer-op20, timer-op38 | 8, 7, 6 passed |
| disk-write-wrap | 6 passed |
| command-files | 30 passed |
| ipl-command | 7 passed |

`ctest` runs all of them (28 tests, all passing) together with the earlier
gates.

### Conformance

The ten CSP vectors pass and the transcript is now identical to the
reference INCLUDING the closing line, which milestone 4 had to exclude:

```
     5418 instruction(s), then: SVC 40 at 1C0E refused by the advanced36 control storage processor:
  SVC 40: the device set refused the request (DeviceSvc returned false; IOB 701E26, command 00, ace FE30)
10 passed, 0 failed
```

### Transcript differentials (`test/csp-families.sh`)

Identical line for line:

- the whole main storage IPL from `ipl pause` to instruction 33434 under
  `trace csp,svc,ace,disk,sched`, followed by `show cpu`, `tasklist`,
  `tasklist current`, `whereis`, `modules`, `mapstate`, `sqsstate`, `show
  ptt`, `actions` and `timers`.  Phase 1, the transfer to phase 2, the
  asynchronous tasks it creates, every wait, post and dispatch, the
  transient calls, the task work area allocations and the system queue
  space accounting all match;
- the command files of eight vector suites (the SVC 2E clock line
  excluded).

Instruction 33435 is SVC 43 command 82 (read current work station
configuration): the first call that needs the work station controller,
which is milestone 6.  The reference services it and runs on.

### Unit tests

```
[doctest] assertions: 1307 | 1307 passed | 0 failed |
```

Both compilers (g++ 10 and g++ 13) build warning-free.

## Divergences from the reference

| Where | Reference | SIM/36 | Why |
|---|---|---|---|
| SVC 43, 42 (work station), 41 (diskette), 46 (tape) | serviced | refused by milestone name | milestones 6 and 7 |
| transient 3E (load control) | resolves the configured unit block and the station's user name | answers as no display and traces `not ported yet (milestone 6)` | the work station controller |
| transient 05 (IPL control) | rewrites the machine's load source | records the requested source; `ipl` does not consult it yet | a pseudo-IPL from the guest is milestone 7 |
| `ipl` (no `pause`), `start`, `stop`, `wait` | driver thread | `ipl` drives synchronously until the stop | milestone 6 |
| `reset` against a running machine | refused in batch mode | not reachable without the driver | milestone 6 (`test/command-files.sh` carries the note) |
| `snapshot` round trip in `system-queue-space` | checked | the two snapshot checks are deferred | milestone 7 |
| reference experiments (`signonexp`, `SignonWddqWrite`, `CrustyInteractiveSession`, the cnfws power-on aid, auto-map faults) | present, off by default | not ported; a comment names each | they fabricate guest state and are not on the default path |
| SVC 2E | host clock | host clock | the transcript gate excludes the line |

## Reference drift recorded

- `test/program-block-svcs.sh` expects `swap area of 258 sector(s) ... cleared
  and deallocated`; the reference prints `not cleared and deallocated`
  (the synthetic swap area's base identifier 00 has no queue-header-46 block
  to resolve through) and fails its own check.  SIM/36 prints the same line;
  the check in this repository asserts the behaviour.
- `test/task-creation.sh`'s discovery regex predates the `(requested WR5
  nnnn)` clause of the task-creation trace and fails on the reference; the
  copy here includes the clause.
- `test/atr-ownership.sh` fails two of its own checks on the reference (a
  stale module ATR count and a reuse ratio); the run itself diverges only at
  the work station frontier, so it becomes a milestone 6 gate.

## Reference bugs suspected and left in place

- `conformance` stops on the reference at instruction 5418 with `SVC 40 ...
  IOB 701E26, command 00`: phase 1's disk IOB at translated 801E26 resolves
  into the module arena and carries command 00.  Reproduced exactly.
- The task work area allocator's tie-break on equal-sized runs is
  emulator policy (first wins); ported as is.
- `nuptask`'s composite allocation is retained after termination (the
  reference documents the leak); ported as is.

## Trace text and the self-containment rule

As at checkpoint 4, every trace and refusal string is carried verbatim
(the gate scripts assert them); comments describe behaviour and cite
SA21-9436 where the reference does.  Methods that the reference named
after the internal routines they transcribe are named after what they do
(`transientTimer`, `queueSetAction`, `loadControlChain`, ...).

## Open items carried forward

- The work station controller and its device path (SVC 42/43), the console
  and station backends, the driver thread, `start`/`stop`/`wait`,
  `stations`, `listener-auto-signon`, `tu` decodes over live blocks, and
  the probes that need them (`atr-ownership`, `modules-monitor`,
  `residency-monitor`, `breakm-monitor`, `smf-status`,
  `msipl-entry-lifecycle`, `nupterm-dependent-immediate`): milestone 6.
- Diskette and tape (SVC 41/46, phase 1 from `#IPLBOOT`), snapshots and
  panic dumps, the guest pseudo-IPL: milestone 7.
- The reference's `s36xref` Python tooling and the `s36xref-indexed` suite
  are not part of the emulator; recorded for the milestone 8 review.
