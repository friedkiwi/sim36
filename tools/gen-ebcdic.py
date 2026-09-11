#!/usr/bin/env python3
"""Regenerates the EBCDIC tables in src/Storage/Ebcdic.cpp from the standard
cp500 and cp037 codecs.  Development-time only; the generated file is committed."""
import sys
for cp in ("cp500", "cp037"):
    t = [ord(bytes([b]).decode(cp)) for b in range(256)]
    print(f"// {cp}")
    for row in range(16):
        print("    " + ", ".join(f"0x{v:04X}" for v in t[row*16:(row+1)*16]) + ",")
