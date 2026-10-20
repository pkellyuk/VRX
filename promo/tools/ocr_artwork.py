"""Read the exact wording out of the real VRX artwork so captions match the product.

Uses the installed Tesseract to OCR the logo, the desktop UI captures and the
site's own hero markup, rather than guessing what they say.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

PROMO = Path(__file__).resolve().parents[1]
DOCS = PROMO.parent / "docs"
TESS = Path(r"C:\Users\paulj\AppData\Local\Programs\Tesseract-OCR\tesseract.exe")


def ocr(path: Path, scale: int = 3, invert: bool = False) -> str:
    if not TESS.exists():
        return "(tesseract not installed)"
    im = Image.open(path)
    if im.mode != "RGB":
        bg = Image.new("RGB", im.size, (255, 255, 255) if invert else (0, 0, 0))
        if "A" in im.getbands():
            bg.paste(im, mask=im.getchannel("A"))
        else:
            bg.paste(im.convert("RGB"))
        im = bg
    im = im.convert("RGB").resize((im.width * scale, im.height * scale), Image.LANCZOS)
    tmp = PROMO / "build" / (path.stem + "_ocr.png")
    tmp.parent.mkdir(exist_ok=True)
    im.save(tmp)
    out = subprocess.run(
        [str(TESS), str(tmp), "stdout", "--psm", "6" if scale < 3 else "11"],
        capture_output=True, text=True,
    )
    return (out.stdout or out.stderr).strip()


if __name__ == "__main__":
    targets = [
        DOCS / "vrx-logo.png",
        DOCS / "vrx-easy-mode.png",
        PROMO / "assets" / "easy-mode.png",
    ]
    for t in targets:
        print(f"=== {t.name} ===")
        for lines in (ocr(t), ocr(t, invert=True)):
            if lines and lines != "(tesseract not installed)":
                print(lines[:800])
        print()
