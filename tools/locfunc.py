#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Locate T2Indisp.dll functions via debug-string xrefs + .pdata bounds."""
import subprocess, re, struct, sys

DLL = "Win10_IDD/x64/T2Indisp.dll"
BASE = 0x180000000
TEXT_VMA, TEXT_OFF = 0x180001000, 0x400
RDATA_VMA, RDATA_OFF = 0x18001A000, 0x18E00
PDATA_OFF = 0x23A00

img = open(DLL, "rb").read()

def rva_to_off(rva):
    v = BASE + rva if rva < 0x1000000 else rva
    if TEXT_VMA <= v < TEXT_VMA + 0x188C0:
        return v - TEXT_VMA + TEXT_OFF
    if RDATA_VMA <= v < RDATA_VMA + 0x9756:
        return v - RDATA_VMA + RDATA_OFF
    return None

# --- .pdata functions ---
pdata = img[PDATA_OFF:PDATA_OFF + 0x1290]
funcs = []
for i in range(0, len(pdata), 12):
    start, end, un = struct.unpack("<III", pdata[i:i+12])
    if start == 0:
        break
    funcs.append((start, end))
funcs.sort()
print(f"{len(funcs)} functions in .pdata")

def func_at(rva):
    for s, e in funcs:
        if s <= rva < e:
            return s, e
    return None

# --- find strings ---
targets = sys.argv[1:] or ["EncodeRleDTR"]
rdata = img[RDATA_OFF:RDATA_OFF + 0x9756]
dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn", DLL],
                     capture_output=True, text=True).stdout
open("t2indisp.asm", "w").write(dis)
print("disassembly saved")

for t in targets:
    needle = t.encode()
    idxs = []
    i = 0
    while True:
        i = rdata.find(needle, i)
        if i < 0:
            break
        idxs.append(i)
        i += 1
    print(f"--- {t}: {len(idxs)} string refs ---")
    for idx in idxs:
        strabs = RDATA_VMA + idx
        # objdump prints RIP-relative lea targets as absolute vma; search hex
        pat = f"{strabs:x}"
        hits = [l for l in dis.splitlines() if pat in l.replace(" ", "")]
        # fallback: search with 0x prefix variations
        if not hits:
            pat2 = hex(strabs)[2:]
            hits = [l for l in dis.splitlines() if pat2 in l]
        print(f"  str@{strabs:#x}: {len(hits)} xref-candidates")
        for h in hits[:6]:
            m = re.match(r"\s*([0-9a-f]+):", h)
            if m:
                fra = int(m.group(1), 16) - BASE
                f = func_at(fra)
                print(f"    {h.strip()[:100]}")
                if f:
                    print(f"      -> in func {f[0]+BASE:#x}..{f[1]+BASE:#x}")
