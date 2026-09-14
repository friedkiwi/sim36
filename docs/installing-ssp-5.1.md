# SSP 5.1 generation from the archived distribution media

## Current status

This is **not yet a complete installation procedure**. SIM/36 can convert
the publicly archived SSP 5.1 diskettes, IPL the first diskette, run
`#IPLBOOT` / `MSPID`, and reach SSP's `SSP GENERATION AND RELOAD` program
(`#MSREL`). Starting with a blank fixed disk does not currently produce a
bootable SSP installation: the run stops with `SYS-3908 System error--an
invalid SSP was found`.

The repository contains no IBM distribution media and no generated SSP
volume. There is no prebuilt-volume fallback. The steps below are useful for
reproducing the implemented part of the path and for continuing development;
they do not install SSP end to end.

## Public media

The 5363/5364 SSP 5.1 diskette archive is publicly mirrored by bitsavers:

- <http://bitsavers.trailing-edge.com/bits/IBM/System_36/5363/S36-5.25.zip>
- <http://bitsavers.trailing-edge.com/bits/IBM/System_36/5_inch/>

The first archive contains `SSP-5.1/SSP51-01.IMD` through
`SSP51-11.IMD`, plus `MCODE11.IMD` and `MCODE12.IMD`. The primary
`bitsavers.org` host may reject command-line downloads with HTTP 403; the
trailing-edge mirror serves the same archive.

The captures use ImageDisk (`.IMD`). Cylinder 0 contains 26 256-byte label
sectors; the data tracks contain 8 1024-byte sectors.

## Prerequisites

- A SIM/36 checkout and C++ build environment.
- `VCPKG_ROOT` configured for the repository's CMake preset.
- Python 3, `curl`, and `unzip`.

From the repository root:

```sh
cmake --preset linux
cmake --build --preset linux
```

The resulting executable is `build/linux/sim36`.

## 1. Download and unpack the archive

```sh
mkdir -p work/ssp51
curl -L -o work/ssp51/S36-5.25.zip \
  http://bitsavers.trailing-edge.com/bits/IBM/System_36/5363/S36-5.25.zip
unzip -o -d work/ssp51 work/ssp51/S36-5.25.zip
ls work/ssp51/S36-5.25/SSP-5.1
```

The last command should show `SSP51-01.IMD` through `SSP51-11.IMD`.

## 2. Convert the diskettes

SIM/36 attaches flat sector data rather than ImageDisk containers:

```sh
python3 tools/imd2flat.py --allow-bad -o work/ssp51/flat \
  work/ssp51/S36-5.25/SSP-5.1/SSP51-*.IMD \
  work/ssp51/S36-5.25/MCODE11.IMD \
  work/ssp51/S36-5.25/MCODE12.IMD
```

One sector in the archived `SSP51-11.IMD` is marked unreadable. The explicit
`--allow-bad` above zero-fills that sector; without it the converter refuses
the capture rather than silently inventing data. Volume 11 is not needed to
reach the generation program.

## 3. Create a blank fixed disk and inspect volume 1

The configured Advanced/36 geometry is 819200 256-byte sectors:

```sh
truncate -s 209715200 work/ssp51-new.img

build/linux/sim36 -s /dev/stdin <<'EOF'
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl pause
diskette
quit
EOF
```

The diskette report should identify a 5.25-inch volume with owner
`5727SS65190V01`.

## 4. Reproduce the current generation frontier

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
set machine load-source diskette
set machine ipl-type attend
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl pause
poke 0850 8D
start
wait idle 120
console
quit
EOF
```

The `poke` supplies a customize byte that a real machine derives from its
unit definition table. SIM/36 does not yet synthesize that table.

The expected frontier is:

```text
SSP GENERATION AND RELOAD - MESSAGES
Relocating system area
SYS-3908 System error--an invalid SSP was found
```

Reaching this panel proves that diskette IPL, `#IPLBOOT`, `MSPID`, and entry
to `#MSREL` worked. It does **not** mean that SSP was installed, and the
blank fixed disk is not bootable afterwards.

## What remains before this can be called an installation guide

The missing work is:

1. Initialize the blank fixed disk with the system-area, VTOC, and unit
   definition structures expected by `#MSREL`.
2. Preserve and automate the multi-volume exchange sequence for the
   `SSPMRI`, `SSPBASE`, and microcode data sets.
3. Replace the customize-byte `poke` with unit-definition-table synthesis.
4. Add a clean-room regression that starts only with a blank fixed disk and
   the downloaded archives, completes generation, re-IPLs from fixed disk,
   and reaches SSP sign-on.

Until those four items are implemented and tested, users cannot install SSP
5.1 from the public archives by following repository instructions alone.
