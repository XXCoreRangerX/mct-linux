#!/usr/bin/env python3
"""Offline: chroma subsampling methods for NV12 on coloured text, Lab dE
against the original, with nearest and bilinear chroma upsampling."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import glob

# --- test image: coloured text and UI bits on several backgrounds
W, H = 960, 540
img = Image.new("RGB", (W, H), (255, 255, 255))
d = ImageDraw.Draw(img)
fonts = glob.glob("/nix/store/*dejavu*/share/fonts/truetype/DejaVuSans.ttf") + glob.glob("/run/current-system/sw/share/X11/fonts/*.ttf")
font = ImageFont.truetype(fonts[0], 13) if fonts else ImageFont.load_default()
bgs = [(255, 255, 255), (30, 30, 30), (40, 44, 52), (245, 245, 220)]
fgs = [(220, 50, 47), (38, 139, 210), (133, 153, 0), (211, 54, 130), (255, 255, 255), (0, 0, 0), (181, 137, 0), (42, 161, 152)]
bh = H // len(bgs)
for i, bg in enumerate(bgs):
    d.rectangle([0, i * bh, W, (i + 1) * bh], fill=bg)
    for j, fg in enumerate(fgs):
        d.text((10 + (j % 2) * 470, i * bh + 8 + (j // 2) * 30), "The quick brown fox 0123 ->|[]{}", fill=fg, font=font)
rgb = np.asarray(img).astype(np.float64)

def srgb_to_lin(c): c = c / 255; return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def lin_to_srgb(c): c = np.clip(c, 0, 1); return 255 * np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)
def to_yuv(r, g, b):
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = -0.168736 * r - 0.331264 * g + 0.5 * b
    v = 0.5 * r - 0.418688 * g - 0.081312 * b
    return y, u, v
def to_rgb(y, u, v):
    return np.stack([y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u], -1)
def box(a): return a.reshape(H // 2, 2, W // 2, 2).mean((1, 3))
def up_nearest(c): return c.repeat(2, 0).repeat(2, 1)
def up_bilinear(c):
    # chroma sited at 2x2 centres, bilinear (clamped edges)
    from numpy import clip
    ys = (np.arange(H) - 0.5) / 2; xs = (np.arange(W) - 0.5) / 2
    y0 = np.floor(ys).astype(int); x0 = np.floor(xs).astype(int)
    fy = ys - y0; fx = xs - x0
    y0c, y1c = clip(y0, 0, H // 2 - 1), clip(y0 + 1, 0, H // 2 - 1)
    x0c, x1c = clip(x0, 0, W // 2 - 1), clip(x0 + 1, 0, W // 2 - 1)
    a = c[y0c][:, x0c] * (1 - fx) + c[y0c][:, x1c] * fx
    b = c[y1c][:, x0c] * (1 - fx) + c[y1c][:, x1c] * fx
    return a * (1 - fy[:, None]) + b * fy[:, None]

R, G, B = rgb[..., 0], rgb[..., 1], rgb[..., 2]
Y, U, V = to_yuv(R, G, B)

def method(name):
    if name in ("box", "box+luma"):
        u, v = box(U), box(V)
    else:  # linear light average
        lr, lg, lb = (box(srgb_to_lin(c)) for c in (R, G, B))
        _, u, v = to_yuv(lin_to_srgb(lr), lin_to_srgb(lg), lin_to_srgb(lb))
    return u, v

def lab(rgb8):
    c = srgb_to_lin(np.clip(rgb8, 0, 255))
    M = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = c @ M.T / np.array([0.95047, 1, 1.08883])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)

ref = lab(rgb)
for up_name, up in (("nearest", up_nearest), ("bilinear", up_bilinear)):
    print(f"--- chip upsampling: {up_name}")
    for name in ("box", "linear", "box+luma", "linear+luma"):
        u, v = method(name.replace("+luma", ""))
        U2, V2 = up(u), up(v)
        y = Y.copy()
        if "luma" in name:
            # per pixel Y that best reproduces its RGB with the shared chroma (unweighted LS)
            c = to_rgb(np.zeros_like(Y), U2, V2)
            y = (rgb - c).mean(-1)
        out = np.clip(np.rint(to_rgb(np.clip(np.rint(y), 0, 255), np.rint(U2), np.rint(V2))), 0, 255)
        de = np.sqrt(((lab(out) - ref) ** 2).sum(-1))
        edge = de[de > 0.5]
        print(f"  {name:12} mean dE {de.mean():.3f}  p99 {np.percentile(de, 99):6.2f}  max {de.max():6.1f}  px over 5: {(de > 5).mean() * 100:.2f}%")
        if up_name == "nearest":
            Image.fromarray(out.astype(np.uint8)).resize((W * 2, H * 2), Image.NEAREST).save(f"{name}.png")
Image.fromarray(rgb.astype(np.uint8)).resize((W * 2, H * 2), Image.NEAREST).save("orig.png")
