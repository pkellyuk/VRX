"""Report the size, mode and depth statistics of every promo asset."""
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

for folder in [ROOT / "assets", ROOT.parent / "docs"]:
    print(f"--- {folder} ---")
    for p in sorted(folder.iterdir()):
        if p.suffix.lower() not in {".png", ".jpg", ".jpeg"}:
            continue
        im = Image.open(p)
        print(f"{p.name:26s} {str(im.size):14s} {im.mode}")
        if "depth" in p.name:
            d = np.asarray(im.convert("L"))
            print(f"{'':26s} depth range {d.min()}-{d.max()} mean {d.mean():.1f}")
