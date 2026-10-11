#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""I2C through the T2's two I2C masters (OpenCores-style, see docs/reference/protocol-t2.md).

bus 0: data fce3, command/status fce4    bus 1: data fceb, command/status fcec
command bits: 0x80 START, 0x40 STOP, 0x20 READ, 0x10 WRITE, 0x08 NACK
status bits:  0x80 no ACK received, 0x02 transfer in progress

Usage:
  t2i2c.py scan [bus]              list responding 7-bit addresses
  t2i2c.py read BUS ADDR OFF LEN   register read (8-bit offset)
  t2i2c.py vcp BUS VCP             DDC/CI get VCP feature (0x37)
  t2i2c.py setvcp BUS VCP VALUE    DDC/CI set VCP feature
"""
import sys, time
import t2dev

REGS = {0: (0xfce3, 0xfce4), 1: (0xfceb, 0xfcec)}
START, STOP, READ, WRITE, NACK = 0x80, 0x40, 0x20, 0x10, 0x08


class I2C:
    def __init__(self, t, bus):
        self.t = t
        self.data, self.cr = REGS[bus]

    def cmd(self, c, data=None):
        if data is not None:
            self.t.wreg(self.data, data)
        self.t.wreg(self.cr, c)
        for _ in range(200):
            st = self.t.reg(self.cr)
            if not st & 0x02:
                return st
            time.sleep(0.001)
        raise TimeoutError(f"i2c timeout, status {st:#x}")

    def start(self, addr, rd=False):
        return not self.cmd(START | WRITE, (addr << 1) | rd) & 0x80

    def stop(self):
        self.t.wreg(self.cr, STOP)

    def write_byte(self, b):
        return not self.cmd(WRITE, b) & 0x80

    def read_byte(self, last=False):
        self.cmd(READ | (NACK | STOP if last else 0))
        return self.t.reg(self.data)

    def probe(self, addr):
        ok = self.start(addr)
        self.stop()
        return ok

    def write(self, addr, data):
        if not self.start(addr):
            self.stop()
            return False
        for b in data:
            if not self.write_byte(b):
                self.stop()
                return False
        self.stop()
        return True

    def read(self, addr, n, reg=None):
        if reg is not None:
            if not self.start(addr) or not self.write_byte(reg):
                self.stop()
                return None
        if not self.start(addr, rd=True):
            self.stop()
            return None
        return bytes(self.read_byte(last=(i == n - 1)) for i in range(n))


def ddc_get_vcp(i2c, vcp):
    msg = [0x51, 0x82, 0x01, vcp]
    chk = 0x6e
    for b in msg:
        chk ^= b
    if not i2c.write(0x37, msg + [chk]):
        return None
    time.sleep(0.05)
    r = i2c.read(0x37, 11)
    if not r or r[2] != 0x02:
        return r
    return {"result": r[3], "vcp": r[4], "max": r[6] << 8 | r[7], "cur": r[8] << 8 | r[9], "raw": r.hex(" ")}


def ddc_set_vcp(i2c, vcp, value):
    msg = [0x51, 0x84, 0x03, vcp, value >> 8, value & 0xff]
    chk = 0x6e
    for b in msg:
        chk ^= b
    return i2c.write(0x37, msg + [chk])


if __name__ == "__main__":
    t = t2dev.T2()
    a = sys.argv[1:]
    try:
        if a[0] == "scan":
            for bus in ([int(a[1])] if len(a) > 1 else [0, 1]):
                i2c = I2C(t, bus)
                found = [f"{addr:#04x}" for addr in range(0x08, 0x78) if i2c.probe(addr)]
                print(f"bus {bus}: {' '.join(found) or 'nothing'}")
        elif a[0] == "read":
            bus, addr, off, n = int(a[1]), int(a[2], 0), int(a[3], 0), int(a[4], 0)
            r = I2C(t, bus).read(addr, n, off)
            print(r.hex(" ") if r else "no ack")
        elif a[0] == "vcp":
            print(ddc_get_vcp(I2C(t, int(a[1])), int(a[2], 0)))
        elif a[0] == "setvcp":
            print(ddc_set_vcp(I2C(t, int(a[1])), int(a[2], 0), int(a[3], 0)))
    finally:
        t.close()
