# Checkpoint 6 — the work station controller, the host layer and the live monitor

Tag: `cp-6`

## What was ported

- **The work station controller** (`Devices/WorkStationController.*`,
  `WorkStationActions.*`, `WorkStationIob.*`, `UnitBlock.*`): the unit
  address routing (unit `FF` is the invite), the IOB command switch and the
  action jump table, cross-checked on every request and refused when they
  disagree; Put, Put-with-invite (one RFC 1205 Put/Get record carrying the
  write and its read command), Read Input Fields with its issue-time buffer
  capture, the staging page selected by the unit block's work-space
  pointer and the deferred delivery onto the work-space block, Clear,
  Cancel Invite, Read Screen (decoded and refused), Get Printer Status,
  Output Data to a printer, the configuration records for command 82, and
  the pending-operation failure path with status 02/03 (device not
  attached) when a display powers off.
- **The devices** (`VirtualWorkstation.*`, `VirtualPrinter.*`,
  `IWorkStationBackend.*`): one per configured station, owned by the
  machine, with the attention and power-off latches read only on the guest
  thread.
- **The host layer** (`Host/Telnet5250Session.*`, `StationBackend.*` with
  the display and printer backends, `StationMultiplexer.*`,
  `ConsoleDisplay.*`, `Sockets.*`): the RFC 1205 / RFC 2877 negotiation,
  record framing, the typed-slot refusal (a display terminal type at a
  printer slot and the reverse), the startup response record, the print
  and null print records, the multiplexer that serves every display
  station from one listener with its "Connect to workstation" prompt, and
  the operator console: an intrinsic attachment that decodes 5250 orders
  into a log (`console`, `console put`, `console send`, `console fields`).
- **The control processor's work station routines**
  (`As36WorkStation.cpp`): SVC 42 and 43 with the action controller, the
  input status delivery into the unit block, the completion of a pending
  read, the M36 transfer binding and its release, the configured unit
  block lookup by unit, and transient 3E (load control) resolving the
  configured unit block and the station's user name.
- **The machine** now owns its stations and printers, starts the listeners
  (`Machine(cfg, const SessionBackends*)` reuses the chassis backends
  across machine lifetimes so a client survives `reset`), and parks the
  guest thread on a native event that socket threads signal.
- **The live monitor** (`Monitor/MonitorCliLive.cpp`): `ipl` returns to
  the prompt and the guest runs on its own thread; monitor commands that
  touch guest state are marshalled onto that thread at the MSP's
  preemption points or at the driver's idle park; `start`, `stop`,
  `wait idle [s]`, `wait <s>`, `reset` against a running machine; the host
  event chain (power-off, input status, input completion, attentions,
  native timers) is drained on the guest thread only.
- **Monitor commands**: `console`, `stations`, `wsconfig`, `wsioch`,
  `wsinput`, `wsread`, `wswrite`, `wsinvite`, `wsoutput`, `wsattach`,
  `wsaid`, `wsstate`, `wsscan`, `wsformat`, `wsentry`, `wshri`, `wsuser`,
  `wspost`, `wspresent*`, `wscontract`, `wsoc`, `prtwrite`, `prtend`, `tu`,
  `tutopology`, `svtubstate`, `wddqstate`, `cptcstate`, `tfrm36`,
  `signoncmd`, `signonreq`, `signonstmt`, `listener-auto-signon`,
  `live`, `power` (removed in the reference; refused with the same text).

## Gate

All runs use the user-supplied volume (`SIM36_VOLUME`); the drivers bind
private ports through `S36_PORT_BASE`.

| suite | checks |
|---|---|
| workstation-actions | 24 passed |
| wsc-config | 4 passed |
| workstation-seam | 7 passed |
| printer-seam | 19 passed (RFC 2877 records byte for byte against a witness client) |
| live-monitor | 12 passed |
| station-multiplex | 44 passed |
| station-power-cycle | 14 passed |
| machine-lifecycle | 3 passed |
| ipl-main-session | attended IPL: console sign-on with date and time, `SYS-5519`, W2 to MAIN and option 1 |
| ipl-unattended-main-session | unattended IPL: W2 through the multiplexer to MAIN, MENU COMMAND, PROGRAM and SEU |
| atr-ownership | passes (two checks that are stale on the reference are pinned to the shared behaviour) |

### Transcript differentials

Identical line for line against the reference, `SVC 2E` (host clock) and
the native timer residue excluded:

- the attended IPL through the console sign-on: `console put` of user,
  date and time, `console send Enter`, and the whole guest run to the next
  idle wait under `trace ws,csp,svc`;
- `test/live-monitor.sim`, the workstation actions vector file, the
  `wsconfig` probe and the work station seam.

### Unit tests

```
[doctest] assertions: 1321 | 1321 passed | 0 failed |
```

Both compilers (g++ 10 and g++ 13) build warning-free.

## Divergences from the reference

| Where | Reference | SIM/36 | Why |
|---|---|---|---|
| stdout through a pipe | .NET's console autoflushes every line | line-buffered on POSIX, unbuffered on Windows | the session drivers read the guest thread's reports (console paints, a display powering off) as they happen; a fully buffered pipe held them until the next prompt |
| display backends | bound to the chassis listener trace before construction and left there | same | printers are rebound to the machine trace at construction, as in the reference; an earlier draft rebound displays too and printed an extra console-record trace line |
| `listen()` on an already-listening backend | the second call is a no-op on the socket | guarded explicitly | the port would otherwise be bound twice |
| reference experiments (`signonexp`, the interactive session experiment, the cnfws power-on aid, the sign-on map gate) | present, off by default | not ported; a comment names each | they fabricate guest state and are not on the default path |

## Reference drift recorded

The reference's own copies of these suites fail against the reference
emulator; the copy here asserts what both emulators actually do, with a
comment at each place.

- `test/workstation-seam.sh` and `test/printer-seam.sh` issue `boot`,
  `wsconfig` and `prtwrite` before any machine exists (refused on both);
  `ipl pause` constructs the machine first here.  The printer suite's live
  halves also let the client negotiate before construction, when the
  pending trace is not applied and the printer still announces the
  backend's default object name; the machine is constructed before the
  client connects.
- `test/workstation-seam.sh` expected `108 byte(s) dropped, no session
  attached` for a write to the console; the console is an intrinsic
  attachment and both emulators account the record as delivered.
- `test/live-monitor.sh` pinned instruction counts (59382 / 75497) the
  reference no longer produces (59388 / 74295).
- `test/machine-lifecycle.py` waited for `volatile machine released`, which
  the reference never prints; it waits for `reset complete; machine stopped
  and configuration editable`.
- `test/ipl-unattended-main-session.py` expected `workspace 800040 selects
  staging displacement 040` for the first MAIN read; the reference reports
  `800640` / `640` on this volume.
- `test/atr-ownership.sh`: a stale module ATR count and a reuse ratio.

## Reference bugs suspected and left in place

- A pending read's captured destination is reported after the pending
  entry is released.  The reference keeps the array alive through garbage
  collection; the port takes the first address before the release so the
  trace line is the same (an earlier draft printed `000000`).

## Not carried

The reference's `test/` directory also holds some two hundred research
probes (`tn5250-*.sim`, `hri-*`, `tfrm36-*`, `mstwa-*`, `cptc-*`,
`nupterm-*`, the monitor-probe pairs `modules-monitor`,
`residency-monitor`, `breakm-monitor`, `smf-status`,
`msipl-entry-lifecycle`, `tu-monitor`, `tutopology`, `wddqstate-monitor`,
...).  They are working notes against the private volume rather than
gates, most of them predate later changes of the reference, and they are
not part of the acceptance path; the milestone 8 review lists them.

## Open items carried forward

- Diskette and tape (SVC 41/46), phase 1 from `#IPLBOOT`, snapshots and
  panic dumps: milestone 7.
