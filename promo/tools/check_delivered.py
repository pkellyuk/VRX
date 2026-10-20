"""Read the current deliverables back and report anything wrong with them."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrxmedia as M  # noqa: E402
from film.vrx_shotlist import FPS  # noqa: E402

PROMO = M.PROMO

TARGETS = [
    ("VRX-promo-1080p.mp4", 1920, 1080),
    ("VRX-promo-vertical-1080x1920.mp4", 1080, 1920),
    ("build/wide_silent.mp4", 1920, 1080),
    ("build/tall_silent.mp4", 1080, 1920),
]

for name, w, h in TARGETS:
    path = PROMO / name
    if not path.exists():
        print(f"{name:38s} MISSING")
        continue
    size = path.stat().st_size / 1e6
    problems = M.verify_encoded(path, w, h, 1200, FPS)
    if problems:
        print(f"{name:38s} {size:6.2f} MB  BAD")
        for p in problems:
            print(f"      {p}")
    else:
        print(f"{name:38s} {size:6.2f} MB  ok")
