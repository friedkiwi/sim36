# SIM/36

SIM/36 is a portable, MIT-licensed emulator of the IBM System/36 family,
written in C++17.  It models the main storage processor, the control storage
processor behind a service interface, the fixed disk, diskette and tape
devices, the work station controller, and a host layer that serves real 5250
terminals and printers over TN5250.

The emulator's operator surface is a monitor with one command language.  The
startup configuration, included files and interactive input all pass through
the same command processor, so any session is reproducible from a command
file.  `RUNNING.md` is the operator guide.

## Design rules

**Nothing is fabricated.** Where the behaviour of the machine is not known,
the emulator refuses, stops, and names the point in its trace.  A message
that says something is not implemented is a statement about what is known,
not a placeholder.

**Inspection is a feature.** Every on-disk and in-storage structure can be
listed, decoded and traced from the monitor.

## Building

Requirements: a C++17 compiler (g++ 10 or later, or MSVC 2019 v142), CMake
3.21 or later, and [vcpkg](https://github.com/microsoft/vcpkg).  Set
`VCPKG_ROOT` to the vcpkg checkout; the manifest pins its baseline.

```sh
cmake --preset linux             # or: linux-make, windows-static
cmake --build --preset linux
ctest --preset linux
./build/linux/sim36
```

Every dependency is fetched and built statically through vcpkg.  The build
writes `THIRD_PARTY_NOTICES` next to the binary from the licence files vcpkg
records for the exact versions it built.  The Windows build is a static
binary that runs on Windows 7 and later.

| Need | Library | Licence |
|---|---|---|
| line editing | replxx | BSD-3 |
| zip containers (snapshots, panic dumps) | minizip-ng, zlib | zlib |
| JSON (tape manifests) | nlohmann-json | MIT |
| formatting | fmt | MIT |
| command-line options | cxxopts | MIT |
| unit tests | doctest | MIT |

## Running

```sh
sim36                        # reads etc/sim36.sim if present, then prompts
sim36 -c etc/sim36-appliance.sim   # IPL unattended; connect a 5250 client to :2300
sim36 -c machine.sim         # use this startup command file
sim36 -s experiment.sim      # execute a command file and exit
sim36 -t disk,ws             # initial trace classes
```

The prompt is `sim36> `.  Type `help` for the command list and `quit` to
leave.  `do <file>` executes another command file; paths inside a command
file are relative to that file.

## Media

No System/36 volume image is distributed with SIM/36.  `etc/sim36.sim`
attaches none, so the bare emulator always starts; `etc/sim36-appliance.sim`
attaches `var/as36.img` as an overlay and IPLs, so put your own volume there
or change the path.  Tests that need a volume read `SIM36_VOLUME` at run time and
skip (ctest return code 77) when it is unset.

## Testing

```sh
ctest --preset linux                                   # unit tests and volume-free gates
SIM36_VOLUME=/path/to/as36.img ctest --preset linux    # every gate
```

The gates under `test/` are shell and Python suites that drive the
emulator through its monitor, attach headless 5250 clients
(`test/tn5250drive.py`) and assert the trace lines, the wire records and
the panels.  `tools/diffrun.sh` runs the same command file through a
second emulator named by `SIM36_REFERENCE` and diffs the normalised
transcripts; it is how parity with the reference implementation was
proven, milestone by milestone.

## Status

SIM/36 was built in eight milestones against a reference implementation,
with the differential harness proving that both produce the same trace for
the same command file.  The checkpoint reports under `docs/checkpoints/`
record what each milestone delivered, the gate commands with their real
output, and every known divergence:

| checkpoint | subject |
|---|---|
| 00 | skeleton, toolchain, CI |
| 01 | storage, the monitor command language |
| 02 | the disk layer: VTOCs, libraries, the boot record |
| 03 | the main storage processor |
| 04 | guest low storage, system queue space, the CSP scaffolding |
| 05 | the supervisor call families, the dispatcher, the loader |
| 06 | the work station controller, TN5250 hosts, the live monitor |
| 07 | diskette, tape, snapshots, panic dumps |
| 08 | the cutover review, documentation, release |

With a volume: an unattended IPL completes, a 5250 client connects through
the multiplexer, signs on and navigates MAIN, MENU COMMAND, PROGRAM and
SEU; an attended IPL is answered on the console.  Where the emulator does
not know what the machine does, it stops and says so.

## Licence

MIT.  See `LICENSE`.  Third-party licences are collected in the generated
`THIRD_PARTY_NOTICES`.
