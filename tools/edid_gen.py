#!/usr/bin/env python3
"""Minimal EDID 1.4 blocks for typical monitors, to see which modes the
driver offers them (through DRM's edid_override, see t6_edid_sim.py).

    edid_gen.py NAME > edid.bin      one profile
    edid_gen.py                      list the profiles
"""
import struct, sys

# (pixel clock kHz, hact, hfront, hsync, hback, vact, vfront, vsync, vback, +h, +v)
def cvt_rb(w, h, hz):
    """CVT reduced blanking v1 style: 160 px horizontal blanking, >= 460 us vertical"""
    vt = h + 6
    while (vt - h) * 1000000 < 460 * hz * vt:
        vt += 1
    return (round((w + 160) * vt * hz / 1000), w, 48, 32, 80, h, 3, 5, vt - h - 8, True, False)


PROFILES = {
    # name: (description, [timings, first = preferred], (min Hz, max Hz, max MHz))
    "1440p144": ("27\" 2560x1440 144 Hz", [cvt_rb(2560, 1440, 144), cvt_rb(2560, 1440, 60)], (48, 144, 600)),
    "1440p120": ("27\" 2560x1440 120 Hz", [cvt_rb(2560, 1440, 120), cvt_rb(2560, 1440, 60)], (48, 120, 500)),
    "4k60": ("27\" 3840x2160 60 Hz", [(533250, 3840, 48, 32, 80, 2160, 3, 5, 54, True, False),
                                     (297000, 3840, 176, 88, 296, 2160, 8, 10, 72, True, True),
                                     cvt_rb(2560, 1440, 60)], (24, 60, 600)),
    "1080p240": ("25\" 1920x1080 240 Hz", [cvt_rb(1920, 1080, 240),
                                          (148500, 1920, 88, 44, 148, 1080, 4, 5, 36, True, True)],
                 (48, 240, 600)),
    "uw100": ("34\" 3440x1440 100 Hz", [cvt_rb(3440, 1440, 100), cvt_rb(3440, 1440, 50)], (48, 100, 600)),
    "uw1080": ("29\" 2560x1080 75 Hz", [cvt_rb(2560, 1080, 75), cvt_rb(2560, 1080, 60)], (48, 75, 250)),
    "1440p60": ("27\" 2560x1440 60 Hz (office)", [(241500, 2560, 48, 32, 80, 1440, 3, 5, 33, True, False)],
                (50, 75, 300)),
}


def dtd(t, size_mm=(600, 340)):
    clk, ha, hf, hs, hb, va, vf, vs, vb, hp, vp = t
    hbl, vbl = hf + hs + hb, vf + vs + vb
    flags = 0x18 | (0x02 if hp else 0) | (0x04 if vp else 0)
    return struct.pack("<H", round(clk / 10)) + bytes([
        ha & 0xff, hbl & 0xff, (ha >> 8) << 4 | hbl >> 8,
        va & 0xff, vbl & 0xff, (va >> 8) << 4 | vbl >> 8,
        hf & 0xff, hs & 0xff, (vf & 0xf) << 4 | (vs & 0xf),
        (hf >> 8) << 6 | (hs >> 8) << 4 | (vf >> 4) << 2 | vs >> 4,
        size_mm[0] & 0xff, size_mm[1] & 0xff, (size_mm[0] >> 8) << 4 | size_mm[1] >> 8,
        0, 0, flags])


def text(tag, s):
    b = s.encode()[:13]
    if len(b) < 13:
        b += b"\n" + b" " * (12 - len(b))
    return bytes([0, 0, 0, tag, 0]) + b


def edid(name):
    desc, timings, (vmin, vmax, mhz) = PROFILES[name]
    hmax = max(round(t[0] / (t[1] + t[2] + t[3] + t[4])) for t in timings)   # kHz
    b = bytearray(b"\x00\xff\xff\xff\xff\xff\xff\x00")
    b += struct.pack(">H", (ord("T") - 64) << 10 | (ord("S") - 64) << 5 | (ord("T") - 64))
    b += struct.pack("<HI", 0x1234, 0) + bytes([1, 36, 1, 4])   # product, serial, week, year, 1.4
    b += bytes([0xa5, 60, 34, 120, 0x0f])                       # digital 8 bpc DP, cm, gamma, features
    b += bytes([0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54])  # sRGB-ish chromaticity
    b += bytes(3) + b"\x01\x01" * 8                              # no established/standard timings
    descs = [dtd(t) for t in timings[:2]]
    descs.append(bytes([0, 0, 0, 0xfd, 0, vmin, vmax, 30, min(hmax + 10, 255), mhz // 10, 0x01,
                        0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20]))
    descs.append(text(0xfc, name))
    while len(descs) < 4:
        descs.append(text(0xfe, desc[:13]))
    b += b"".join(descs[:4])
    b += bytes([0])                                              # no extensions
    b.append((-sum(b)) & 0xff)
    assert len(b) == 128
    return bytes(b)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        for k, (d, t, r) in PROFILES.items():
            print(f"{k:10} {d}, preferred {t[0][1]}x{t[0][5]} at {t[0][0] / 1000:.1f} MHz")
    else:
        sys.stdout.buffer.write(edid(sys.argv[1]))
