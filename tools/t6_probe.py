#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
"""Probe two T6 features that were never tried on the 5600 (j5create JUA254,
HDMI output 0): HDMI audio and DDC/CI through the adapter's I2C requests.
Each step asks y/n first; every request, reply and error goes to a log file
with timestamps. Summary at the end.

  A  read-only: function descriptors (b1 index 0 and 3) decoded, the audio
     engine reads in the order the Windows driver sends them (a1 engine 0,
     a4, a1 engine 1, a5, a2, a3 per node).
  B  read-only I2C: EDID block 0 through the I2C read request (0x81) at
     address 0x50, compared byte for byte with the EDID request (0x80).
     PASS = the I2C path reaches the monitor's DDC bus.
  C  DDC/CI, LG 27GN750 only: "Get VCP Feature" brightness (VCP 0x10) at
     address 0x37 through the I2C write (0x02) and read (0x81) requests,
     prints current / maximum. Needs B's PASS (selecting C runs B first).
     Reads only: no Set VCP in this version.
  D  HDMI audio, risk medium: sets 1920x1080@60 from the adapter's own mode
     table with a grey picture (optional), enables the audio engine as MCT
     does, plays a quiet 440 Hz tone in signature-3 bulk sessions framed
     like the Windows driver, disables the engine, asks whether you heard
     it. The LG 27GN750 has no speakers: listen on its headphone jack.

After every step a harmless read (0x87 status, b0 platform) checks that the
T6 still answers.

Risks:
  - Unknown requests can stall (harmless EPIPE on the 5600 so far).
  - The I2C request layout (wValue / wIndex / data) is not documented
    anywhere; B tries the 7-bit then the 8-bit address form, reads only.
    The bus may be the board's own I2C bus (HDMI transmitter), not the
    monitor's DDC bus, in which case B fails and C is refused.
  - C writes a DDC/CI request to the monitor. The Gigabyte MO34WQC2 hangs
    its controller on DDC experiments: the step refuses to run unless the
    EDID says LG "27GN7" and you confirm it.
  - D: a malformed audio session can wedge the bulk parser. The only known
    recovery is the cancel request (40 14), which restarts the 5600 (it
    leaves the bus, ~3 s without picture, then re-enumerates). The tool
    explains it and offers it if the device stops answering. Never a USB
    reset (on the T2 a reset made the chip drop off the bus).
  - Never sent: 0x01 (SPI ROM write), 0x13 (write ROM data), 0x30 (hardware
    reset). Every request goes through an allowlist.

Run (driver unbound, root, nothing else talking to the T6):
  tools/t6_run.sh tools/t6_probe.py                 all steps, y/n before each
  tools/t6_run.sh tools/t6_probe.py --steps AB      read-only steps only
  tools/t6_probe.py --dry-run [--steps ...]         no USB: print every request
                                                    with canned replies
  tools/t6_probe.py --self-test                     check the builders/decoders
Log: --log PATH (default /tmp/t6_probe-<time>.log).
"""
import argparse, math, os, statistics, struct, sys, threading, time

OUTPUT = 0
EP_OUT = 0x02
FRAG = 0x19000
TIMEOUT_MS = 2000

# requests (MCT t6.h)
REQ_I2C_WRITE, REQ_OUTPUT, REQ_SETMODE, REQ_CANCEL, REQ_AUD_SET_STATE, REQ_READY = \
    0x02, 0x03, 0x12, 0x14, 0x24, 0x31
REQ_EDID, REQ_I2C_READ, REQ_STATUS, REQ_MODES = 0x80, 0x81, 0x87, 0x89
REQ_AUD_ENGINE_CAPS, REQ_AUD_NODE_CAPS, REQ_AUD_NODE_VALUE, REQ_AUD_GET_STATE, REQ_AUD_FORMATS = \
    0xA1, 0xA2, 0xA3, 0xA4, 0xA5
REQ_VERSION, REQ_FUNC_ID, REQ_SECTION = 0xB0, 0xB1, 0xB3

FORBIDDEN = {0x01: "SPI ROM write", 0x13: "write ROM data", 0x30: "hardware reset"}
ALLOWED_IN = {REQ_EDID, REQ_I2C_READ, REQ_STATUS, REQ_MODES, REQ_AUD_ENGINE_CAPS,
              REQ_AUD_NODE_CAPS, REQ_AUD_NODE_VALUE, REQ_AUD_GET_STATE, REQ_AUD_FORMATS,
              REQ_VERSION, REQ_FUNC_ID, REQ_SECTION}
ALLOWED_OUT = {REQ_I2C_WRITE, REQ_OUTPUT, REQ_SETMODE, REQ_AUD_SET_STATE, REQ_READY, REQ_CANCEL}

DDC_EDID, DDC_CI = 0x50, 0x37            # 7-bit I2C addresses
VCP_BRIGHTNESS = 0x10


# ---------------------------------------------------------------- builders

def setup_packet(bm, req, value, index, length):
    """The 8-byte USB setup packet, checked against the allowlist."""
    if req in FORBIDDEN:
        raise ValueError(f"request {req:#04x} ({FORBIDDEN[req]}) is never sent")
    if bm == 0xC0 and req not in ALLOWED_IN or bm == 0x40 and req not in ALLOWED_OUT \
            or bm not in (0xC0, 0x40):
        raise ValueError(f"request {bm:02x} {req:02x} is not on this tool's allowlist")
    if not (0 <= value <= 0xFFFF and 0 <= index <= 0xFFFF and 0 <= length <= 0xFFFF):
        raise ValueError("wValue / wIndex / wLength out of range")
    return struct.pack("<BBHHH", bm, req, value, index, length)


def audio_session_header(n, restart=False):
    """Bulk session header for one audio packet (MCT t6_libusb_SendAudio):
    signature 3, payload length = packet length = n, address 0. restart sets
    the function-specific audio Flags byte (offset 24) to 0x01, "restart
    engine" in MCT's t6bulkdef.h: for playback the engine state "was changed
    by bulk cmd" (t6auddef.h), and on the 5600 the engine stayed stopped
    with the flag at 0 as in the 5601 capture (2026-10-12)."""
    return struct.pack("<6I2I", 3, n, 0, n, 0, 0, 1 if restart else 0, 0)


def set_engine_state(activity, fmt_index, cyclic, return_size=9600):
    """T6AUD_SETENGINESTATE (16 bytes): Activity, Rsvd, FormatIndex, Rsvd2,
    ReturnSize, CyclicBufferSize."""
    return struct.pack("<BBHIII", activity, 0, fmt_index, 0, return_size, cyclic)


def ddc_checksum(data, init):
    c = init
    for b in data:
        c ^= b
    return c


def ddc_get_vcp(vcp):
    """6E 51 82 01 vcp checksum (XOR of all bytes from 0x6E)."""
    msg = bytes((0x6E, 0x51, 0x82, 0x01, vcp))
    return msg + bytes((ddc_checksum(msg, 0),))


def ddc_parse_vcp_reply(buf, vcp):
    """11 bytes: 6E 88 02 RC VCP TYPE MH ML SH SL CHK, checksum = 0x50 XOR
    bytes 0..9 (the reply's virtual destination address is 0x50)."""
    if len(buf) < 11:
        raise ValueError(f"reply too short ({len(buf)} bytes)")
    b = buf[:11]
    want = ddc_checksum(b[:10], 0x50)
    r = {"checksum_ok": b[10] == want, "checksum": b[10], "expected": want,
         "source": b[0], "length": b[1] & 0x7F, "opcode": b[2], "result": b[3],
         "vcp": b[4], "type": b[5], "max": (b[6] << 8) | b[7], "cur": (b[8] << 8) | b[9]}
    r["ok"] = (r["checksum_ok"] and b[0] == 0x6E and b[1] == 0x88 and b[2] == 0x02
               and b[3] == 0 and b[4] == vcp)
    return r


def tone(rate, seconds, freq=440.0, amplitude=0.03, fade=0.02):
    """S16LE stereo, the same sample on both channels, faded in and out."""
    n = int(rate * seconds)
    nf = max(1, int(rate * fade))
    out = bytearray()
    for i in range(n):
        g = min(1.0, i / nf, (n - 1 - i) / nf)
        s = int(round(32767 * amplitude * g * math.sin(2 * math.pi * freq * i / rate)))
        out += struct.pack("<hh", s, s)
    return bytes(out)


def pll(khz, base=40000):
    """MCT's PLL formula (docs/reference/protocol-t6.md), for the dry run's
    canned mode table only; live runs use the device's own entry."""
    for i, x in enumerate((4, 8, 16, 32)):
        if khz * x >= 800000:
            vco = khz * x
            n = vco // base
            return (vco - n * base) * 1000 // base, 1000, n, (3, 1, 2, 0)[i]
    raise ValueError(khz)


MODE_FMT = "<IBB8HHHBBBBBB"
MODE_FIELDS = ("khz hz rsvd htotal hdisplay hsync_start hsync_len vtotal vdisplay "
               "vsync_start vsync_len p q n osel hpol vpol reduced flag").split()


def mode_entry(khz, hz, ht, w, hs, hsl, vt, h, vs, vsl, hpol=1, vpol=1):
    p, q, n, osel = pll(khz)
    return struct.pack(MODE_FMT, khz, hz, 0, ht, w, hs, hsl, vt, h, vs, vsl, p, q, n, osel,
                       hpol, vpol, 0, 0)


def grey_nv12_frame(w, h, fb=0x0, seq=1):
    """Session-0 pieces of one raw NV12 grey frame (t6_cursor_test.py's
    recipe): list of (session header, fragment)."""
    payload = bytes([110]) * (w * h) + bytes([128]) * (w * h // 2)
    p0 = fb + 0x30
    hdr = struct.pack("<4I2H3I I3I", 3, len(payload), seq, 6, w, w, p0, p0 + w * h, p0, 6, 0, 0, 0)
    data = hdr + payload
    out = []
    for off in range(0, len(data), FRAG):
        frag = data[off:off + FRAG]
        more = 1 if off + FRAG < len(data) else 0
        out.append((struct.pack("<8I", 0, len(data), fb, len(frag), off, more, 0, 0), frag))
    return out


# ---------------------------------------------------------------- decoders

def utf16z(b):
    s = b.decode("utf-16-le", "replace")
    return s.split("\0", 1)[0]


def decode_mode(b):
    m = dict(zip(MODE_FIELDS, struct.unpack(MODE_FMT, b[:32])))
    return (f"{m['hdisplay']}x{m['vdisplay']}@{m['hz']} {m['khz']} kHz htotal {m['htotal']} "
            f"vtotal {m['vtotal']} PLL P/Q/N/sel {m['p']}/{m['q']}/{m['n']}/{m['osel']} "
            f"pol {m['hpol']}/{m['vpol']} flag {m['flag']}")


def decode_aud_format(b):
    f = struct.unpack("<IHBBIIIIIB3s", b[:32])
    return {"rate": f[0], "fs": f[1], "bits": f[2], "channels": f[3], "pll_hz": f[4],
            "mclk": f[5], "bclk": f[6], "pll1": f[7], "pll2": f[8], "engine_mask": f[9]}


def fmt_aud_format(d):
    return (f"{d['rate']} Hz, {d['bits']} bit, {d['channels']} ch, fs {d['fs']}, PLL {d['pll_hz']} Hz, "
            f"MCLK {d['mclk']}, BCLK {d['bclk']}, PLL settings {d['pll1']:#010x} {d['pll2']:#010x}, "
            f"engine mask {d['engine_mask']:#04x}")


def decode_func_desc(b, index):
    """b1 reply (132 bytes): u16 VID, u16 function id, UTF-16 name[32]; then
    for the display function (index 0) the DISP section header from its
    +0x50 (MCT t6.h T6ROMDISPLAYSECTIONHDR, shifted by its 12-byte tag,
    size and reserved field) and a 32-byte timing entry at +0x64; for the
    audio function (index 3) 32 unknown bytes and a T6AUD_FORMAT at +0x64."""
    lines = []
    if len(b) < 0x44:
        return [f"short reply ({len(b)} bytes)"]
    vid, fid = struct.unpack_from("<HH", b)
    lines.append(f"VID {vid:#06x}, function {fid:#06x}, name \"{utf16z(b[4:0x44])}\"")
    if len(b) < 0x84:
        return lines + [f"(only {len(b)} bytes, rest not decoded)"]
    if index == 0:
        ver, func, c1, c2, intf = struct.unpack_from("<5I", b, 0x44)
        lines.append(f"version {ver}, display function flags {func:#x} (bit 0 rotate?, bit 1 reset?)")
        for i, c in enumerate((c1, c2)):
            lines.append(f"display {i + 1} caps {c:#010x}: link interfaces {c & 0xF:#x}, "
                         f"{(c >> 8) & 0xFF} timing tables at offset {c >> 16:#x}")
        lines.append(f"display interface {intf:#010x}: DAC I2C {intf & 3}, DVO I2C {(intf >> 8) & 3}, "
                     f"DVO transmitter {(intf >> 12) & 0xF}, DVI I2C {(intf >> 16) & 3}, "
                     f"LVDS I2C {(intf >> 24) & 3}")
        lines.append(f"+0x58 reserved {b[0x58:0x64].hex()}")
        lines.append(f"+0x64 timing entry: {decode_mode(b[0x64:0x84])}")
    else:
        words = struct.unpack_from("<8I", b, 0x44)
        lines.append("+0x44 unknown: " + " ".join(f"{w:08x}" for w in words)
                     + f" (bytes {b[0x44:0x64].hex()})")
        lines.append(f"+0x64 format: {fmt_aud_format(decode_aud_format(b[0x64:0x84]))}")
    return lines


def decode_engine_caps(b):
    t, fl, nn, _, nf, pref, bw, mbuf = struct.unpack("<BBBBHHII", b[:16])
    return {"type": t, "flags": fl, "nodes": nn, "formats": nf, "preferred": pref,
            "max_bandwidth": bw, "max_buffer": mbuf}


def fmt_engine_caps(d):
    kind = {0: "render", 1: "capture"}.get(d["type"], "?")
    return (f"type {d['type']} ({kind}), flags {d['flags']:#x} (bit 0 jack state), {d['nodes']} nodes, "
            f"{d['formats']} formats, preferred {d['preferred']}, max bandwidth {d['max_bandwidth']} B/s, "
            f"max buffer {d['max_buffer']} B")


def decode_engine_state(b):
    act, jack, fmt, hw, fw, cyc = struct.unpack("<BBHIII", b[:16])
    return {"activity": act, "jack": jack, "format": fmt, "hwpos": hw, "fwpos": fw, "cyclic": cyc}


def fmt_engine_state(d):
    j = d["jack"]
    return (f"activity {d['activity']} ({'run' if d['activity'] else 'stop'}), jack {j:#04x} "
            f"(speaker {j & 1}, DVO HDMI {(j >> 1) & 1}, LVDS HDMI {(j >> 2) & 1}), format {d['format']}, "
            f"HW pos {d['hwpos']}, FW pos {d['fwpos']}, cyclic buffer {d['cyclic']} B")


NODE_TYPES = {0: "volume", 1: "mute", 2: "mux", 3: "volume dB"}


def fmt_node_caps(b):
    t, step, lo, hi = struct.unpack("<B3xIii", b[:16])
    return f"type {t} ({NODE_TYPES.get(t, '?')}), step {step}, range {lo}..{hi}"


def fmt_node_value(b):
    mask = b[0]
    vals = struct.unpack_from("<8i", b, 8)
    return f"channel mask {mask:#04x}, values " + " ".join(str(v) for v in vals)


def edid_info(b):
    """(manufacturer, product code, monitor name, checksum ok) of block 0."""
    if len(b) < 128 or b[:8] != bytes.fromhex("00ffffffffffff00"):
        return None
    m = (b[8] << 8) | b[9]
    mfg = "".join(chr(((m >> s) & 0x1F) + 64) for s in (10, 5, 0))
    name = ""
    for o in (54, 72, 90, 108):
        if b[o:o + 3] == b"\0\0\0" and b[o + 3] == 0xFC:
            name = b[o + 5:o + 18].split(b"\n")[0].decode("ascii", "replace").strip()
    return mfg, b[10] | (b[11] << 8), name, sum(b[:128]) % 256 == 0


# ---------------------------------------------------------------- canned replies (dry run)

def synthetic_edid():
    """Block 0 that edid_info() reads as GSM / 27GN7 (not the real LG EDID)."""
    b = bytearray(128)
    b[0:8] = bytes.fromhex("00ffffffffffff00")
    m = ((ord("G") - 64) << 10) | ((ord("S") - 64) << 5) | (ord("M") - 64)
    b[8], b[9] = m >> 8, m & 0xFF
    b[10:12] = (0x5B9F).to_bytes(2, "little")
    b[18], b[19] = 1, 3
    b[72:77] = bytes((0, 0, 0, 0xFC, 0))
    b[77:90] = b"27GN7\n       "
    b[127] = (-sum(b[:127])) % 256
    return bytes(b)


def canned_vcp_reply(cur=50, mx=100):
    b = bytes((0x6E, 0x88, 0x02, 0x00, VCP_BRIGHTNESS, 0x00, mx >> 8, mx & 0xFF, cur >> 8, cur & 0xFF))
    return b + bytes((ddc_checksum(b, 0x50),))


# Replies of a 0711:5601 in the Windows capture (third_party/trigger6
# mctt6.pcapng, captures/t6-analysis/ctrl.txt and ctrlfull.txt).
CAP_B1_0 = bytes.fromhex(
    "110721565400720069006700670065007200200036002000450078007400650072006e0061006c0020004700"
    "7200610070006800690063007300000000000000000000000100000003000000081e7000011e30040000000000"
    "0000000000000000000000576200003c0020038002900260000d02e001ea0102008c00e803140000000000")
CAP_B1_3 = bytes.fromhex(
    "11074056550053004200200041007500640069006f0020004400650076006900630065000000000000000000"
    "0000000000000000000000000000000000000000000000000101700000000000104000001100000000000000"
    "00000000000000000000000080bb000080001002000050460000000000701700e94c0100e801e80300000000")
CAP_A1_0 = bytes.fromhex("00000100010000000000000000000200")
CAP_A4 = bytes.fromhex("00010000000000000000000000000200")
CAP_A5 = bytes.fromhex("80bb000080001002000050460000000000701700e94c0100e801e80300000000")
CAP_A2 = bytes.fromhex("0000000001000000000000001f000000")
CAP_A3 = bytes.fromhex("03000000000000001f0000001f0000000f0000000f0000000f0000000f0000000f0000000f000000")


def canned(bm, req, value, index, length):
    if req == REQ_FUNC_ID:
        return (CAP_B1_0 if index == 0 else CAP_B1_3)[:length]
    if req == REQ_AUD_ENGINE_CAPS:
        return CAP_A1_0 if index == 0 else b""
    if req == REQ_AUD_GET_STATE:
        return CAP_A4
    if req == REQ_AUD_FORMATS:
        return (CAP_A5 * 8)[:length]
    if req == REQ_AUD_NODE_CAPS:
        return CAP_A2
    if req == REQ_AUD_NODE_VALUE:
        return CAP_A3
    if req == REQ_STATUS:
        return b"\x01"
    if req == REQ_VERSION:
        return {0: b"\x01\0\0\0", 3: b"JUA254H".ljust(16, b"\0")}.get(index, bytes(length))[:length]
    if req == REQ_SECTION:
        return (b"DISP" + struct.pack("<II", 0x7F0, 0) + CAP_B1_0[:100])[:length]
    if req == REQ_EDID:
        return synthetic_edid()[:length]
    if req == REQ_MODES:
        table = mode_entry(148500, 60, 2200, 1920, 2008, 44, 1125, 1080, 1084, 5).ljust(960, b"\0")
        return table[index:index + length]
    if req == REQ_I2C_READ:
        if value in (DDC_EDID, DDC_EDID << 1):
            return synthetic_edid()[index:index + length]
        return canned_vcp_reply()[:length]
    return bytes(length)


# ---------------------------------------------------------------- device access with logging

class Probe:
    def __init__(self, args):
        self.args = args
        self.dry = args.dry_run
        self.t = None
        self.dev = None
        self.err = None
        self.gone = False
        self.logf = open(args.log, "a", encoding="utf-8")
        self.log(f"t6_probe {'dry run' if self.dry else 'live'}, steps {args.steps}, argv {sys.argv[1:]}")

    def log(self, msg, console=True):
        ts = time.strftime("%Y-%m-%d %H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
        self.logf.write(f"{ts} {msg}\n")
        self.logf.flush()
        if console:
            print(msg, flush=True)

    def open(self):
        if self.dry:
            self.log("DRY RUN: no USB access. Replies are canned: the Windows capture of a 5601 "
                     "for b1/a1-a5, a synthetic GSM \"27GN7\" EDID, a synthetic DDC/CI reply.")
            return
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import t6dev
        self.t = t6dev.T6(wait=10.0, output=OUTPUT)
        self.dev = self.t.dev
        self.log(f"opened {self.dev.idVendor:04x}:{self.dev.idProduct:04x} bus {self.dev.bus} "
                 f"address {self.dev.address}, interface 0 claimed")

    def close(self):
        if self.t is not None and not self.gone:
            try:
                self.t.close()
            except Exception as e:  # noqa: BLE001
                self.log(f"release failed: {e}")
        self.logf.close()

    @staticmethod
    def _hex(b, limit=64):
        h = b.hex()
        return h if len(b) <= limit else h[:limit * 2] + f"... ({len(b)} B, full in log)"

    def cin(self, req, value, index, length, what=""):
        setup = setup_packet(0xC0, req, value, index, length)
        head = (f"CTRL IN  c0 {req:02x} wValue={value:#06x} wIndex={index:#06x} wLength={length}"
                f" setup={setup.hex()}{'  ' + what if what else ''}")
        self.err = None
        if self.dry:
            r = canned(0xC0, req, value, index, length)
            self.log(f"{head} -> [canned] {len(r)} B: {r.hex()}", console=False)
            print(f"{head} -> [canned] {len(r)} B: {self._hex(r)}", flush=True)
            return r
        try:
            r = bytes(self.dev.ctrl_transfer(0xC0, req, value, index, length, TIMEOUT_MS))
        except Exception as e:  # noqa: BLE001  (pyusb USBError / USBTimeoutError)
            self.err = e
            self.log(f"{head} -> ERROR {e}")
            return None
        self.log(f"{head} -> {len(r)} B: {r.hex()}", console=False)
        print(f"{head} -> {len(r)} B: {self._hex(r)}", flush=True)
        return r

    def cout(self, req, value, index, data=b"", what=""):
        setup = setup_packet(0x40, req, value, index, len(data))
        head = (f"CTRL OUT 40 {req:02x} wValue={value:#06x} wIndex={index:#06x} wLength={len(data)}"
                f" setup={setup.hex()} data={data.hex() or '-'}{'  ' + what if what else ''}")
        self.err = None
        if self.dry:
            self.log(f"{head} -> [dry run, not sent]")
            return True
        try:
            n = self.dev.ctrl_transfer(0x40, req, value, index, data, TIMEOUT_MS)
        except Exception as e:  # noqa: BLE001
            self.err = e
            self.log(f"{head} -> ERROR {e}")
            return False
        self.log(f"{head} -> ok ({n})")
        return True

    def bulk(self, data, what="", quiet=False):
        """One bulk OUT transfer on EP 0x02. quiet: log file only."""
        head = f"BULK OUT ep 02 {len(data)} B {what}: {data[:32].hex()}{'...' if len(data) > 32 else ''}"
        self.err = None
        if self.dry:
            self.log(head + " -> [dry run, not sent]", console=not quiet)
            return True
        try:
            self.dev.write(EP_OUT, data, TIMEOUT_MS)
        except Exception as e:  # noqa: BLE001
            self.err = e
            self.log(f"{head} -> ERROR {e}")
            return False
        self.log(head + " -> ok", console=not quiet)
        return True

    def ask(self, q):
        if self.dry:
            self.log(f"  {q} -> [dry run: y]")
            return True
        while True:
            try:
                a = input(f"  {q} y/n + Enter: ").strip().lower()
            except EOFError:
                a = "n"
            if a in ("y", "n"):
                self.log(f"  {q} -> {a}", console=False)
                return a == "y"

    def wait(self, s):
        if not self.dry:
            time.sleep(s)

    # -------------------------------------------------------- shared checks

    def alive(self):
        """Harmless reads: monitor status (87) and the platform field (b0 0)."""
        s = self.cin(REQ_STATUS, OUTPUT, 0, 1, "health check: monitor status")
        p = self.cin(REQ_VERSION, 0, 0, 4, "health check: platform")
        ok = s is not None and len(s) == 1 and p is not None and len(p) == 4
        self.log(f"health check: {'T6 answers' if ok else 'T6 does NOT answer'}")
        return ok

    def recovery(self):
        self.log("""
The T6 does not answer harmless reads any more. Most likely the bulk
parser is waiting for bytes of a broken session.
Recovery: the cancel request 40 14 (wValue 0, wIndex 0, no data). On the
5600 it restarts the chip: it leaves the bus and re-enumerates about 3 s
later, no picture meanwhile. mct sends the same request itself after a
failed transfer, so rebinding the driver (t6_run.sh does that on exit) may
also recover it. If the adapter does not come back, unplug and replug it.
Do NOT use a USB reset (usbreset, port reset, authorized=0/1): on the T2 a
reset made the chip drop off the bus until a power cycle.""")
        if self.ask("Send the cancel request 40 14 now (the T6 restarts, ~3 s)?"):
            self.cout(REQ_CANCEL, 0, 0, b"", "cancel: restarts the 5600")
            self.gone = True
            self.log("sent; the device leaves the bus now. Stopping here, nothing else is sent.")
            return True
        return False

    def edid0(self):
        return self.cin(REQ_EDID, 0, OUTPUT, 128, "EDID block 0, output 0")


# ---------------------------------------------------------------- steps

def step_a(p, st):
    """Function descriptors and audio engine reads (read-only)."""
    proj = p.cin(REQ_VERSION, 0, 3, 16, "project code (board)")
    if proj:
        board = proj.split(b"\0", 1)[0].decode("ascii", "replace")
        p.log(f"  board: {board}")
    for idx in (0, 3):
        r = p.cin(REQ_FUNC_ID, 0, idx, 132, f"function descriptor {idx}")
        if r:
            for line in decode_func_desc(r, idx):
                p.log(f"  b1[{idx}] {line}")
    r = p.cin(REQ_SECTION, 0, 0, 112, "DISP section header")
    if r and len(r) >= 0x64:
        intf = struct.unpack_from("<I", r, 0x60)[0]
        st["dvo_i2c"] = (intf >> 8) & 3
        p.log(f"  b3 tag {r[:4]!r}, valid size {struct.unpack_from('<I', r, 4)[0]:#x}, "
              f"display function {struct.unpack_from('<I', r, 0x54)[0]:#x}, "
              f"display interface {intf:#010x} (DVO I2C bus {st['dvo_i2c']}, transmitter {(intf >> 12) & 0xF})")

    # the Windows driver's order (capture pkts 108-119): a1 e0, a4, a1 e1, a5, a2, a3
    caps = None
    r = p.cin(REQ_AUD_ENGINE_CAPS, 0, 0, 16, "audio engine 0 caps")
    if r and len(r) >= 16:
        caps = decode_engine_caps(r)
        p.log(f"  engine 0: {fmt_engine_caps(caps)}")
    r = p.cin(REQ_AUD_GET_STATE, 0, 0, 16, "audio engine 0 state")
    if r and len(r) >= 16:
        es = decode_engine_state(r)
        st["engine_state"] = es
        p.log(f"  engine 0 state: {fmt_engine_state(es)}")
    r = p.cin(REQ_AUD_ENGINE_CAPS, 0, 1, 16, "audio engine 1 caps (capture engine; 5601: 0 bytes)")
    if r and len(r) >= 16:
        p.log(f"  engine 1: {fmt_engine_caps(decode_engine_caps(r))}")
    nf = max(1, min(8, caps["formats"])) if caps else 1
    r = p.cin(REQ_AUD_FORMATS, 0, 0, 32 * nf, f"audio format list, {nf} x 32 B")
    fmts = []
    if r:
        for i in range(len(r) // 32):
            fmts.append(decode_aud_format(r[i * 32:(i + 1) * 32]))
            p.log(f"  format {i}: {fmt_aud_format(fmts[-1])}")
    st["formats"] = fmts
    for node in range(max(1, min(4, caps["nodes"])) if caps else 1):
        r = p.cin(REQ_AUD_NODE_CAPS, node, 0, 16, f"node {node} caps")
        if r and len(r) >= 16:
            p.log(f"  node {node}: {fmt_node_caps(r)}")
        r = p.cin(REQ_AUD_NODE_VALUE, node, 0, 40, f"node {node} value")
        if r and len(r) >= 40:
            p.log(f"  node {node} value: {fmt_node_value(r)}")
    st["audio_caps"] = caps
    return "INFO" if caps else "FAIL (no engine caps)"


def i2c_variants(st):
    v = [("7-bit address in wValue", lambda a: a), ("8-bit address in wValue", lambda a: a << 1)]
    bus = st.get("dvo_i2c", 0)
    if bus:
        v += [(f"bus {bus} << 8 | 7-bit address", lambda a, b=bus: (b << 8) | a),
              (f"bus {bus} << 8 | 8-bit address", lambda a, b=bus: (b << 8) | (a << 1))]
    return v


def step_b(p, st):
    """EDID block 0 via I2C read at 0x50 against request 0x80."""
    ref = p.edid0()
    if not ref or len(ref) != 128:
        p.log("  no EDID from request 0x80 (monitor asleep or absent?)")
        return "FAIL (no 0x80 EDID)"
    info = edid_info(ref)
    p.log(f"  0x80 EDID: {info}")
    chunk = p.args.i2c_chunk
    for label, addr in i2c_variants(st):
        r = p.cin(REQ_I2C_READ, addr(DDC_EDID), 0, 8, f"I2C read 8 B at offset 0, {label}")
        if r is None or len(r) < 8 or r[:8] != ref[:8]:
            p.log(f"  {label}: no EDID header")
            continue
        p.log(f"  {label}: EDID header found; reading 128 bytes in {chunk}-byte pieces, wIndex = offset")
        got = bytearray()
        for off in range(0, 128, chunk):
            r = p.cin(REQ_I2C_READ, addr(DDC_EDID), off, chunk, f"I2C read EDID offset {off}")
            if r is None or len(r) != chunk:
                break
            got += r
        if len(got) != 128:
            p.log(f"  only {len(got)} bytes came back")
            return "FAIL (short I2C read)"
        diff = [i for i in range(128) if got[i] != ref[i]]
        if not diff:
            st["i2c_addr"] = addr
            st["i2c_label"] = label
            p.log(f"  PASS: identical to the 0x80 EDID ({label}, wIndex = register offset)")
            return "PASS"
        if all(got[o:o + chunk] == got[:chunk] for o in range(0, 128, chunk)):
            p.log("  every piece repeats offset 0: wIndex is not the offset")
        p.log(f"  {len(diff)} bytes differ, first at {diff[0]}: i2c {got[diff[0]]:#04x} vs 0x80 {ref[diff[0]]:#04x}")
        p.log(f"  I2C: {got.hex()}", console=False)
        return "FAIL (EDID differs)"
    return "FAIL (no variant returned an EDID header)"


def confirm_lg(p, st):
    if st.get("lg") is not None:
        return st["lg"]
    st["lg"] = False
    e = p.edid0()
    info = edid_info(e) if e else None
    p.log(f"  monitor EDID: {info}")
    if not info or info[0] != "GSM" or "27GN7" not in info[2]:
        p.log("  REFUSED: the EDID is not the LG 27GN750 (GSM \"27GN7\"). DDC/CI and audio tests "
              "only run on the LG; the MO34WQC2's controller hangs on DDC experiments.")
        return False
    if p.dry:
        p.log("  [dry run: LG confirmation assumed]")
        st["lg"] = True
        return True
    a = input("  Is the monitor on the T6 the LG 27GN750, NOT the Gigabyte MO34WQC2? "
              "Type LG + Enter to confirm: ").strip()
    p.log(f"  LG confirmation: {a!r}", console=False)
    st["lg"] = a == "LG"
    if not st["lg"]:
        p.log("  REFUSED: not confirmed.")
    return st["lg"]


def step_c(p, st):
    """DDC/CI Get VCP brightness at 0x37 (LG only)."""
    if not confirm_lg(p, st):
        return "REFUSED (not the LG)"
    if "i2c_addr" not in st:
        p.log("  REFUSED: step B did not PASS, the I2C layout is unknown")
        return "REFUSED (B did not pass)"
    addr = st["i2c_addr"](DDC_CI)
    req = ddc_get_vcp(VCP_BRIGHTNESS)
    p.log(f"  DDC/CI request (8-bit address byte first): {req.hex(' ')}")
    for attempt in (1, 2):
        # The address byte 0x6E is the I2C address itself; the first payload
        # byte (0x51) goes in wIndex, like the register offset in step B.
        if not p.cout(REQ_I2C_WRITE, addr, req[1], req[2:],
                      f"I2C write to {DDC_CI:#04x} ({st['i2c_label']}): Get VCP {VCP_BRIGHTNESS:#04x}"):
            return "FAIL (write rejected)"
        p.wait(0.05)
        r = p.cin(REQ_I2C_READ, addr, p.args.ddc_read_index, 11, "I2C read 11 B: VCP reply")
        if r is None:
            return "FAIL (read rejected)"
        try:
            d = ddc_parse_vcp_reply(r, VCP_BRIGHTNESS)
        except ValueError as e:
            p.log(f"  bad reply: {e}")
            d = None
        if d:
            p.log(f"  reply: source {d['source']:#04x}, length {d['length']}, opcode {d['opcode']:#04x}, "
                  f"result {d['result']} ({'ok' if d['result'] == 0 else 'unsupported VCP'}), "
                  f"VCP {d['vcp']:#04x}, type {d['type']}, checksum {d['checksum']:#04x} "
                  f"(expected {d['expected']:#04x}, {'ok' if d['checksum_ok'] else 'BAD'})")
            if d["ok"]:
                p.log(f"  brightness: current {d['cur']}, maximum {d['max']}")
                seen = p.ask(f"Does the LG's on-screen menu show brightness {d['cur']}?")
                return "PASS" if seen else "INFO (value read, OSD differs)"
        if attempt == 1 and not p.ask("No valid reply. Try once more (same request)?"):
            break
    # TODO(brightness set): a later version adds "Set VCP Feature"
    # (6E 51 84 03 10 hi lo checksum) after a PASS here: read first, write
    # the same value back, then +/-10, restore the original at the end.
    # Never VCP 0x60 (input select). Not in this version: reads only.
    return "FAIL (no valid VCP reply)"


def read_modes(p):
    a = p.cin(REQ_MODES, OUTPUT, 0, 512, "mode table, first 512 B")
    b = p.cin(REQ_MODES, OUTPUT, 512, 448, "mode table, last 448 B")
    if a is None or b is None:
        return []
    raw = a + b
    return [raw[i:i + 32] for i in range(0, len(raw) - 31, 32)]


def step_d(p, st):
    """Enable audio as MCT does, quiet tone, disable."""
    if not confirm_lg(p, st):
        return "REFUSED (not the LG)"
    s = p.cin(REQ_STATUS, OUTPUT, 0, 1, "monitor status")
    if not s or s[0] != 1:
        p.log("  monitor not present / asleep on output 0")
        return "SKIP (no monitor)"
    if p.ask("Set 1920x1080@60 from the adapter's own table and show a grey picture "
             "(the HDMI link must run for audio; n = leave the output as it is)?"):
        entry = None
        for m in read_modes(p):
            f = dict(zip(MODE_FIELDS, struct.unpack(MODE_FMT, m)))
            if (f["hdisplay"], f["vdisplay"], f["hz"]) == (1920, 1080, 60):
                entry = m
                break
        if entry is None:
            p.log("  1920x1080@60 not in the device's table")
            return "FAIL (no 1080p60 entry)"
        p.log(f"  table entry: {decode_mode(entry)}")
        if not (p.cout(REQ_SETMODE, OUTPUT, 0, entry, "set mode 1920x1080@60")
                and p.cout(REQ_READY, 0, 0, b"", "software ready")
                and p.cout(REQ_OUTPUT, OUTPUT, 1, b"", "output on")):
            return "FAIL (mode set)"
        p.wait(1.0)
        pieces = grey_nv12_frame(1920, 1080)
        for i, (sess, frag) in enumerate(pieces):
            if not (p.bulk(sess, f"display session {i}", quiet=i > 0)
                    and p.bulk(frag, f"NV12 grey piece {i}", quiet=i > 0)):
                return "FAIL (frame write)"
        p.log(f"  grey frame sent ({len(pieces)} fragments, all in the log)")
        if not p.ask("Grey picture on the LG (it may take a few seconds to sync)?"):
            return "SKIP (no picture, audio not tried)"

    r = p.cin(REQ_AUD_GET_STATE, 0, 0, 16, "audio engine 0 state (MCT reads it before enabling)")
    if not r or len(r) < 16:
        return "FAIL (no engine state)"
    es = decode_engine_state(r)
    p.log(f"  engine state: {fmt_engine_state(es)}")
    fmts = st.get("formats")
    if not fmts:
        r = p.cin(REQ_AUD_FORMATS, 0, 0, 32, "audio format list")
        fmts = [decode_aud_format(r)] if r and len(r) >= 32 else []
    if es["format"] >= len(fmts):
        p.log(f"  format index {es['format']} not in the format list")
        return "FAIL (unknown format)"
    f = fmts[es["format"]]
    if f["bits"] != 16 or f["channels"] != 2 or not 8000 <= f["rate"] <= 48000:
        p.log(f"  format {fmt_aud_format(f)}: only 16-bit stereo is implemented")
        return "SKIP (format)"
    rate = f["rate"]
    pkt = rate // 100 * 4                      # 10 ms: 1920 B at 48 kHz
    cyc = es["cyclic"] or 0x20000
    secs = max(0.2, min(30.0 if st.get("watch") else 10.0, p.args.tone_seconds))
    amp = max(0.0, min(0.1, p.args.amplitude))
    p.log(f"  playing {secs:.1f} s of 440 Hz at {rate} Hz, amplitude {amp:.3f} "
          f"({20 * math.log10(amp) if amp else -999:.0f} dBFS), {pkt}-byte packets every 10 ms")
    if not p.ask("Volume on the monitor / headphones low? Enable audio and play the tone?"):
        return "SKIP"

    enabled = False
    sent = 0
    result = None
    watcher = EventWatcher(p) if st.get("watch") else None
    if watcher:
        watcher.start()
    try:
        # MCT Linux: a4, then 24 with Activity 1, FormatIndex and CyclicBufferSize
        # from a4, ReturnSize 9600 (t6usbdongle.c t6_libusb_set_AudioEngineStatus).
        if not p.cout(REQ_AUD_SET_STATE, 0, 0, set_engine_state(1, es["format"], cyc),
                      "audio engine 0: Activity 1 (enable)"):
            return "FAIL (enable rejected)"
        enabled = True
        r = p.cin(REQ_AUD_GET_STATE, 0, 0, 16, "engine state after enable")
        if r and len(r) >= 16:
            p.log(f"  {fmt_engine_state(decode_engine_state(r))}")
        # Windows: one 50 ms packet of silence first (9600 B), then 10 ms packets.
        pre = bytes(pkt * 5)
        restart = p.args.restart_flag
        if not (p.bulk(audio_session_header(len(pre), restart),
                       f"audio session (prefill{', restart flag' if restart else ''})")
                and p.bulk(pre, "audio 50 ms silence")):
            result = "FAIL (bulk)"
            return result
        pcm = tone(rate, secs, amplitude=amp) + bytes(pkt * 10)   # + 100 ms silence
        n = (len(pcm) + pkt - 1) // pkt
        t0 = time.monotonic()
        for k in range(n):
            data = pcm[k * pkt:(k + 1) * pkt].ljust(pkt, b"\0")
            if not p.dry:
                due = t0 + k * 0.010 - 0.040            # keep ~40 ms ahead of playback
                d = due - time.monotonic()
                if d > 0:
                    time.sleep(d)
            quiet = 2 <= k < n - 1
            if not (p.bulk(audio_session_header(pkt), f"audio session {k + 1}/{n}", quiet=quiet)
                    and p.bulk(data, f"audio pcm {k + 1}/{n}", quiet=quiet)):
                result = "FAIL (bulk)"
                return result
            sent += 1
            if k == n // 2:
                r = p.cin(REQ_AUD_GET_STATE, 0, 0, 16, "engine state mid-tone")
                if r and len(r) >= 16:
                    p.log(f"  {fmt_engine_state(decode_engine_state(r))}")
        p.log(f"  sent {sent} packets of {pkt} B after the prefill")
        p.wait(0.15)
    finally:
        if enabled:
            # Windows stops with Activity 0 (capture pkt 1092); MCT Linux never stops.
            p.cout(REQ_AUD_SET_STATE, 0, 0, set_engine_state(0, es["format"], cyc),
                   "audio engine 0: Activity 0 (disable)")
            r = p.cin(REQ_AUD_GET_STATE, 0, 0, 16, "engine state after disable")
            if r and len(r) >= 16:
                p.log(f"  {fmt_engine_state(decode_engine_state(r))}")
        if watcher:
            p.wait(0.3)
            watcher.stop()
            watcher.report(t0 if sent else None)
    heard = p.ask("Did the monitor play a short, quiet 440 Hz tone (headphone jack)?")
    return "PASS" if heard else "FAIL (not heard)"


class EventWatcher:
    """Reads the interrupt endpoint (0x83) in a thread while audio plays and
    logs every 64-byte event: MCT's T6INTERRUPTDATA (t6bulkdef.h): byte 0
    function mask (0x04 video, 0x20 audio), 0x13 display event, 0x20 u32:
    audio render DMA pointer (bits 0-23), jack 0 (24), DVO HDMI (25), LVDS
    HDMI (26). The DMA pointer's rate against the host clock is the audio
    clock the kernel driver can follow."""

    def __init__(self, p):
        self.p, self.events, self.stop_flag, self.thread = p, [], False, None

    def start(self):
        if self.p.dry:
            self.p.log("  [dry run] would read EP 0x83 in a thread during playback")
            return
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop_flag:
            try:
                d = bytes(self.p.dev.read(0x83, 64, 100))
            except Exception as e:                      # timeouts while idle
                if "timeout" not in str(e).lower() and "timed out" not in str(e).lower():
                    self.events.append((time.monotonic(), None, str(e)))
                    time.sleep(0.05)
                continue
            self.events.append((time.monotonic(), d, None))

    def stop(self):
        self.stop_flag = True
        if self.thread:
            self.thread.join(1.0)

    def report(self, t0):
        p = self.p
        audio = []
        for t, d, err in self.events:
            if d is None:
                p.log(f"  EVENT error {err}", console=False)
                continue
            rel = (t - t0) if t0 else t
            fm = d[0]
            disp = d[0x13] if len(d) > 0x13 else 0
            val = struct.unpack_from("<I", d, 0x20)[0] if len(d) >= 0x24 else 0
            ptr = val & 0xFFFFFF
            p.log(f"  EVENT {rel * 1000:9.1f} ms fmask {fm:02x} disp {disp:02x} audio "
                  f"{val:08x} (ptr {ptr}, jack {val >> 24 & 1}, dvo-hdmi {val >> 25 & 1}, "
                  f"lvds-hdmi {val >> 26 & 1}) raw {d.hex()}", console=False)
            if fm & 0x20:
                audio.append((t, ptr))
        p.log(f"  events read: {len(self.events)}, with the audio bit: {len(audio)}")
        if len(audio) >= 3:
            gaps = [(b[0] - a[0]) * 1000 for a, b in zip(audio, audio[1:])]
            p.log(f"  audio events every {statistics.median(gaps):.1f} ms (median; min "
                  f"{min(gaps):.1f}, max {max(gaps):.1f})")
            # pointer advance per second, unwrapping at the cyclic buffer
            span = audio[-1][0] - audio[0][0]
            adv, prev = 0, audio[0][1]
            for _, ptr in audio[1:]:
                step = ptr - prev
                if step < 0:
                    step += max(prev, 1 << 17)          # wrapped at the cyclic buffer
                adv += step
                prev = ptr
            if span > 0:
                rate = adv / span
                p.log(f"  DMA pointer advance: {adv} B in {span:.3f} s = {rate:.1f} B/s "
                      f"(48 kHz stereo 16-bit = 192000 B/s: "
                      f"{(rate / 192000 - 1) * 1e6:+.0f} ppm against the host clock)")
            p.log(f"  pointer values: first {audio[0][1]}, last {audio[-1][1]}, "
                  f"distinct steps {sorted(set(b[1] - a[1] for a, b in zip(audio, audio[1:])))[:8]}")
        elif not self.events:
            p.log("  no events at all during playback")


def step_e(p, st):
    """Step D with a 20 s tone while logging interrupt events (the audio clock)."""
    st["watch"] = True
    if p.args.tone_seconds == 5.0:
        p.args.tone_seconds = 20.0
    return step_d(p, st)


STEPS = {
    "A": ("function descriptors and audio caps (read-only)", step_a, "low"),
    "B": ("EDID through the I2C read request vs 0x80 (read-only)", step_b, "low"),
    "C": ("DDC/CI Get VCP brightness, LG only (I2C write + read)", step_c, "low-medium"),
    "D": ("HDMI audio test tone (enable, bulk sessions, disable)", step_d, "MEDIUM"),
    "E": ("audio clock: 20 s tone while logging interrupt events (EP 0x83)", step_e, "MEDIUM"),
}


def run(args):
    steps = "".join(dict.fromkeys(c for c in args.steps.upper() if c in STEPS))
    if "C" in steps and "B" not in steps:
        steps = steps.replace("C", "BC")            # C needs B's I2C layout
    args.steps = steps
    p = Probe(args)
    print(f"log: {args.log}")
    st, res = {}, []
    try:
        try:
            p.open()
        except RuntimeError as e:
            p.log(f"cannot open the T6: {e}")
            return 1
        if not p.alive():
            p.log("the T6 does not answer before the first step; stopping")
            return 1
        for c in steps:
            title, fn, risk = STEPS[c]
            print()
            p.log(f"=== STEP {c}: {title} [risk {risk}]")
            if not p.ask(f"Run step {c}?"):
                res.append((c, "SKIP"))
                continue
            try:
                r = fn(p, st)
            except KeyboardInterrupt:
                r = "ABORTED (ctrl-c)"
            res.append((c, r))
            p.log(f"--- step {c}: {r}")
            if p.gone:
                break
            if not p.alive():
                res.append((c + "+", "T6 STOPPED ANSWERING"))
                p.recovery()
                break
    finally:
        print()
        p.log("summary:")
        for c, r in res:
            p.log(f"  {c}  {r}")
        p.log(f"log: {args.log}")
        p.close()
    return 0


# ---------------------------------------------------------------- self-test

def self_test():
    fails = []

    def check(name, cond):
        print(f"  {'ok  ' if cond else 'FAIL'} {name}")
        if not cond:
            fails.append(name)

    check("setup packet c0 b1 0 3 132", setup_packet(0xC0, 0xB1, 0, 3, 132).hex() == "c0b1000003008400")
    check("setup packet 40 24 0 0 16", setup_packet(0x40, 0x24, 0, 0, 16).hex() == "4024000000001000")
    for req in (0x01, 0x13, 0x30):
        for bm in (0x40, 0xC0):
            try:
                setup_packet(bm, req, 0, 0, 0)
                check(f"request {req:#04x} refused", False)
            except ValueError:
                check(f"request {bm:02x} {req:#04x} refused", True)
    for bad in ((0x40, 0x1F), (0xC0, 0x82), (0x40, 0x13), (0x00, 0x09)):
        try:
            setup_packet(bad[0], bad[1], 0, 0, 0)
            check(f"{bad[0]:02x} {bad[1]:02x} off the allowlist", False)
        except ValueError:
            check(f"{bad[0]:02x} {bad[1]:02x} off the allowlist", True)
    src = open(os.path.abspath(__file__), encoding="utf-8").read()
    check("no USB reset call in the source", ("." + "reset(") not in src)

    # audio framing against the Windows capture (bulk.txt / ctrl.txt)
    check("audio session header = capture (1920 B)", audio_session_header(1920).hex() ==
          "0300000080070000000000008007000000000000000000000000000000000000")
    check("audio session header = capture (9600 B prefill)", audio_session_header(9600).hex() ==
          "0300000080250000000000008025000000000000000000000000000000000000")
    check("set engine state enable = capture pkt 174",
          set_engine_state(1, 0, 0x20000).hex() == "01000000000000008025000000000200")
    check("set engine state disable = capture pkt 154/1092",
          set_engine_state(0, 0, 0x20000).hex() == "00000000000000008025000000000200")
    es = decode_engine_state(CAP_A4)
    check("a4 decode", (es["activity"], es["jack"], es["format"], es["cyclic"]) == (0, 1, 0, 0x20000))
    c = decode_engine_caps(CAP_A1_0)
    check("a1 decode", (c["type"], c["nodes"], c["formats"], c["max_buffer"]) == (0, 1, 1, 0x20000))
    f = decode_aud_format(CAP_A5)
    check("a5 decode 48 kHz 16 bit stereo", (f["rate"], f["bits"], f["channels"], f["bclk"]) ==
          (48000, 16, 2, 1536000))
    check("a2 decode volume 0..31", fmt_node_caps(CAP_A2) == "type 0 (volume), step 1, range 0..31")
    check("a3 decode", fmt_node_value(CAP_A3).startswith("channel mask 0x03, values 31 31 15"))
    check("capture replies have their request lengths",
          [len(x) for x in (CAP_B1_0, CAP_B1_3, CAP_A1_0, CAP_A4, CAP_A5, CAP_A2, CAP_A3)] ==
          [132, 132, 16, 16, 32, 16, 40])
    d0 = decode_func_desc(CAP_B1_0, 0)
    check("b1[0] name and id", d0[0] == 'VID 0x0711, function 0x5621, name "Trigger 6 External Graphics"')
    check("b1[0] caps: 30 tables at 0x70", any("30 timing tables at offset 0x70" in x for x in d0))
    check("b1[0] mode entry 640x480@60", any("640x480@60 25175 kHz" in x for x in d0))
    d3 = decode_func_desc(CAP_B1_3, 3)
    check("b1[3] name", d3[0] == 'VID 0x0711, function 0x5640, name "USB Audio Device"')
    check("b1[3] format = a5", any(x.startswith("+0x64 format: 48000 Hz, 16 bit, 2 ch") for x in d3))
    pcm = tone(48000, 0.01)
    check("tone: 10 ms at 48 kHz = 1920 B", len(pcm) == 1920)
    peak = max(abs(v) for v in struct.unpack(f"<{len(tone(48000, 0.5)) // 2}h", tone(48000, 0.5)))
    check("tone peak at amplitude 0.03 <= 984", 900 < peak <= 984)

    # DDC/CI
    check("Get VCP 0x10 = 6E 51 82 01 10 AC", ddc_get_vcp(0x10).hex() == "6e51820110ac")
    r = ddc_parse_vcp_reply(canned_vcp_reply(37, 100), 0x10)
    check("VCP reply parse", r["ok"] and (r["cur"], r["max"]) == (37, 100))
    bad = bytearray(canned_vcp_reply(37, 100))
    bad[10] ^= 1
    check("VCP reply bad checksum detected", not ddc_parse_vcp_reply(bytes(bad), 0x10)["ok"])
    check("VCP reply 'unsupported' not ok", not ddc_parse_vcp_reply(
        bytes((0x6E, 0x88, 0x02, 0x01, 0x10, 0, 0, 0, 0, 0)) +
        bytes((ddc_checksum(bytes((0x6E, 0x88, 0x02, 0x01, 0x10, 0, 0, 0, 0, 0)), 0x50),)), 0x10)["ok"])
    try:
        ddc_parse_vcp_reply(b"\x6e\x88", 0x10)
        check("short VCP reply refused", False)
    except ValueError:
        check("short VCP reply refused", True)

    # EDID and modes
    e = synthetic_edid()
    check("synthetic EDID reads as GSM 27GN7", edid_info(e) == ("GSM", 0x5B9F, "27GN7", True))
    check("not an EDID -> None", edid_info(bytes(128)) is None)
    m = mode_entry(148500, 60, 2200, 1920, 2008, 44, 1125, 1080, 1084, 5)
    check("1080p60 entry PLL 700/1000/29/1", struct.unpack(MODE_FMT, m)[11:15] == (700, 1000, 29, 1))
    pieces = grey_nv12_frame(1920, 1080)
    total = 48 + 1920 * 1080 * 3 // 2
    check("grey frame fragments cover the frame",
          sum(len(f) for _, f in pieces) == total and all(
              struct.unpack("<8I", s)[1] == total for s, _ in pieces))
    check("grey frame: more flag on all but the last",
          [struct.unpack("<8I", s)[5] for s, _ in pieces] == [1] * (len(pieces) - 1) + [0])
    print(f"\nself-test: {'all ok' if not fails else f'{len(fails)} FAILED'}")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--steps", default="ABCD", help="steps to run, e.g. AB (default ABCD)")
    ap.add_argument("--dry-run", action="store_true", help="no USB access: print every request")
    ap.add_argument("--self-test", action="store_true", help="check builders and decoders, then exit")
    ap.add_argument("--log", default=None, help="log file (default /tmp/t6_probe-<time>.log)")
    ap.add_argument("--tone-seconds", type=float, default=5.0, help="tone length, 0.2-10 s (default 5)")
    ap.add_argument("--restart-flag", action="store_true",
                    help="audio: set the restart-engine flag on the first packet (not needed: "
                         "playback works without it, 2026-10-12)")
    ap.add_argument("--amplitude", type=float, default=0.03,
                    help="tone amplitude, 0-0.1 of full scale (default 0.03, about -30 dBFS)")
    ap.add_argument("--i2c-chunk", type=int, default=16, choices=(8, 16, 32, 64, 128),
                    help="bytes per I2C read in step B (default 16)")
    ap.add_argument("--ddc-read-index", type=lambda s: int(s, 0), default=0x51,
                    help="wIndex of the DDC/CI reply read in step C (default 0x51; unknown semantics)")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.log is None:
        args.log = time.strftime("/tmp/t6_probe-%Y%m%d-%H%M%S.log")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
