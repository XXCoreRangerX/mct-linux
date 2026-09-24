#!/usr/bin/env python3
"""Extract the Windows init (redir2.bin) up to and incl. the 0x14 display-enable
into t2d/init.bin for the daemon. Record format (little-endian):
  u8 ep | u8 flags | u32 len | data[len if not ZERO]
  flags: 1 = ZERO (send `len` zero bytes, no data stored)
         2 = READ (after this OUT, read `rdlen` from EP 0x81; rdlen = u32 after data)
Each record is exactly one USB transfer (firmware is transfer-framed)."""
import struct, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay import load

ops, in_lens = load()
it = iter(in_lens)
out = bytearray(); n = 0
for ep, data in ops:
    flags = 0; extra = b""
    if ep == 2 and len(data) > 64 and not any(data):
        flags |= 1
    if ep == 3 and data[0] in (0x05, 0x91, 0x1D):
        flags |= 2; extra = struct.pack("<I", max(next(it), 1))
    out += struct.pack("<BBI", ep, flags, len(data))
    if not flags & 1:
        out += data
    out += extra; n += 1
    if ep == 3 and data[0] == 0x14:
        break
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "t2d", "init.bin"), "wb").write(out)
print(f"{n} records, {len(out)} bytes -> t2d/init.bin")
