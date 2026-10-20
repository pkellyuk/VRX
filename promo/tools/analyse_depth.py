"""Depth pairing analysis: are the published depth maps aligned with the captures?

Also infers the depth convention (which end of the range is nearest the camera),
which decides the sign of the parallax in the film.
"""
from __future__ import annotations

import sys
from pathlib import Path

import cv2
import numpy as np

PROMO = Path(__file__).resolve().parents[1]
ASSETS = PROMO / "assets"
PAIRS = ["helldivers", "cyberpunk", "system-shock"]


def load_pair(name: str):
    img = cv2.imread(str(ASSETS / f"{name}.jpg"), cv2.IMREAD_COLOR)
    dep = cv2.imread(str(ASSETS / f"{name}-depth.png"), cv2.IMREAD_GRAYSCALE)
    dep = cv2.resize(dep, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
    return img, dep


def edge_correlation(img, dep, dx, dy):
    """Correlate image luminance edges with depth edges at a pixel offset."""
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    mag = np.hypot(gx, gy)
    dx_ = np.roll(np.roll(dep.astype(np.float32), dy, axis=0), dx, axis=1)
    dgx = cv2.Sobel(dx_, cv2.CV_32F, 1, 0, ksize=3)
    dgy = cv2.Sobel(dx_, cv2.CV_32F, 0, 1, ksize=3)
    dmag = np.hypot(dgx, dgy)
    m = 40
    a = mag[m:-m, m:-m].ravel()
    b = dmag[m:-m, m:-m].ravel()
    return float(np.corrcoef(a, b)[0, 1])


for name in PAIRS:
    img, dep = load_pair(name)
    h, w = dep.shape
    print(f"=== {name} ===  {img.shape[1]}x{img.shape[0]}  depth 0-255")

    # Alignment: which offset maximises the edge correlation?
    best, best_off = -2, (0, 0)
    for dy in range(-4, 5, 2):
        for dx in range(-4, 5, 2):
            c = edge_correlation(img, dep, dx, dy)
            if c > best:
                best, best_off = c, (dx, dy)
    print(f"  best edge alignment offset {best_off}, corr {best:.4f} "
          f"(zero offset corr {edge_correlation(img, dep, 0, 0):.4f})")

    # Depth convention: sample the top band (usually sky/far in a game) and the
    # bottom band (usually ground/near).
    top = dep[: int(h * 0.12)].mean()
    bottom = dep[int(h * 0.88):].mean()
    mid = dep[int(h * 0.45):int(h * 0.55)].mean()
    print(f"  row means: top {top:.1f}  middle {mid:.1f}  bottom {bottom:.1f}")

    # Where is the darkest and brightest content?
    bright = dep > np.percentile(dep, 97)
    dark = dep < np.percentile(dep, 3)
    print(f"  brightest 3% occupies rows {np.where(bright.any(axis=1))[0].min()}-"
          f"{np.where(bright.any(axis=1))[0].max()} of {h}, "
          f"{bright.mean()*100:.2f}% of pixels, mean colour "
          f"{img[bright].mean(axis=0).round(0)}")
    print(f"  darkest   3% occupies rows {np.where(dark.any(axis=1))[0].min()}-"
          f"{np.where(dark.any(axis=1))[0].max()} of {h}, "
          f"{dark.mean()*100:.2f}% of pixels, mean colour "
          f"{img[dark].mean(axis=0).round(0)}")

    # Is the sky region flat in depth (a good far-field proxy)?
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY).astype(np.float32)
    skyish = g[: int(h * 0.10)].mean() > g.mean()
    print(f"  top band is brighter than average (likely sky): {skyish}")
    hist = np.histogram(dep, bins=8, range=(0, 256))[0]
    print(f"  depth histogram (8 bins): {hist}")
    print()
