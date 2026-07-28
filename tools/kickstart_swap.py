#!/usr/bin/env python3
"""Byte-swap a Kickstart ROM for in-place execution by the Amiga core.

The 68000 is big-endian and the H7 is little-endian; the core reads ROM words
straight out of memory-mapped flash, so the file must already be in 68000 byte
order. Swapping at runtime would need 256K of RAM the machine does not have.

    python3 tools/kickstart_swap.py <kickstart.rom> <roms/amiga/output.rom>
"""
import sys

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(1)

data = open(sys.argv[1], "rb").read()
size = len(data)
if size == 0 or (size & (size - 1)) or size > 512 * 1024:
    sys.exit(f"not a Kickstart: {size:,} bytes (needs a power of two <= 512K)")

out = bytearray(size)
out[0::2] = data[1::2]
out[1::2] = data[0::2]
open(sys.argv[2], "wb").write(out)
print(f"swapped {size:,} bytes -> {sys.argv[2]}")
