# SSP 5.1 generation from the archived distribution media

## Current status

This is **not yet a complete installation procedure**. SIM/36 can convert
the publicly archived SSP 5.1 diskettes, IPL the first diskette, initialize
the blank fixed disk's SSP system area, restore `SSPMRI` and `SSPBASE`, and
read both 5364 microcode volumes. It does not yet complete the hardware
microcode-load phase, so the resulting fixed disk is not bootable.

The repository contains no IBM distribution media or generated SSP volume,
and there is no prebuilt-volume fallback. The steps below reproduce the
implemented path for continuing development; they do not install SSP end to
end.

## Public media

The 5363/5364 SSP 5.1 diskette archive is publicly mirrored by bitsavers:

- <https://www.bitsavers.org/bits/IBM/System_36/5363/S36-5.25.zip>
- <https://www.bitsavers.org/bits/IBM/System_36/5363/>

The archive contains `SSP-5.1/SSP51-01.IMD` through `SSP51-11.IMD`, plus
the `MCODE11`/`MCODE12` and `MCODE61`/`MCODE62` pairs. The 5364 personality
used by the virtual Advanced/36 selects `DSKT11` and `DSKT12`; the 5363
personality selects `DSKT61` and `DSKT62`.

The captures use ImageDisk (`.IMD`). Cylinder 0 contains 26 256-byte label
sectors. The SSP volumes' data tracks contain 8 1024-byte sectors; the
microcode volumes use 15 512-byte sectors.

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
  https://www.bitsavers.org/bits/IBM/System_36/5363/S36-5.25.zip
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

## 4. Restore the base SSP volumes

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
set machine load-source diskette
set machine ipl-type attend
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl pause
start
wait idle 120
console
EOF
```

No `poke` is required. On a blank disk the virtual Advanced/36 publishes
system-customize value `8D`, the 5364-family value accepted by SSP 5.1. An
installed disk's unit definition table replaces this seed during every IPL.

The first panel contains `SYS-3908`; press Enter to reveal the more specific
`SYS-3922 SSP level error. SSP = 05, Microcode = 00`, then press Enter again.
These are diagnostics for the initially empty system area, not a terminal
failure. The reload initializes the system files, processes volume 1, and
asks for volume 2.

For each requested base volume through `SSP51-07.img`, replace the diskette
and continue:

```text
diskette insert work/ssp51/flat/SSP51-02.img
console send Enter
wait idle 120
```

Repeat for volumes 03 through 07. Volumes 08 through 11 contain optional
products and are not part of the base SSP restore.

## 5. Reproduce the current microcode frontier

After volume 07, the panel names functional microcode volume `DSKT12` and
also says to insert additional microcode volumes first. Supply both volumes
in this order:

```text
diskette insert work/ssp51/flat/MCODE11.img
console send Enter
wait idle 120
diskette insert work/ssp51/flat/MCODE12.img
console send Enter
wait idle 120
```

The current frontier is:

```text
SSP GENERATION AND RELOAD - MESSAGES
Relocating system area
SYS-3913 Microcode error. Type-81. Module ID-801E WSDVCCS
```

SIM/36's virtual control processor has no writable hardware microcode store.
Diskette command `DF`, issued once per hardware module by the reload, is
currently acknowledged without implementing that load. Acknowledging the
panels exposes the other affected hardware modules and eventually reaches an
invalid zero-length SVC 06 request. This remains an emulator gap, not a usable
way to finish generation.

## What `0850` means

Low-storage byte `0850` and its copy at `08BD` are the first customize byte
of the system record in the unit definition table at fixed-disk sector 26.
The control-storage IPL copies that byte from an installed disk. For a blank
disk, the virtual Advanced/36 now seeds both locations with `8D` before MSP
starts.

SSP 5.1's `MSPID` checks `0850` twice and accepts only `8B` or `8D`. The
choice remains live later in `#MSREL`: `8B` requests the 5363 `DSKT61/62`
pair, while `8D` requests the 5364 `DSKT11/12` pair. It is therefore machine
configuration, not a magic “continue installation” flag.

An existing SSP 7.5 volume supplies `89` from its UDT, so the new blank-disk
seed has no effect on it. SSP 7.5 is more permissive and normalizes the value
during `MSNIP`: `89` and `8C` become `8E`; `8E` and `8F` are retained; other
tested values, including `00`, `8B`, `8D`, and `FF`, become `8D`. Every tested
value still reached sign-on. Thus a manual `poke 0850 8D` changes SSP 7.5's
running compatibility branch, but it neither repairs nor updates its on-disk
UDT.

## What remains before this can be called an installation guide

The missing work is:

1. Decode and implement the Advanced/36-native contract for the `DE`/`DF`
   microcode-load operations, or explicitly virtualize the whole hardware
   microcode phase without presenting false per-module failures.
2. Complete generation and verify that it writes the final UDT and boot
   records required for a fixed-disk IPL.
3. Automate the media exchange sequence and add a clean-room regression that
   starts only with a blank fixed disk and the downloaded archive, completes
   generation, re-IPLs from fixed disk, and reaches SSP sign-on.

Until those three items are implemented and tested, users cannot install SSP
5.1 from the public archives by following repository instructions alone.
