"""Why is the depth-map visual warm when the ramp is cyan to violet?"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

PROMO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROMO))
from film import vrx_brand as B  # noqa: E402
from film import vrx_dibr as D  # noqa: E402

shot = D.DepthShot("cyberpunk", 1920, 1080, crop_bias=0.60)
print("Z: min %.3f max %.3f" % (shot.Z.min(), shot.Z.max()))
inv = 1.0 / shot.Z
print("inv: min %.3f max %.3f  p1 %.3f p99 %.3f"
      % (inv.min(), inv.max(), *np.percentile(inv, [1, 99])))

dep = shot.depth_colour()
print("depth_colour shape", dep.shape, "range", dep.min(), dep.max())
mean = dep.reshape(-1, 3).mean(axis=0)
print("depth_colour mean RGB", np.round(mean * 255, 1))
print("  RGB(%.0f, %.0f, %.0f)" % tuple(mean * 255))
for name, c in (("CYAN", B.CYAN), ("VIOLET", B.VIOLET)):
    print(f"  {name} is RGB{c}")

# What does the film's own depth shot produce?
from film import vrx_film as F  # noqa: E402
film = F.Film(1920, 1080, vertical=False)
frame = film._depth(1.2)
m = frame.reshape(-1, 3).mean(axis=0) * 255
print("film._depth(1.2) mean RGB (%.0f, %.0f, %.0f)" % tuple(m))
print("  fraction of frame that is depth:", round(1 - (0.15), 3))

# Sample a pixel well inside the depth region.
print("frame[540, 1200] =", np.round(frame[540, 1200] * 255, 1))
print("dep[540, 1200]   =", np.round(dep[540, 1200] * 255, 1))
