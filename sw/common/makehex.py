#!/usr/bin/env python3
"""bin -> $readmemh: one 32-bit little-endian word per line."""
import sys

data = open(sys.argv[1], "rb").read()
data += b"\x00" * (-len(data) % 4)
for i in range(0, len(data), 4):
    w = int.from_bytes(data[i:i+4], "little")
    print(f"{w:08x}")