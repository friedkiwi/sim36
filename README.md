# SIM/36

SIM/36 is a portable, MIT-licensed emulator of the IBM System/36,
written in C++17.

## Status

Experimental. SIM/36 can IPL user-supplied SSP 5.1 and SSP 7.5 volumes.
The repository distributes no IBM media or generated SSP volumes. Generation
of a fresh SSP 5.1 volume from the public distribution diskettes is not yet
complete; see `docs/installing-ssp-5.1.md` for the current frontier.

## Something is broken

I know. Pretty much everything is still broken. If you run into a particular
issue, use the `panic` command. This will ask you for what went wrong, and 
how to reproduce it, and will then create a ZIP file in your temp folder which
will contain the full internal state to help me debug it. Attach this ZIP file
to a Github issue, together with the SIM36 output until that point/

## Documentation

`RUNNING.md` covers building, providing a volume, IPL and connecting a
5250 client.

## Licence

MIT.  See `LICENSE`.  Third-party licences are collected in the generated
`THIRD_PARTY_NOTICES`.
