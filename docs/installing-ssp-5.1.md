# SSP 5.1 generation from the archived distribution media

## Current status

SIM/36 can convert the publicly archived SSP 5.1 diskettes, IPL the first
diskette, initialize the blank fixed disk's SSP system area, restore `SSPMRI`
and `SSPBASE`, read both 5364 microcode volumes, and IPL the generated fixed
disk. The generated SSP is bootable.

The repository contains no IBM distribution media or generated SSP volume,
and there is no prebuilt-volume fallback. The user supplies the archived
distribution media and creates the fixed-disk image locally.

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

## 3. Create a blank fixed disk

The configured Advanced/36 geometry is 819200 256-byte sectors:

```sh
truncate -s 209715200 work/ssp51-new.img
```

## 4. Restore the base SSP volumes

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
set machine load-source diskette
set machine ipl-type attend
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl
wait idle 120
console
EOF
```

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

## 5. Finish generation

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

SSP finishes generation and displays a completion panel similar to:

```text
SSP GENERATION AND RELOAD - MESSAGES
Relocating system area
SYS-3913 Microcode error. Type-81. Module ID-801E WSDVCCS
SSP reload complete, remove diskettes.
Microcode load complete, remove diskette.
SSP generation complete, MSIPL from disk required.
```

The `SYS-3913` entries describe unresolved references in physical 5364
control-storage modules. The Advanced/36 CSP implements those services
natively and does not load or retain the physical microcode, so these entries
do not prevent the generated SSP from running. Do not press Enter repeatedly
to page through the unused physical-microcode diagnostics. Once the panel says
`SSP generation complete`, remove the diskette and IPL the generated fixed
disk in a fresh SIM/36 invocation:

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
set machine load-source disk
attach disk0 work/ssp51-new.img rw
ipl
wait idle 120
console
EOF
```

The resulting display should be the SSP 5.1 IPL sign-on screen.
