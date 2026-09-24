#!/usr/bin/env python3
"""Build xref map of T2Indisp.dll .text with capstone; locate functions by string refs."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_REG_RIP, X86_OP_MEM, X86_OP_IMM

DLL = "Win10_IDD/x64/T2Indisp.dll"
BASE = 0x180000000
TEXT_VMA, TEXT_OFF, TEXT_SIZE = 0x180001000, 0x400, 0x188C0
PDATA_OFF, PDATA_SIZE = 0x23A00, 0x1290

img = open(DLL, "rb").read()
text = img[TEXT_OFF:TEXT_OFF + TEXT_SIZE]

pdata = img[PDATA_OFF:PDATA_OFF + PDATA_SIZE]
funcs = []
for i in range(0, len(pdata), 12):
    s, e, _ = struct.unpack("<III", pdata[i:i+12])
    if s == 0:
        break
    funcs.append((BASE + s, BASE + e))
funcs.sort()

def func_at(addr):
    for s, e in funcs:
        if s <= addr < e:
            return s
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
xrefs = {}   # target_abs -> list of (instr_abs, mnemonic, op_str)
insn_count = 0
for insn in md.disasm(text, TEXT_VMA):
    insn_count += 1
    if insn.mnemonic in ("lea", "mov", "cmp", "test", "call", "jmp", "je", "jne",
                         "ja", "jb", "jae", "jbe", "jg", "jge", "jl", "jle", "jo",
                         "jno", "js", "jns", "jp", "jnp", "jb", "jbe"):
        for op in insn.operands:
            if op.type == X86_OP_MEM:
                if op.mem.base == X86_REG_RIP:  # RIP
                    tgt = insn.address + insn.size + op.mem.disp
                    xrefs.setdefault(tgt, []).append(
                        (insn.address, insn.mnemonic, insn.op_str))
            elif op.type == X86_OP_IMM and insn.mnemonic in ("call", "jmp"):  # IMM
                xrefs.setdefault(op.imm, []).append(
                    (insn.address, insn.mnemonic, insn.op_str))
print(f"disassembled {insn_count} insns, {len(xrefs)} xref targets")

rdata = img[0x18E00:0x18E00 + 0x9756]
# resolve .rdata pointer-table entries -> strings (one more indirection)
table = {}
for off in range(0, 0x9756 - 8, 8):
    q = struct.unpack("<Q", rdata[off:off + 8])[0]
    if 0x18001A000 <= q < 0x18001A000 + 0x9756:
        s = rdata[q - 0x18001A000:q - 0x18001A000 + 80].split(bytes([0]))[0]
        if s.startswith(b"t2umdf!"):
            table[0x18001A000 + off] = s.decode(errors="replace")
print(f"{len(table)} table entries -> t2umdf! strings")
if sys.argv[1:] and sys.argv[1] == "--rdata-refs":
    ks = sorted(t for t in xrefs if 0x18001A000 <= t < 0x18001A000 + 0x9756)
    print(f"{len(ks)} xref targets in .rdata")
    for k in ks[:25]:
        s = rdata[k - 0x18001A000:k - 0x18001A000 + 60].split(bytes([0]))[0]
        print(hex(k), s[:64])
    sys.exit(0)
for t in sys.argv[1:]:
    print(f"===== {t} =====")
    needle = t.encode()
    i, n = 0, 0
    # also match via pointer-table entries
    strabs_list = []
    while True:
        i = rdata.find(needle, i)
        if i < 0:
            break
        strabs_list.append(0x18001A000 + i)
        i += 1
    for te, ts in table.items():
        if t in ts:
            strabs_list.append(te)
    for strabs in strabs_list:
        refs = xrefs.get(strabs, [])
        fset = {}
        for (ia, mn, op) in refs:
            f = func_at(ia)
            fset.setdefault(f, []).append((ia, mn, op))
        print(f"  str@{strabs:#x}: {len(refs)} xrefs in {len(fset)} funcs")
        for f, lst in fset.items():
            fn = f"{f:#x}" if f else "NO FUNC"
            print(f"    func {fn}:")
            for (ia, mn, op) in lst[:4]:
                print(f"      {ia:#x}: {mn} {op}")
        i += 1
    if n == 0:
        print("  (no string found)")
