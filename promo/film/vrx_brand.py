"""The VRX brand tokens, taken from the product rather than invented.

Colours come from the published site stylesheet and the real logo:
    deep background #05070F, panel #111628
    cyan accent    #3EE8F0   (the site's --accent and the logo's neon)
    magenta accent #FF3FD0
    violet accent  #7B6CFF
    body text      #E6E9F5, muted #AAB2CC

Typeface: the site and the app both set system-ui, which is Segoe UI on Windows,
so the film uses Segoe UI (regular and bold) with Bahnschrift for numeric labels.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import ImageFont

PROMO = Path(__file__).resolve().parents[1]
REPO = PROMO.parent
ASSETS = PROMO / "assets"
DOCS = REPO / "docs"
BUILD = PROMO / "build"

# --- colour (RGB, as the eye sees it) -------------------------------------
DEEP = (5, 7, 15)
BG = (10, 13, 24)
PANEL = (17, 22, 40)
CYAN = (62, 232, 240)
MAGENTA = (255, 63, 208)
VIOLET = (123, 108, 255)
TEXT = (230, 233, 245)
MUTED = (170, 178, 204)
DIM = (120, 128, 156)

ACCENTS = (CYAN, MAGENTA, VIOLET)

# --- type -----------------------------------------------------------------
FONTS = Path(r"C:\Windows\Fonts")
SEGOE = FONTS / "segoeui.ttf"
SEGOE_BOLD = FONTS / "segoeuib.ttf"
SEGOE_LIGHT = FONTS / "segoeuil.ttf"
SEGOE_SEMILIGHT = FONTS / "segoeuisl.ttf"
BAHNSCHRIFT = FONTS / "bahnschrift.ttf"

_font_cache: dict[tuple[str, int], ImageFont.FreeTypeFont] = {}


def font(size: int, weight: str = "regular") -> ImageFont.FreeTypeFont:
    """Load a brand font at a pixel size. weight: regular|bold|light|semilight|mono."""
    path = {
        "regular": SEGOE,
        "bold": SEGOE_BOLD,
        "light": SEGOE_LIGHT,
        "semilight": SEGOE_SEMILIGHT,
        "mono": BAHNSCHRIFT,
    }[weight]
    key = (str(path), size)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(str(path), size)
    return _font_cache[key]


def gradient(width: int, height: int, top=DEEP, bottom=PANEL) -> np.ndarray:
    """The film's background: a vertical ramp between two brand darks, RGB float.

    Returned in the film's working range of 0..1, like every other frame buffer.
    """
    t = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None, None]
    top_a = np.array(top, np.float32)[None, None, :] / 255.0
    bot_a = np.array(bottom, np.float32)[None, None, :] / 255.0
    ramp = top_a * (1 - t) + bot_a * t
    return np.repeat(ramp, width, axis=1)


def vignette(width: int, height: int, strength: float = 0.55, power: float = 2.4) -> np.ndarray:
    """A radial darkening mask, 1.0 in the middle, (1-strength) in the corners."""
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    cx, cy = (width - 1) / 2, (height - 1) / 2
    r = np.sqrt(((x - cx) / cx) ** 2 + ((y - cy) / cy) ** 2) / np.sqrt(2)
    return (1.0 - strength * np.clip(r, 0, 1) ** power)[:, :, None]


def hex_to_rgb(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


if __name__ == "__main__":
    names = {
        "DEEP": DEEP, "BG": BG, "PANEL": PANEL, "CYAN": CYAN,
        "MAGENTA": MAGENTA, "VIOLET": VIOLET, "TEXT": TEXT, "MUTED": MUTED,
    }
    for k, v in names.items():
        print(f"{k:8s} #{v[0]:02X}{v[1]:02X}{v[2]:02X}")
    for p in [SEGOE, SEGOE_BOLD, SEGOE_LIGHT, SEGOE_SEMILIGHT, BAHNSCHRIFT]:
        print(f"{p.name:18s} {'OK' if p.exists() else 'MISSING'}")
    for w in ["regular", "bold", "light", "semilight", "mono"]:
        print(f"font(28, {w:10s}) -> {font(28, w).getname()}")
