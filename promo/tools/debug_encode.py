"""Reproduce the corrupt wide encode under controlled conditions.

Tries the same pipe-into-ffmpeg approach three ways and probes each result:
  1. as the render does it now (write, close, read stderr, wait)
  2. draining stderr on a thread while writing
  3. writing to a temporary file first, then renaming

The point is to find out whether the corruption is the pipe protocol, the
stderr pipe filling up, or something outside this process.
"""
from __future__ import annotations

import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrxmedia as M  # noqa: E402

W, H, FPS, FRAMES = 1920, 1080, 30, 240
OUT = M.ensure_build_dir() / "encode_repro"
OUT.mkdir(parents=True, exist_ok=True)


def frames():
    """Frames that are cheap to make but hard to encode, like the film's."""
    rng = np.random.default_rng(7)
    base = rng.integers(0, 255, (H, W, 3), dtype=np.uint8)
    for n in range(FRAMES):
        # A moving element forces real inter-frame prediction.
        f = base.copy()
        x = (n * 7) % (W - 200)
        f[H // 3:H // 3 + 120, x:x + 200] = 240
        yield f


def probe(path: Path) -> str:
    res = M.run([M.FFPROBE, "-v", "error", "-show_entries",
                 "stream=codec_type,pix_fmt,nb_frames,duration",
                 "-of", "default=noprint_wrappers=1", str(path)])
    err = M.run([M.FFMPEG, "-v", "error", "-i", str(path), "-f", "null", "-"])
    noise = (err.stderr or "").strip()
    return (f"probe rc={res.returncode} {(res.stdout or '').splitlines()}"
            f" | decode rc={err.returncode} noise={len(noise)} chars")


def attempt(label: str, mode: str) -> None:
    path = OUT / f"{label}.mp4"
    args = [
        M.FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24",
        "-s", f"{W}x{H}", "-r", str(FPS), "-i", "pipe:0", "-an",
        "-c:v", "libx264", "-preset", "slow", "-crf", "16",
        "-pix_fmt", "yuv420p", "-profile:v", "high", "-level", "4.1",
        "-movflags", "+faststart", str(path),
    ]
    t0 = time.perf_counter()
    proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    drained = []

    if mode == "thread":
        def pump():
            for line in proc.stderr:
                drained.append(line)
        th = threading.Thread(target=pump, daemon=True)
        th.start()

    assert proc.stdin
    for f in frames():
        proc.stdin.write(f.tobytes())
    proc.stdin.close()

    if mode == "thread":
        proc.wait()
        th.join(timeout=5)
        err = b"".join(drained).decode("utf-8", "replace")
    else:
        err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
        proc.wait()

    dt = time.perf_counter() - t0
    print(f"{label:16s} rc={proc.returncode} {dt:5.1f}s stderr={len(err)} chars")
    if err.strip():
        print("   stderr:", err.strip()[:200])
    print("   ", probe(path))


print(f"ffmpeg: {M.FFMPEG}")
print(f"frames: {FRAMES} at {W}x{H}\n")
attempt("pipe_current", "current")
attempt("pipe_thread", "thread")
print("\ndone")
