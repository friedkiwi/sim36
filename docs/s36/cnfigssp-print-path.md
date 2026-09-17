# CNFIGSSP master-configuration print: what the path needed

The gate `test/cnfigssp-print-regression.sh` prints the master configuration
record from CNFIGSSP on W1 with the shipped PB-printer topology and requires
the whole report on printer 0.1: the report text, the form feed that closes
the spool entry, and no processor check.  Six things stood between the PRINT
MENU and that report.  Each is listed with the evidence it rests on and with
whether it agrees with the private reference emulator.

## 1. A0-class callees inherit no caller map (reference restored)

`nuprblen` (c18a64d8..c18a64e4) reserves no caller-map room for an A0-class
callee and `nup2000` does not call `nucmclr` for it.  Commit `f4f3090` had
made A0 callees inherit the caller's MAP table, clipped around the module.
That hid real page 1 (SLIC low storage, `$0B97` current task, `$0B9B` work
base) behind the caller's region, so `#DPDM`, `SPALC` and `#CAS1` read zeros
there and the print open did nothing.  The reference rule is back; the
printer IPL rebuild gate that motivated the change still passes with it.

## 2. The privilege byte travels through the request block (beyond the reference)

`rb+19` bit 0 is the task privilege byte `nusvc` tests and `nudyprv`
(SVC 0A) clears.  CNFIGSSP drops privilege with `LPMR 01`, asks for it back
with SVC 0A and then issues a privileged LPMR.  Neither emulator copied that
byte into the MSP's PMR, so the LPMR stopped the machine.  Register save and
restore now carry PMR bit 7 in both directions.  Related: a stale idle flag
made that stop look like the external-event idle; `dispatch()` now clears
the flag whenever a task is selected, so stops report their real reason.

## 3. A translated assign from a program block grows the region (beyond the reference)

`SPALC` assigns the 2560-byte spool buffer from the job's region block with
WR7 = JCB+92 << 8 (the region's used size in 256-byte units, `DA00`).  The
area ends at `E400`, past the 28-page region.  `getHeap` does not answer Low
there: it raises the JCB ceiling through `mspag000` function 04 and retries
(c18cb104..c18cb178).  SVC 2C now publishes the larger page count as SVC 12
would, keeps JCB+92 untouched (SPCLO derives the same base from it when it
frees the buffer), latches +89 and counts +90..91 as `getHeap` does, and
marks every outer request block of the task for an ATR rebuild on resume
(`rb+44` bit 0x40, the request `nupexit` honours at c18a4184).

## 4. Printer output completes when the guest yields (emulator policy)

A printer Put used to complete inside the SVC.  SSP's spool writer polls
its disk reads (`SVC 02` Q=0C, WR6=0020) before it waits for the printer
(Q=4D, WR6=2020), and the synchronous completion let the poll consume the
printer's event.  A printer record is now retained (ECM armed, ACE kept)
and completes in the wait-and-dispatch tail, when the waiter has given the
processor away.  Retained records are part of the snapshot (format 20).

## 5. Event-type matching: the low bytes must share a bit (reference drift)

`nuevt`'s last arm (c18b5e98/c18b5eac) requires `(WR6 & type) & 0xFF` to be
non-zero.  The reference transcribed it as "type even and WR6 low byte
non-zero" and called the AND dead; `docs/s36/console-acquire-and-wake-2026-09-08.md`
in the research tree verified the AND is the test.  With the old arm the
writer's type-0020 polls consumed its own type-0010 general-post element
(built by SVC 4C with Q bit 5, which now stores WR6 at ace+22 as `nuidpost`
does), stored that element's ECM address into a work field the end-of-entry
code later uses as a table index, and read logical `7F9D`.  The writer takes
that element only through its keyed wait (Q=01, XR1 = the ECM).

## 6. Multiple-wait candidates are read through the task's ATRs (beyond the reference)

`nuevt`'s ECM candidate test read `ace+13` as a raw address.  The writer's
disk IOB is a translated `80nnnn` field, so the raw read landed in unrelated
storage and HISTORY's writer progressed by accident.  The byte is now read
through the waiting task's live translation registers.

## What the gate does not require

SSP never issues a printer Clear after the last entry: the writer prints
the form feed, then ends through CTEJB or waits for more work.  The
emulator's "end of job" for a print client remains a Clear or the operator's
`prtend`, exactly as `docs/s36/printer-path.md` records, so the gate accepts
the form feed as the guest's end of entry.
