#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""wacom-t2d/init-full.bin (359 vendor transfers) -> wacom-t2d/init.bin (78, validated cold 2026-09-24).

Dropped (see docs/reference/t2-init-sequence.md, "Trimmed init"):
  - 0x05 register reads whose values are unused, 0x91 ID-ROM query
  - 0x90 + 32x1 probe fill at address 0 + 0x90
  - manual scanout programming in bank FC (b1..b4), done by our 0x15 flips
  - second copy of the fb70../fea8../fe70.. block
  - the two 1.8 MB black clears (the driver writes a black frame before 0x14)
Kept, although it looks like flip programming: the bank-FB buffer-address setup
(fb60/6e/65/62..64 for buffers A and B, records 75..90). Without it a garbage
band appears at the bottom of the screen.
Pacing is not stored in the blob. Consumers apply t2dev.pace_for()'s rule
(2 ms after reg writes, 50 ms after feb0/feb1, 20 ms after other EP3/EP4 commands).
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from t2dev import load_init

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
recs = load_init(os.path.join(ROOT, "wacom-t2d", "init-full.bin"))
assert len(recs) == 359

reads = [0, 2, 3, 6, 8, 21, 22, 55, 56, 66]
idrom = [1]
probe_fill = [32, 33, 34]
fc_flip_regs = [74, 91, 92, 93, 94, 95] + list(range(97, 113))
dup_block = list(range(113, 130))
clears = list(range(130, 358))
drop = set(reads + idrom + probe_fill + fc_flip_regs + dup_block + clears)
keep = [i for i in range(len(recs)) if i not in drop]

out = bytearray()
for i in keep:
    ep, fl, d, ln, rd = recs[i]
    out += struct.pack("<BBI", ep, fl, ln)
    if not fl & 1:
        out += d
    if fl & 2:
        out += struct.pack("<I", rd)
open(os.path.join(ROOT, "wacom-t2d", "init.bin"), "wb").write(out)
print(f"{len(keep)} records, {len(out)} bytes -> wacom-t2d/init.bin; kept: {keep}")
