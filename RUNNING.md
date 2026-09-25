# Running SIM/36

A short operator guide: build, provide a volume, IPL, and drive the SSP
sign-on from a live tn5250 client.  Everything below runs from the
repository root (or from the directory an installed archive was unpacked
into).

## 1. Build

```sh
cmake --preset linux           # or: linux-make, windows-static
cmake --build --preset linux
ctest --preset linux           # the volume gates skip until a volume is given
```

The traditional out-of-source flow works as well:

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

Both forms use an existing `VCPKG_ROOT` when one is set.  Otherwise CMake
fetches and bootstraps the vcpkg version pinned by `vcpkg.json` inside the
chosen build directory.

The traditional flow writes `build/sim36`.  With presets, the binary is
`build/linux/sim36` (`build\windows-static\Release\sim36.exe` on Windows).
The packaged archives on the releases page contain the same binary with
`etc/`, `README.md`, this file and `THIRD_PARTY_NOTICES`.

## 2. The volume and the startup command file

SIM/36 ships no System/36 volume.  Put a volume image at `var/as36.img`
(the appliance startup file attaches `../var/as36.img` relative to `etc/`),
or point the `attach disk0` line of your own command file at it.

```
sim36                        # reads etc/sim36.sim when it exists, then prompts
sim36 -c etc/sim36.sim       # the same, explicitly
sim36 -c machine.sim         # your own startup file
sim36 -s experiment.sim      # execute a command file and exit
sim36 -t disk,ws             # initial trace classes
```

`etc/sim36.sim` is an ordinary monitor command file: it declares station
0.0 as the console, 0.1 as a 5224 Model 1 printer writing to the monitor
console, and 0.2..0.6 as displays, then leaves you at the prompt
with the listeners open, no volume attached and no machine constructed.
`attach disk0 var/as36.img overlay` attaches the volume (writes stay in
memory and never reach the file); the appliance file does that for you.  `show config`
prints the machine as configured; `save config stdout` prints it back as a
replayable command file.

> Use `overlay`, or a private copy, and never share an image or a port
> between concurrent runs.  A read-write attach takes an exclusive lock
> (`<image>.lock`) and refuses a second attach by name.  Listener ports are
> not protected; pick private ports when running several emulators.

Media commands:

```
attach disk0 <image> [ro|rw|overlay]
attach diskette0 <image> [ro|rw]      # flat diskette image
attach tape0 <folder> [ro|rw]         # folder tape with its manifest
detach <device>
diskette / dsktread / dsktwrite       # inspect or drive the diskette
tape status|vtoc|files|...            # inspect or drive the tape
```

Folder-tape layout, validation, unpack/repack, extraction, and fixed-record
EBCDIC import are documented in `docs/tape-folder-format.md`.  The
standard-library-only CLI is `tools/tape-folder.py`; its synthetic tests do not
need an SSP volume.

Fixed disks and diskettes are attached read-write when the mode is omitted.
Use `ro` for media that must not be changed.

`set machine ipl-source diskette` boots phase 1 from the `#IPLBOOT` data
set of the attached diskette instead of the fixed disk.

## 3. Run it as an appliance

```sh
sim36 -c etc/sim36-appliance.sim
```

That file includes `etc/sim36.sim`, attaches `var/as36.img` as an overlay
and ends with a bare `ipl`.  The IPL
starts execution on the guest thread and returns to the `sim36> ` prompt;
the guest keeps running across its idle waits, and a terminal that attaches
later is powered on and given a sign-on.

For a different machine, copy the command file, change its `set` and
`attach` commands, and end it with `ipl`.

### 3a. Keeping the monitor while the machine runs

`ipl` runs the freshly reset machine on a guest thread of its own.  Every
monitor command keeps working (`dump`, `tasklist`, `breakm`, `watch`,
`snapshot save`, `console put`, `console send`, ...); each one is executed
by the guest thread at its next architected preemption point, so it sees
the same state a scripted command sees.  `stop` stops execution at a safe
point, `start` resumes it, `show status` reports which it is.

`wait idle [seconds]` blocks the monitor (not the machine) until the guest
parks in its idle wait.  It replaces guessing an instruction count with a
condition:

```
set machine ipl-type attend
ipl
wait idle 120           ; the IPL SIGN ON panel is now on the console
console put 6 56 QSECOFR
console put 16 56 090896
console put 17 56 120000
console send Enter
wait idle 120
console
```

`ipl pause` resets and performs the control storage IPL but returns before
the first guest instruction; it is the deterministic entry for `step N`, and
`start` continues normally from there.  `step`, `boot` and `reset` are
refused while execution is in progress (`stop` first).

## 4. Connecting a 5250 client

The console (station `0.0`) is available through the station multiplexer
and through the monitor's `console`, `console put`, and `console send`
commands. On an attended IPL it receives the `IPL SIGN ON` panel; on an
unattended IPL SSP completes without writing to it, and you sign on through
one of the workstations.

By default one listener, the station multiplexer on `127.0.0.1:2300`,
serves every display station.  Connect anything that speaks 5250:

```sh
tn5250 telnet://127.0.0.1:2300
```

You get a panel asking which station to connect to. Type its `port.address`
identifier, such as `0.0`, and press Enter; the field is pre-filled with
the lowest free station and the hint states the required notation. A client
that sends an RFC 2877 device name that is a station
(`tn5250 env.DEVNAME=0.2 telnet://127.0.0.1:2300`) goes straight there.
Selecting `0.0` immediately shares the console device with the monitor's
`console` commands.

The multiplexer is not part of the machine: it comes up when configured,
with or without a constructed machine, and survives `reset` and the next
`ipl`. A client that picked `0.2` while the machine was stopped is on that
station when the guest IPLs, on the same socket.

```
set terminal multiplex off                     # one listener per station instead
set terminal multiplex listen 127.0.0.1:2400   # move the multiplexer
set station 0.3 listen 127.0.0.1:2403          # a per-station listener
```

Printers have one output attachment. They can keep their own TN5250 listener,
write decoded output to the monitor console, or append the exact guest byte
stream to a file:

```
set station 0.4 role printer
set station 0.4 device-code PB
set station 0.4 output tn5250
set station 0.4 listen 127.0.0.1:2404
set station 0.4 output console
set station 0.4 output file spool/printer-04.bin
set station 0.4 output txtout spool/printer-04
set station 0.4 output pdfout spool/printer-04-pdf
set station 0.4 paper green
```

`file` is the legacy raw-byte output and appends every job to one file.
`txtout` decodes EBCDIC/SCS and atomically publishes one file per completed
job as `job-000001.txt`, `job-000002.txt`, and so on. Existing numbers are
never reused across emulator restarts. A guest Clear Printer command ends a
job; `prtend 0.4` provides the same boundary from the monitor.

`pdfout` uses the same job boundaries and numbering to produce wide,
66-line fanfold forms with an embedded IBM Plex Mono font. Paper can be
`green` (the default), `blue`, `gray`, `orange`, or `white`. The IBM Plex
font is bundled under OFL-1.1; an IBM 1403 imitation font is not bundled
because the referenced distribution does not state a redistribution license.

Selecting console, file, txtout, or pdfout output clears the listener; assigning a listener is
refused until output is switched back to `tn5250`. Printer slots are never
offered by the display multiplexer. A 5250 printer client such as `lp5250d`
receives RFC 2877 print records.
`prtwrite` and `prtend` drive a printer from the monitor.

`stations` shows which port each station is reached through and what is on
it.  Hanging up a client powers its display off: whatever the guest had
outstanding against that terminal fails with "device not attached", and the
next client on that station is a new power-on with its own sign-on.

## 5. A complete unattended session, scripted

`test/ipl-unattended-main-session.py` is the executable form of this
guide: it starts the emulator on private ports, IPLs unattended, connects
a headless 5250 client (`test/tn5250drive.py`), picks `0.1`, signs on, walks
MAIN to MENU COMMAND and PROGRAM and starts SEU.  `test/ipl-main-session.py`
does the attended form through the console.  Both need `SIM36_VOLUME`.

## 6. Snapshots and panic dumps

`snapshot save <file>` writes the machine (main storage, registers,
control processor, scheduler, devices, stations, tape position) as a
`S36CKPT` archive; `snapshot load <file>` restores it into a machine with
the same configuration.  Live TN5250 sessions and pending host callbacks
are not serialisable: detach the client at the boundary you want, save,
then reconnect after loading.

`panic` asks two questions (what happened, how to reproduce it), writes an
owner-only zip with the configuration replay, trace settings, station
state, main storage, the runtime and control processor state, the last I/O
buffers and the deferred trace tail, prints the file name, and leaves.  The
archive never contains the volume, diskette or tape media.

## 7. Tracing and inspection

```
trace csp,svc,ace,disk,ws,sched     ; trace classes; also msp, or off
trace ws                            ; the work station path only
tasklist / whereis / modules        ; the SSP tasks, the current member, the loaded members
dump 08AB 1                         ; guest storage
tu 00E870 / iob 020600 / ace 03C0   ; decoded control blocks
sqsstate / mapstate / actions       ; system queue space, address mapping, action controller
```

`help` lists every command by group.  The trace lines name the guest
structures they decode and, where the emulator refuses something, say what
was refused and why: a refusal is a statement about what is known, not a
placeholder.
