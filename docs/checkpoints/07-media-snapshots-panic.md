# Checkpoint 7 — diskette, tape, snapshots and panic dumps

Tag: `cp-7`

## What was ported

- **Diskette** (`Storage/DisketteBackend.*`, `Devices/VirtualDiskette.*`):
  flat images with the geometry read from the volume and data set labels,
  SVC 41 (the command set the reference decodes, the label and sector
  addressing, the refusals), the operator commands `diskette`
  (mount/unmount/status), `dsktread` and `dsktwrite`, and the `ipl-source
  diskette` load of phase 1 from the `#IPLBOOT` data set with the
  reference's operator-error texts when the diskette or the member is not
  usable.
- **Tape** (`Storage/TapeBackend.h`, `FolderTapeBackend.*`,
  `TapeManifest.*`, `Devices/VirtualTape.*`): a folder of files with a JSON
  manifest (serialised by hand so the file is byte-identical to the
  reference's), block and mark positioning, read-only mounts, SVC 46, and
  the operator commands `tape` (mount/unmount/status/vtoc/files),
  `tapetest` and `tapesvc` (the self-checks), `savemain`.
- **Configuration**: `attach diskette`, `attach tape`, the read-only flags,
  and construction-time refusals (`ConfigError`) when a configured
  diskette or tape folder cannot be used, with the reason.
- **Snapshots** (`Monitor/MachineSnapshot.*`, `As36Checkpoint.cpp`,
  `Machine::restoreCheckpoint`): the `S36CKPT` version 15 archive (zip,
  minizip-ng) carrying main storage, the address translation registers,
  the registers, the MSP, scheduler, control processor, device, station,
  printer and tape state; `snapshot save/load`; the control processor's
  capture and restore with the reference's failure texts when an archive
  names a station or unit the configuration does not have.
- **Panic dumps** (`Monitor/PanicDump.*`): `panic` asks the operator the
  two questions, then writes an owner-only zip with the configuration
  replay, the trace settings, the chassis listeners, and when a machine
  exists its main storage, runtime state, task list, control processor
  state, system queue space, devices, the last I/O buffers, scheduler,
  stations, printers and the deferred trace tail; capture errors go into
  `capture-errors.txt` and `panic` terminates the process.

## Gate

| suite | checks |
|---|---|
| tape | 14 passed |
| tape-svc | 11 passed |
| tape-operator | 23 passed |
| ipl-source | 25 passed |
| snapshot | 6 passed |
| panic-command | passes (prompt, capture, owner-only file, exit; no media payload) |
| system-queue-space | 22 passed, the two snapshot round-trip checks deferred at checkpoint 5 included |

`ctest` runs every gate (46 tests) with the earlier ones; each skips with
return code 77 when `SIM36_VOLUME` is unset, which is what CI does.

### Transcript differentials

`tapeops.sim` (mount, status, vtoc, files, `tapesvc`, unmount) and the
diskette read path are identical line for line against the reference.

### Unit tests

```
[doctest] assertions: 1321 | 1321 passed | 0 failed |
```

## Divergences from the reference

| Where | Reference | SIM/36 | Why |
|---|---|---|---|
| empty last-I/O buffer in a panic dump | `ZipArchive` writes a zero-length entry | a zero-length entry is written from a static byte | minizip-ng refuses a null buffer; the entry set is the reference's |
| zip writer | .NET `System.IO.Compression` | minizip-ng (zlib licence) | the archive layout, entry names and the `S36CKPT` header are the reference's; the deflate streams differ byte for byte, which nothing reads back |
| `snapshot` media directory | `Path.GetTempPath()` | `TMPDIR` / the platform temporary directory | same intent |

## Reference drift recorded

- `test/panic-command.sh` expects a 1 MB `runtime/main-storage.bin`; the
  reference writes the whole 16 MB backing store of an Advanced/36 and
  fails its own check.  The copy here asserts 16 MB.
- `test/ipl-source.sh` traces, resets and then steps; since `reset` began
  releasing the construction latch on the reference that sequence stops at
  `'step' requires a constructed machine`.  The copy here traces, then `ipl
  pause`, then steps, which is what the gate meant.
- The tape suites issue their first machine commands before construction
  in the reference's copies; `ipl pause` comes first here.

## Reference bugs suspected and left in place

- None new.  The task work area tie-break and the retained composite
  allocation from checkpoint 5 still apply.

## Open items carried forward

- Milestone 8: the cutover review (no reference paths, no monorepo names,
  no SSP bytes in the tree), the user documentation, the release
  artefacts and `THIRD_PARTY_NOTICES`, and the list of reference research
  probes not carried.
