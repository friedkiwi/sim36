# SIM/36

SIM/36 is a portable, MIT-licensed emulator of the IBM System/36 family,
written in C++17.  It models the main storage processor, the control storage
processor behind a service interface, the fixed disk, diskette and tape
devices, the work station controller, and a host layer that serves real 5250
terminals over TN5250.

The emulator's operator surface is a monitor with one command language.  The
startup configuration, included files and interactive input all pass through
the same command processor, so any session is reproducible from a command
file.

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
records for the exact versions it built.

| Need | Library | Licence |
|---|---|---|
| line editing | replxx | BSD-3 |
| zip and gzip containers | minizip-ng, zlib | zlib |
| JSON | nlohmann-json | MIT |
| formatting | fmt | MIT |
| command-line options | cxxopts | MIT |
| unit tests | doctest | MIT |

## Running

```sh
sim36                        # reads etc/sim36.sim if present, then prompts
sim36 -c machine.sim         # use this startup command file
sim36 -s experiment.sim      # execute a command file and exit
sim36 -t disk,ws             # initial trace classes
```

The prompt is `sim36> `.  Type `help` for the command list and `quit` to
leave.  `do <file>` executes another command file; paths inside a command
file are relative to that file.

## Media

No System/36 volume image is distributed with SIM/36.  Tests that need one
read a user-supplied image path at runtime and report "no volume" when it is
absent.

## Status

SIM/36 is being built milestone by milestone against a reference
implementation, with a differential harness proving that both produce the
same trace for the same command file.  The checkpoint reports under
`docs/checkpoints/` record what each milestone delivered, the gate commands
with their real output, and every known divergence.

## Licence

MIT.  See `LICENSE`.  Third-party licences are collected in the generated
`THIRD_PARTY_NOTICES`.
