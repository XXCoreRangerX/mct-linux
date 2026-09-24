#!/usr/bin/env python3
"""Map vftables -> class names -> methods for T2Indisp.dll via RTTI scan."""
import struct, re

DLL = "Win10_IDD/x64/T2Indisp.dll"
img = open(DLL, "rb").read()
TEXT0, TEXT1 = 0x180001000, 0x180001000 + 0x188C0
RDATA0, RDATA1 = 0x18001A000, 0x18001A000 + 0x9756
rdata = img[0x18E00:0x18E00 + 0x9756]

def qword_at_rva(v):
    return struct.unpack("<Q", rdata[v - RDATA0:v - RDATA0 + 8])[0]

def cstr_at_rva(v):
    end = rdata.find(b"\x00", v - RDATA0)
    return rdata[v - RDATA0:end].decode(errors="replace")

def demangle(td_name):
    m = re.match(r"\.?AV(.+?)@@", td_name)
    return m.group(1) if m else td_name

# find runs of >=3 consecutive .text pointers in .rdata
tables = []
i = 0
while i < 0x9756 - 8:
    v = RDATA0 + i
    q = qword_at_rva(v)
    if TEXT0 <= q < TEXT1:
        j = i
        methods = []
        while j < 0x9756 - 8:
            q2 = struct.unpack("<Q", rdata[j:j + 8])[0]
            if not (TEXT0 <= q2 < TEXT1):
                break
            methods.append(q2)
            j += 8
        if len(methods) >= 3:
            tables.append((v, methods))
        i = j
    else:
        i += 8

print(f"{len(tables)} vftable candidates")
for v, methods in tables:
    # COL at v-8
    cls = "?"
    if v - 8 >= RDATA0:
        col = qword_at_rva(v - 8)
        if RDATA0 <= col < RDATA1:
            # x64 MSVC COL uses 32-bit RVAs
            td_rva = struct.unpack("<I", rdata[col - RDATA0 + 12:col - RDATA0 + 16])[0]
            td = 0x180000000 + td_rva
            if RDATA0 <= td < RDATA1:
                try:
                    cls = demangle(cstr_at_rva(td + 16))
                except Exception:
                    pass
    print(f"{cls} @ {v:#x} ({len(methods)} methods)")
    for m in methods:
        print(f"    {m:#x}")
