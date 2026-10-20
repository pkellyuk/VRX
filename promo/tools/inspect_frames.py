"""Inspect specific frames of the delivered master: what colour are they really?"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

PROMO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROMO))
from film import vrxmedia as M  # noqa: E402

VIDEO = PROMO / "VRX-promo-1080p.mp4"


def grab(t: float) -> np.ndarray | None:
    res = subprocess.run(
        [M.FFMPEG, "-v", "error", "-ss", f"{t:.3f}", "-i", str(VIDEO),
         "-frames:v", "1", "-f", "image2pipe", "-vcodec", "png", "-"],
        capture_output=True)
    if res.returncode != 0 or not res.stdout:
        return None
    return cv2.imdecode(np.frombuffer(res.stdout, np.uint8), cv2.IMREAD_COLOR)


print(f"{'time':>6s} {'mean BGR':>20s} {'dominant hue':>14s}  coarse grid")
for t in [1.2, 3.1, 4.6, 5.2, 5.8, 6.4, 6.8, 8.6, 19.5]:
    img = grab(t)
    if img is None:
        print(f"{t:6.1f}  (no frame)")
        continue
    mean = img.reshape(-1, 3).mean(axis=0)
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
    # Dominant hue among reasonably saturated, reasonably bright pixels.
    sel = (hsv[:, :, 1] > 60) & (hsv[:, :, 2] > 60)
    hue = float(np.median(hsv[:, :, 0][sel])) * 2 if sel.any() else -1.0
    small = cv2.resize(img, (6, 3), interpolation=cv2.INTER_AREA)
    grid = " ".join(f"{b:3d},{g:3d},{r:3d}" for b, g, r in
                    small.reshape(-1, 3)[:6])
    print(f"{t:6.1f}  {mean[0]:6.1f},{mean[1]:6.1f},{mean[2]:6.1f}  "
          f"{hue:6.0f} deg  {grid}")
