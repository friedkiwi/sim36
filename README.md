# SIM/36

SIM/36 is a portable, MIT-licensed emulator of the IBM System/36,
written in C++17.

It models the main storage processor, the control storage processor, the
fixed disk, diskette and tape devices and the work station controller, and
serves real 5250 terminals and printers over TN5250.  The operator surface
is a monitor with one command language: startup files, included files and
interactive input all go through the same command processor, so any
session is reproducible from a command file.

## Design rules

**Nothing is fabricated.**  Where the behaviour of the machine is not
known, the emulator refuses, stops, and names the point in its trace.

**Inspection is a feature.**  Every on-disk and in-storage structure can be
listed, decoded and traced from the monitor.

## Status

Experimental.  SIM/36 can IPL an SSP 5.1 volume created from the
distribution media on bitsavers (see `docs/installing-ssp-5.1.md`), and an
SSP 7.5 disk image dumped from an Advanced/36.  No volume image is
distributed with the emulator.

## Documentation

`RUNNING.md` covers building, providing a volume, IPL and connecting a
5250 client.

## Licence

MIT.  See `LICENSE`.  Third-party licences are collected in the generated
`THIRD_PARTY_NOTICES`.
