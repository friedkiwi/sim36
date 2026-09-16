# SSP system power-off

SSP's **POWER OFF** command performs its shutdown work and then transfers to
`#CCPW`, the resident control-command power program. On physical System/36
hardware that path removes system power. There is no host instruction or SVC
for an emulator to intercept.

The panic dump `sim36-panic-20260916-181227-07f9ac2e.zip` captured SSP 5.1
after the panel reported `POWER OFF command successful`. The only runnable
task was executing `#CCPW+03E5`, logical IAR `13E5`. Its translated bytes were:

```
F1 87 03    JC condition-on, backward 3
```

The branch target is the instruction itself. With PSR `02`, its `87`
condition remains true, so interpreting it as an ordinary program loop
consumes the host indefinitely. This is the final hardware power-control
wait, not an SSP idle wait: no device requests or scheduler events were
pending.

SIM/36 recognizes only that exact self-loop while the active loaded member is
`#CCPW`. It halts the MSP with a power-off reason and reports the transition on
the monitor. The member-relative offset is diagnostic rather than part of the
signature so another SSP build may relocate the instruction without turning
unrelated loops into power-off requests.
