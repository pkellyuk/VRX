"""Narrow down why the renderer is not an exact identity at the origin."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_dibr as D  # noqa: E402

W, H = 1920, 1080
s = D.DepthShot("cyberpunk", W, H)
mx, my, holes = s.maps(0.0, 0.0, 0.0)
print("map_x error vs pure grid: max", float(np.abs(mx - s._px).max()))
print("map_y error vs pure grid: max", float(np.abs(my - s._py).max()))
print("holes", int(holes.sum()), "at", np.argwhere(holes)[:5].tolist())

f, n = s.render(0.0, 0.0, 0.0)
d = np.abs(f - s.colour).max(axis=2)
print("max channel diff", float(d.max()))
ys, xs = np.where(d > 0.05)
print("pixels over 0.05:", len(ys))
if len(ys):
    print("  rows", ys.min(), "-", ys.max(), " cols", xs.min(), "-", xs.max())
    print("  sample", [(int(y), int(x), round(float(d[y, x]), 3)) for y, x in list(zip(ys, xs))[:8]])

print("\nZ stats: min", float(s.Z.min()), "max", float(s.Z.max()))
print("depth stats: min", float(s.depth.min()), "max", float(s.depth.max()))
bad = s.Z < 1e-3
print("Z below 1e-3:", int(bad.sum()))
