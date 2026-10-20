"""Validate the depth-parallax renderer against the real captures and depth maps.

Reports, for each published pair:
  * the depth distribution inside candidate crops, to choose the framing
  * how far the picture actually moves at each depth for a given camera move
  * the fraction of pixels that are disocclusions and need filling
  * how long a frame takes, broken down, so the full render is predictable

Also checks the renderer is an exact identity at the origin (zero camera move
must return the untouched capture) and writes sample frames to promo/build/dibr.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_dibr as D  # noqa: E402
from film import vrxmedia as M  # noqa: E402

W, H = 1920, 1080
OUT = M.ensure_build_dir() / "dibr"
OUT.mkdir(parents=True, exist_ok=True)
GAMES = ["cyberpunk", "helldivers", "system-shock"]


def save(path: Path, rgb: np.ndarray) -> None:
    cv2.imwrite(str(path), cv2.cvtColor((np.clip(rgb, 0, 1) * 255).astype(np.uint8),
                                        cv2.COLOR_RGB2BGR))


print("=== identity check: no camera move must return the capture untouched ===")
s = D.DepthShot("cyberpunk", W, H)
f, holes = s.render(0.0, 0.0, 0.0)
print(f"  max |rendered - source| = {np.abs(f - s.colour).max():.6f}   holes = {holes}")
assert np.abs(f - s.colour).max() < 2e-4 and holes == 0, \
    "renderer is not an identity at the origin"
print("  OK (float32 resampling noise only)")

print("\n=== crop choice: which vertical slice of the capture to use ===")
# A 16:9 frame cut from a 1.27:1 capture has to lose 343 source rows whatever we
# do. The measurement below shows depth spread keeps improving as the crop moves
# down, because the near ground is at the bottom - but the search is capped at
# 0.65 so the film keeps the sky and the game's own composition, and 0.60 is used
# for all three so the three shots are framed consistently.
CROP_BIAS = 0.60
chosen = {}
for g in GAMES:
    rows = []
    for bias in [0.30, 0.40, 0.50, 0.60, 0.65, 0.75]:
        shot = D.DepthShot(g, W, H, crop_bias=bias)
        d = shot.depth
        rows.append((bias, float(d.mean()), float(d.std()),
                     float((d > 0.5).mean()), float(np.percentile(d, 99))))
    print(f"\n{g}")
    for bias, mean, std, near, p99 in rows:
        mark = " <- used" if abs(bias - CROP_BIAS) < 1e-6 else ""
        print(f"  bias {bias:.2f}  mean {mean:.3f}  std {std:.3f}  "
              f"frac>0.5 {near:.3f}  p99 {p99:.3f}{mark}")
    chosen[g] = CROP_BIAS

print("\n=== depth curve: how much of the frame reads as near ===")
for curve in [1.0, 0.8, 0.7, 0.5]:
    line = []
    for g in GAMES:
        shot = D.DepthShot(g, W, H, crop_bias=chosen[g], depth_curve=curve)
        d = shot.depth
        line.append(f"{g.split('-')[0][:6]:6s} std {d.std():.3f} >0.5 {(d>0.5).mean():.3f}")
    print(f"  curve {curve:.2f}: " + " | ".join(line))

print("\n=== parallax strength on the actual camera path ===")
for g in GAMES:
    shot = D.DepthShot(g, W, H, crop_bias=chosen[g])
    for p in [0.0, 0.5, 1.0]:
        tx, ty, tz = shot.camera_path(p)
        pr = shot.shift_profile(tx, tz)
        print(f"{g:14s} p={p:.1f}  tx={tx:+.4f} tz={tz:+.3f}  far {pr['far 10%']:5.2f}  "
              f"mid {pr['middle 10%']:5.2f}  near {pr['nearest 5%']:6.2f}  "
              f"max {pr['max']:6.2f}px  far:near 1:{pr['far_near_ratio']:4.1f}  "
              f"holes {pr['holes_pct']:5.2f}%")
    print(f"  allowed |tx| at tz=0.030: {shot.max_lateral(0.030):.4f}, "
          f"at tz=0.085: {shot.max_lateral(0.085):.4f}")
    print()

print("=== timing breakdown (cyberpunk) ===")
shot = D.DepthShot("cyberpunk", W, H, crop_bias=chosen["cyberpunk"])
tx, ty, tz = shot.camera_path(0.5)
t = time.perf_counter()
for _ in range(5):
    mx, my, holes = shot.maps(tx, ty, tz)
t_maps = (time.perf_counter() - t) / 5
t = time.perf_counter()
for _ in range(5):
    out = cv2.remap(shot.colour, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
t_remap = (time.perf_counter() - t) / 5
mask = cv2.dilate(holes.astype(np.uint8) * 255, np.ones((3, 3), np.uint8))
bgr = cv2.cvtColor((np.clip(out, 0, 1) * 255).astype(np.uint8), cv2.COLOR_RGB2BGR)
t = time.perf_counter()
for _ in range(5):
    cv2.inpaint(bgr, mask, 3, cv2.INPAINT_TELEA)
t_inpaint = (time.perf_counter() - t) / 5
print(f"  maps {1000*t_maps:6.1f} ms   colour remap {1000*t_remap:5.1f} ms   "
      f"inpaint {1000*t_inpaint:6.1f} ms   hole pixels {int(holes.sum())}")
print(f"  -> about {1000*(t_maps+t_remap+t_inpaint):.0f} ms per frame")

print("\n=== motion across a shot: arrives still, drifts, settles ===")
N = 40
diffs, hole_counts, txs = [], [], []
prev = None
for i in range(N):
    p = i / (N - 1)
    tx, ty, tz = shot.camera_path(p)
    f, h = shot.render(tx, ty, tz)
    hole_counts.append(h)
    txs.append(tx)
    if prev is not None:
        diffs.append(float(np.abs(f - prev).mean()))
    prev = f
print(f"  tx over the shot: {txs[0]:+.4f} -> {txs[N//4]:+.4f} -> {txs[N//2]:+.4f} "
      f"-> {txs[3*N//4]:+.4f} -> {txs[-1]:+.4f}")
print(f"  mean |frame-to-frame|: {np.mean(diffs):.5f}   max {max(diffs):.5f}   "
      f"first {diffs[0]:.5f}   last {diffs[-1]:.5f}")
print(f"  holes: min {min(hole_counts)} max {max(hole_counts)}")

print("\n=== sample frames ===")
for g in GAMES:
    shot = D.DepthShot(g, W, H, crop_bias=chosen[g])
    for label, p in [("start", 0.0), ("left", 0.25), ("right", 0.75)]:
        tx, ty, tz = shot.camera_path(p)
        f, h = shot.render(tx, ty, tz)
        save(OUT / f"{g}_{label}.png", f)
    save(OUT / f"{g}_depth.png", shot.depth_colour())
    # A motion map: bright where the picture moved a lot between eye positions,
    # dark where it barely moved. That is the depth structure made visible.
    a, _ = shot.render(-0.016, 0.0, 0.05)
    b, _ = shot.render(0.016, 0.0, 0.05)
    diff = np.abs(a - b).mean(axis=2)
    cv2.imwrite(str(OUT / f"{g}_motion.png"),
                cv2.applyColorMap((np.clip(diff * 5, 0, 1) * 255).astype(np.uint8),
                                  cv2.COLORMAP_INFERNO))
    print(f"  {g}: start/left/right, depth and motion maps")

print(f"\nchosen crop biases: {chosen}")
