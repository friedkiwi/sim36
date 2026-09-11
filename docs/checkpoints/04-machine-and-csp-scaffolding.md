# Checkpoint 4 — machine, scheduler, devices, CSP scaffolding

Tag: `cp-4`

## What was ported

- **Guest low storage** (`ControlStorage/GuestLowStorage.*`): the control
  processor's IPL build of guest 0x0800..0x0FFF, the queue header table at
  0x0B00 (3-byte values at 0x0B03 + 4n), the ACE pool and disk IOB
  eyecatchers, the system block at 0x6000, the disk-end fields, and the
  unit definition table walk with every arm's trace text (twelve records,
  466 bytes on the reference volume).
- **System queue space** (`GuestHeap.*`): the buddy-class allocator over
  0x2000..0x10000 with 64 KB growth to 0x6F0000, its 112-byte header, the
  refusal reasons and diagnostics; the per-work-space free map
  (`WorkSpaceHeap`) with its 64-byte granularity.
- **Control blocks** (`TaskBlock.*`, `ProgramBlock.*` with `ControlBlock`,
  `StorageBlock`, `JobControlBlock` and `LoadMemberHeader`,
  `MapParameterList.*` with `MapTable`, `AllocationQueueElement.*`,
  `ActionControlElement*`, `NuPtt.*`, `DirectArea.*`, `TransientArea.*`,
  `TaskWorkArea.h`): offsets, eyecatchers, accessors and the translation
  file pool, all as big-endian byte views through `MachineState`.
- **The control storage processor** (`As36ControlStorageProcessor.*`,
  `As36Storage.cpp`): stage A (heap and ACE reset, direct area words 1074
  and 1124 from the panel), stage B (low storage, the UDT walk and the
  reload gate, the console unit block "TU" queued on headers 49 and 50,
  phase 1 from sector 8191 to 0x1000, the initial task block at 0xF00 with
  its request block at 0xE00, its translation file, the ready-queue insert
  and the MSP start), the supervisor call path (register spill into the
  request block, the call stamped at rb+16..22, the last-resort refusal
  record, the reload from whatever block is current on exit), and these
  families:
  - SVC 0F system control block access (queue header and direct area arms);
  - SVC 0E and the queue engine (FIFO, LIFO, both priority variants, the
    duplicate and not-found answers, the walk guards);
  - SVC 06 and 07 (plain and chained arms, the length validation);
  - SVC 2C and 2D (work space assign and free, the high-water growth that
    rebuilds the task's registers);
  - SVC 2F MAP with every action (1, 2, 3, 4, 5, 6, 7, 9), the register
    rewrite, the map table append with the reference's full-table
    replacement rule, the inherited-map walk with the request-block-own
    compensating entry, compaction, and the translation register builder
    with its three object kinds;
  - SVC 51 (direct, indirect, job work space and relative addresses; the
    read-only put refusal);
  - SVC 50 and 18 through the transient area, SVC 4C, SVC 09 and 0A;
  - the device path for SVC 40–48: ACE build, task association, event
    type, dispatch to the device set, completion post from the device's
    own code, release; the unmodelled-device declines for 44, 45 and 48.
  - the control-block use counts, domain count, deletion and dequeue; the
    task work area queue headers (queue header 46), relative address
    resolution and the swap-area clear.
- **Device set** (`Devices/DeviceSet.*`, `WorkStationIob.h`, `UnitBlock.*`):
  the IOB resolution rule, the disk dispatch, the declines and their
  counters, and the terminal unit block decoder.
- **Monitor**: `diskread` (SVC 40 through the control processor), `ace`,
  `ace queue`, `iob`, `tu`, `sched`, `conformance`, and `ipl` without
  `pause`, which drives the machine in the foreground until it stops.
- Gate scripts: `test/storage-svcs.sh` (27 checks) with its vector builder
  `test/build-storage-vectors.py`, `test/udt-walk.sh` (27 checks), and
  `test/csp-scaffolding.sh` (five transcript differentials).

## Gate

All runs use the user-supplied volume (`SIM36_VOLUME`) and, for the
differentials, the reference (`SIM36_REFERENCE`).  Nothing from the volume
is in the repository.

### Storage-SVC vectors and the UDT walk

```
$ SIM36=build/gcc10/sim36 SIM36_VOLUME=<image> test/storage-svcs.sh | tail -1
27 passed, 0 failed
$ SIM36=build/gcc10/sim36 SIM36_VOLUME=<image> test/udt-walk.sh | tail -1
27 passed, 0 failed
```

The reference's own scripts assert 27 checks each (the brief's "22" counts
the storage vectors before the action-4/5 assertions were added to the
reference); both sets pass in full.  The storage-SVC command file is also
run as a full transcript differential and is identical line for line,
including every `csp` trace line of the seven MAP, assign and free calls
and the five SVC 51 calls.

### Conformance

```
$ ... ipl pause / conformance
  low storage: ACE eyecatchers               PASS
  low storage: disk IOB eyecatchers          PASS
  low storage: system block at 6000          PASS
  setfd: fixed-disk end is exclusive 1-based image end PASS
  setfd: post-IPL disk extent forms ten-sector blocks PASS
  task block at F00 carries "TB" (E3C2)      PASS
  ace+19 points at the task block            PASS
  4 KB of phase 1 at 1000, from sector 8191  PASS
  MSP running, IAR inside phase 1            PASS
  MSP executes phase 1 without external help PASS
     24 instruction(s), then: SVC 10 at 11BC refused by the advanced36 control storage processor:
  SVC 10 (Overlapped) is not ported yet (milestone 5: transfer control and the loader)
10 passed, 0 failed
```

The ten vectors pass on both emulators and the transcript is identical
except for the closing instruction count and stop reason (see below).

### `ipl pause`, `diskread`, phase 1

`test/csp-scaffolding.sh` compares, and finds identical:

- `ipl pause` under `trace csp,ace,disk` with `show cpu`, `show atr`, dumps
  of 0x0800..0x09FF, the queue header table, the request and task blocks,
  the system queue space and the system block, `ace 03C0`, `ace queue 39`,
  `ace queue 40`, `sched` and `tu 0000`: every low-storage byte and every
  trace line of the main storage IPL, including the heap's own accounting
  lines.
- `diskread 8191 1`, `iob 0600`, `diskread 26 2` under `trace ace,disk`, and
  a rejected `diskread 99999999 1` under `trace csp`: the ACE build, the
  device's own trace, the completion post and release, and the refusal.
- the first 24 instructions of phase 1 under every trace class.

### Unit tests

```
$ build/gcc10/sim36_tests | tail -2
[doctest] assertions: 1307 | 1307 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Both compilers (g++ 10 and g++ 13) build warning-free; MSVC v142 runs the
same in CI.

## Divergences from the reference

| Where | Reference | SIM/36 | Why |
|---|---|---|---|
| phase 1 after instruction 24 | services SVC 10 and runs on to instruction 5418 | refuses `SVC 10 ... not ported yet (milestone 5)` | transfer control and the loader are milestone 5 |
| `conformance` last line | `5418 instruction(s), then: SVC 40 at 1C0E refused ...` | `24 instruction(s), then: SVC 10 at 11BC refused ...` | same; the gate excludes this line only |
| `ipl` (no `pause`) | non-blocking, runs on a driver thread, returns to the prompt | runs in the foreground until the machine stops | the driver and `wait idle` are milestone 6 |
| device completion to a task in an event wait | delivers the element to the task and readies it | refuses by name | task posting is milestone 5 (not reachable in phase 1's first 24 instructions) |
| SVC 41, 42, 43, 46 | diskette, work station, tape | refused by milestone name | milestones 6 and 7 |
| `ipl` with `ipl-source diskette` | reads phase 1 from `#IPLBOOT` | refused by milestone name | milestone 7 |

Every family not listed above that the reference services (00–05, 08,
0B–0D, 10–14, 17, 19–26, 2B, 2E, 30–36, 52) is dispatched and refused with
its milestone named; the refusal stops the machine through the same path a
reference refusal does, so the diagnostic shape is the reference's.

## Trace text and the self-containment rule

The reference's trace lines cite the routine names and addresses it was
decoded from, and the gate scripts grep for those exact lines.  Parity
before improvement: every trace and refusal string is carried verbatim, as
guest-observable output.  The rule against carrying those names into SIM/36
applies to the comments and documentation, which describe the behaviour
instead; a grep over `src/` for the internal names finds them only inside
string literals.

## Reference bugs suspected and left in place

- `conformance` on the reference stops at instruction 5418 with `SVC 40 ...
  the device set refused the request (IOB 701E26, command 00)`: phase 1's
  first disk read through a translated IOB address is refused by the
  reference's own device set.  Recorded, not investigated; SIM/36 will
  reach the same point in milestone 5 and must stop the same way.
- The reference's `DiskRead` monitor command prints the IOB decode and the
  first 64 bytes of the last read even when the call was rejected; ported
  as is.
- `SVC 06`'s chained arm writes a trailing halfword the reference marks as
  INFERRED (the rounded total size); ported as is, with the same trace.

## Open items carried forward

- The M3 trace gate of 5,000 phase-1 instructions and the M4 "same stop,
  same count" gate both wait on SVC 10 (milestone 5).
- `raiseStorageProtection` still returns false (no task to dispatch); the
  level-5 path that dispatches a task is milestone 5.
- `show ptt`, `whereis`, `breakm`, `tasklist`, `mapstate`, `sqsstate`,
  `residency`, `modules`, `modstorage`, `allocchain` (milestone 5);
  `stations`, `listener-auto-signon`, `start`, `stop`, `wait`, `timers`,
  `actions` (milestone 6); diskette, tape, snapshots, panic (milestone 7).
- Work station IPL activation (host-side slot state only, no guest effect)
  arrives with the controller in milestone 6.
