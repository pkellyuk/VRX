"""Render the VRX promo: silent masters, the score, the muxed deliverables.

    python promo/film/render_film.py            # everything
    python promo/film/render_film.py wide       # just the 16:9 master
    python promo/film/render_film.py tall       # just the 9:16 cut
    python promo/film/render_film.py poster     # just the poster frames

Writes to promo/:
    VRX-promo-1080p.mp4              1920x1080, H.264, AAC
    VRX-promo-vertical-1080x1920.mp4 1080x1920, H.264, AAC
    VRX-promo-poster.png             a still from the master
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_brand as B  # noqa: E402
from film import vrx_film as F  # noqa: E402
from film import vrx_score as SCORE  # noqa: E402
from film import vrxmedia as M  # noqa: E402
from film.vrx_shotlist import FPS, TOTAL_SECONDS  # noqa: E402

PROMO = B.PROMO
BUILD = M.ensure_build_dir()

WIDE = "VRX-promo-1080p.mp4"
TALL = "VRX-promo-vertical-1080x1920.mp4"
POSTER = "VRX-promo-poster.png"

# Frames worth keeping as a still. The first is the default.
POSTER_TIMES = [13.5, 22.0, 30.0, 9.0]


def render_master(width: int, height: int, vertical: bool, silent_out: Path,
                  progress_every: int = 120) -> Path:
    """Render every frame, then read the encoded file back before trusting it."""
    label = "9:16" if vertical else "16:9"
    film = F.Film(width, height, vertical=vertical)
    total = int(round(TOTAL_SECONDS * FPS))
    state = {"t0": time.perf_counter(), "n": 0}

    def frames():
        for n in range(total):
            frame = film.frame_at(n / FPS)
            yield (np.clip(frame, 0, 1) * 255.0 + 0.5).astype(np.uint8)
            state["n"] = n + 1
            if n % progress_every == 0 or n == total - 1:
                done = n + 1
                rate = done / max(time.perf_counter() - state["t0"], 1e-6)
                left = (total - done) / max(rate, 1e-6)
                print(f"  [{label}] {done:4d}/{total} frames  "
                      f"{rate:5.1f} fps  {left:5.0f}s left", flush=True)

    return M.encode_verified(silent_out, width, height, FPS, frames, total,
                             crf=16, preset="slow")


def render_poster(width: int, height: int, vertical: bool, out: Path,
                  times: list[float]) -> Path:
    film = F.Film(width, height, vertical=vertical)
    frames = []
    for t in times:
        f = np.clip(film.frame_at(t), 0, 1)
        frames.append((np.clip(f, 0, 1) * 255.0 + 0.5).astype(np.uint8))
    out.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out), cv2.cvtColor(frames[0], cv2.COLOR_RGB2BGR))
    for i, f in enumerate(frames[1:], start=1):
        alt = out.with_name(f"{out.stem}-alt{i}.png")
        cv2.imwrite(str(alt), cv2.cvtColor(f, cv2.COLOR_RGB2BGR))
        print(f"  wrote alternate still {alt.name}")
    return out


def mux_master(silent: Path, out: Path, score_wav: Path) -> Path:
    M.mux(silent, score_wav, out)
    return out


def main(argv: list[str]) -> int:
    wanted = set(argv[1:]) or {"wide", "tall", "poster"}

    score_wav = BUILD / "score.wav"
    if not score_wav.exists() or "score" in wanted:
        print("building the score...")
        M.write_wav(score_wav, SCORE.build(), SCORE.RATE)
        print(f"  wrote {score_wav}")

    made: list[Path] = []

    if "wide" in wanted:
        print("\n=== 16:9 master, 1920x1080 ===")
        silent = render_master(1920, 1080, False, BUILD / "wide_silent.mp4")
        print("  muxing the score...")
        out = PROMO / WIDE
        mux_master(silent, out, score_wav)
        made.append(out)

    if "tall" in wanted:
        print("\n=== 9:16 social cut, 1080x1920 ===")
        silent = render_master(1080, 1920, True, BUILD / "tall_silent.mp4")
        print("  muxing the score...")
        out = PROMO / TALL
        mux_master(silent, out, score_wav)
        made.append(out)

    if "poster" in wanted:
        print("\n=== poster stills ===")
        out = PROMO / POSTER
        render_poster(1920, 1080, False, out, POSTER_TIMES)
        made.append(out)

    print("\n=== deliverables ===")
    for p in made:
        print(f"  {p}  {p.stat().st_size/1e6:.2f} MB")
        if p.suffix == ".mp4":
            for k, v in M.video_summary(p).items():
                print(f"      {k:14s} {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
