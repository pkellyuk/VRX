"""Extract the exact VRX brand tokens from the logo and the published captures.

Accent colours are taken from the real logo and the neon edges of the published
artwork, so the film reuses the product's own palette rather than an invented one.
The site's own tokens (recorded by tools/record_brand_tokens.py) are the source of
truth for the interface colours.
"""
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT.parent / "docs"


def hex_of(rgb):
    return "#%02X%02X%02X" % tuple(int(v) for v in rgb)


def alpha_of(img):
    if "A" in img.getbands():
        return np.asarray(img.getchannel("A"))
    return np.full(img.size[::-1], 255, np.uint8)


def top_colours(img, mask=None, k=8, quant=16):
    a = np.asarray(img.convert("RGB")).reshape(-1, 3)
    if mask is not None:
        a = a[np.asarray(mask).reshape(-1).astype(bool)]
    if not len(a):
        return []
    return [(hex_of(c), n) for c, n in Counter(map(tuple, (a // quant) * quant)).most_common(k)]


print("=== logo ===")
logo = Image.open(DOCS / "vrx-logo.png")
alpha = alpha_of(logo)
print("size", logo.size, "opaque pixels", int((alpha > 200).sum()))
solid = np.asarray(logo.convert("RGBA"))[alpha > 200]
sat = solid[:, :3].max(axis=1).astype(int) - solid[:, :3].min(axis=1).astype(int)
if sat.max() > 0:
    vivid = solid[sat >= np.percentile(sat, 99)]
    print("logo accent (mean of most saturated opaque pixels):", hex_of(vivid[:, :3].mean(axis=0)))
for hx, n in top_colours(logo, alpha > 200, k=6, quant=8):
    print("   ", hx, n)

print("\n=== published captures: accent light ===")
for name in ["vrx-headset.png", "vrx-easy-mode.png"]:
    a = np.asarray(Image.open(DOCS / name).convert("RGB")).reshape(-1, 3).astype(int)
    sat = a.max(axis=1) - a.min(axis=1)
    sel = a[(sat > 60) & (a.mean(axis=1) > 90)]
    print(f"{name}: {len(sel)} vivid pixels")
    cyan = sel[(sel[:, 2] >= sel[:, 1]) & (sel[:, 1] >= sel[:, 0])]
    mag = sel[(sel[:, 0] > sel[:, 1]) & (sel[:, 2] > sel[:, 1])]
    if len(cyan):
        print("    cyan   ", hex_of(cyan.mean(axis=0)), len(cyan))
    if len(mag):
        print("    magenta", hex_of(mag.mean(axis=0)), len(mag))

print("\n=== headset capture: dominant colours ===")
for hx, n in top_colours(Image.open(DOCS / "vrx-headset.png").convert("RGB"), k=10, quant=16):
    print("   ", hx, n)
