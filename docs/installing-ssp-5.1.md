# Install SSP 5.1

This guide creates and boots a new SSP 5.1 fixed-disk image. Start in the
root of a SIM/36 checkout with SIM/36 and a `tn5250` client ready to run.

## 1. Download and unpack the archive

Download [S36-5.25.zip](https://www.bitsavers.org/bits/IBM/System_36/5363/S36-5.25.zip)
and extract it under `work/ssp51`. The commands below expect to find
the `SSP51-*.IMD` files under
`work/ssp51/S36-5.25/SSP-5.1`.

## 2. Convert the diskettes

SIM/36 attaches flat sector data rather than ImageDisk containers:

```sh
python3 tools/imd2flat.py -o work/ssp51/flat \
  work/ssp51/S36-5.25/SSP-5.1/SSP51-{01,02,05,06,07}.IMD \
  work/ssp51/S36-5.25/MCODE12.IMD
```

These are the base SSP volumes requested by generation. Volumes 03 and 04
continue the optional `HELPMRI` data set, and volumes 08 through 11 contain
optional products.

## 3. Create a blank fixed disk

The configured Advanced/36 geometry is 819200 256-byte sectors:

```sh
truncate -s 209715200 work/ssp51-new.img
```

## 4. Restore the base SSP volumes

Start SIM/36 and leave its monitor running:

```text
sim36
```

At the `sim36>` prompt, configure the installation media and terminal
multiplexer:

```text
set terminal multiplex on
set terminal multiplex listen 127.0.0.1:2300
set machine load-source diskette
set machine ipl-type attend
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
```

In another terminal, connect a 5250 client to the multiplexer:

```sh
tn5250 telnet://127.0.0.1:2300
```

On the station-selection panel, enter `W1` and press Enter. Then start the
installation from the SIM/36 monitor:

```text
ipl
```

W1 first displays `SYS-3908`. Press Enter to reveal the more specific
`SYS-3922 SSP level error. SSP = 05, Microcode = 00`, then press Enter again
on W1. These are diagnostics for the initially empty system area, not a
terminal failure. The reload initializes the system files, processes volume
1, and asks for volume 2.

When SSP requests the next `SSPMRI` volume, replace the diskette from the
SIM/36 monitor:

```text
diskette insert work/ssp51/flat/SSP51-02.img
```

Press Enter on W1 and wait for the next media request. The base installation
then requests `SSPBASE`, which starts on volume 05; volumes 03 and 04 are not
needed. Insert volumes 05, 06, and 07 in the same way as SSP requests them.

## 5. Finish generation

After volume 07, SSP requests functional microcode volume `DSKT12`. Insert:

```text
diskette insert work/ssp51/flat/MCODE12.img
```

Press Enter on W1 and wait for generation to finish. `MCODE11` is an optional
additional physical-microcode volume and is not needed by the virtual
Advanced/36.

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
do not prevent the generated SSP from running. Press Enter when prompted to
advance through the messages. Once the panel includes `SSP generation
complete, MSIPL from disk required`, generation is finished: do not press
Enter again even if the panel still offers it. Return to the SIM/36 monitor
and IPL the generated fixed disk:

```text
diskette eject
stop
reset --yes
set machine load-source disk
ipl
```

The multiplexer connection survives the reset. W1 should display the SSP 5.1
IPL sign-on screen.
