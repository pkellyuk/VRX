"""Render sample frames from the film and lay them out as contact sheets.

Checks both aspect ratios, reports per-shot render timing, and writes the sheets
to promo/build/sheets/ so the film can be looked at before committing to a full
render.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_film as F  # noqa: E402
from film import vrxmedia as M  # noqa: E402
from film.vrx_shotlist import SHOTS, schedule  # noqa: E402

OUT = M.ensure_build_dir() / "sheets"
OUT.mkdir(parents=True, exist_ok=True)

# Sample points: one early and one late frame inside each shot, plus the beats
# that matter (the wipe, the pull-back, the end card).
SAMPLES = [
    ("flat  early", 0.30), ("flat  late", 2.60),
    ("depth mid", 4.60), ("depth late", 6.60),
    ("window a", 9.00), ("window b", 12.40), ("window c", 15.60), ("window pull", 17.30),
    ("room  early", 18.60), ("room  late", 24.00),
    ("easy  early", 26.40), ("easy  late", 31.20),
    ("end   mid", 34.60), ("end   late", 39.00),
]


def render_samples(vertical: bool) -> tuple[list[tuple[str, np.ndarray]], float, dict]:
    film = F.Film(1080 if vertical else 1920, 1920 if vertical else 1080,
                  vertical=vertical)
    out, total = [], 0.0
    for label, t in SAMPLES:
        t0 = time.perf_counter()
        frame = film.frame_at(t)
        dt = time.perf_counter() - t0
        total += dt
        print(f"  {label:14s} t={t:5.2f}s  {dt*1000:6.0f} ms  "
              f"range {frame.min():.3f}-{frame.max():.3f}  {frame.shape}")
        out.append((label, frame))
    return out, total / len(SAMPLES), dict(film.audit)


def sheet(frames, cols: int, tile_w: int, path: Path, title: str) -> None:
    tiles = []
    for label, f in frames:
        h, w = f.shape[:2]
        th = int(tile_w * h / w)
        small = cv2.resize(f, (tile_w, th), interpolation=cv2.INTER_AREA)
        small = (np.clip(small, 0, 1) * 255).astype(np.uint8)
        small = cv2.cvtColor(small, cv2.COLOR_RGB2BGR)
        cv2.putText(small, label, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (240, 240, 240), 1, cv2.LINE_AA)
        tiles.append(small)
    rows = []
    for i in range(0, len(tiles), cols):
        chunk = tiles[i:i + cols]
        th = chunk[0].shape[0]
        while len(chunk) < cols:
            chunk.append(np.zeros_like(chunk[0]))
        rows.append(np.hstack(chunk[:cols]))
    grid = np.vstack(rows)
    grid = cv2.copyMakeBorder(grid, 34, 6, 6, 6, cv2.BORDER_CONSTANT, value=(20, 16, 12))
    cv2.putText(grid, title, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.7,
                (240, 232, 62), 2, cv2.LINE_AA)
    cv2.imwrite(str(path), grid)
    print("wrote", path, grid.shape)


print("=== 16:9 master ===")
wide, wide_avg, wide_audit = render_samples(vertical=False)
sheet(wide, 3, 640, OUT / "wide_sheet.png", "VRX promo 1920x1080 - sample frames")

print("\n=== 9:16 social ===")
tall, tall_avg, tall_audit = render_samples(vertical=True)
sheet(tall, 5, 384, OUT / "tall_sheet.png", "VRX promo 1080x1920 - sample frames")

print("\n=== headline layout audit (fitted size, space available) ===")
for name, audit in (("16:9", wide_audit), ("9:16", tall_audit)):
    print(f"  {name}")
    for text, (size, avail) in audit.items():
        layer = None
        print(f"    {size:3d}px in {avail:5d}px  \"{text[:44]}\"")

print(f"\naverage frame render: wide {wide_avg*1000:.0f} ms, tall {tall_avg*1000:.0f} ms")
print(f"shots: {[s.key for s in SHOTS]}")
print(f"schedule: {[(k, round(a,2), round(b,2)) for k, a, b in schedule()]}")
