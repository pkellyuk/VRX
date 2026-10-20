"""Prove the render toolchain: 1080p libx264 plus AAC, from raw numpy frames.

Renders a moving gradient with a moving cyan sweep and a 440 Hz tone, encodes
video and audio, muxes them, and reports what the result actually contains.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrxmedia as M  # noqa: E402

W, H, FPS, SECONDS = 1920, 1080, 30, 2
BUILD = M.ensure_build_dir()


def frames():
    """RGB frames, matching the convention every film module works in."""
    xs = np.linspace(0, 1, W, dtype=np.float32)[None, :]   # 1 x W
    ys = np.linspace(0, 1, H, dtype=np.float32)[:, None]   # H x 1
    for n in range(FPS * SECONDS):
        t = n / FPS
        base = np.zeros((H, W, 3), np.float32)
        base[:, :, 2] = 40 + 120 * xs          # blue ramps across
        base[:, :, 1] = 30 + 90 * ys           # green ramps down
        base[:, :, 0] = 90 + 60 * np.sin(6.0 * xs + 2.0 * t)   # red ripples
        x = int((W - 8) * (0.5 + 0.5 * np.sin(t * 2.2)))
        base[:, x:x + 8] = (74, 232, 240)      # the brand cyan
        # A crisp checker corner proves there is no chroma smear or scaling.
        if (n // 5) % 2 == 0:
            base[0:120, 0:120] = 235
        yield np.clip(base, 0, 255).astype(np.uint8)


def score():
    n = np.arange(FPS * SECONDS * 48000) / 48000
    left = 0.35 * np.sin(2 * np.pi * 440 * n)
    right = 0.35 * np.sin(2 * np.pi * 440 * n + 0.4)
    return np.stack([left, right], axis=1)


silent = BUILD / "toolchain_test_silent.mp4"
score_wav = BUILD / "toolchain_test.wav"
master = BUILD / "toolchain_test.mp4"

with M.FrameWriter(silent, W, H, FPS, crf=18) as w:
    for f in frames():
        w.write(f)

M.write_wav(score_wav, score(), 48000)
M.mux(silent, score_wav, master)

summary = M.video_summary(master)
print("silent master :", silent.name, silent.stat().st_size, "bytes")
print("muxed master  :", master.name, master.stat().st_size, "bytes")
for k, v in summary.items():
    print(f"  {k:14s} {v}")

ok = (summary["width"], summary["height"]) == (W, H)
ok &= summary["vcodec"] == "h264" and summary["acodec"] == "aac"
ok &= summary["achannels"] == 2 and summary["asample_rate"] == 48000
ok &= abs(summary["duration"] - SECONDS) < 0.4
print("\nTOOLCHAIN OK" if ok else "\nTOOLCHAIN PROBLEM")
sys.exit(0 if ok else 1)
