"""Extract frames from a finished deliverable and lay them out as a contact sheet.

Reviewing frames pulled back out of the encoded file, rather than re-rendered
ones, means the review is of exactly what ships.

    python promo/tools/review_sheet.py                        # the 16:9 master
    python promo/tools/review_sheet.py --vertical
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

PROMO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROMO))
from film import vrxmedia as M  # noqa: E402

OUT = M.ensure_build_dir() / "sheets"
TIMES = [1.2, 3.1, 5.2, 6.8, 8.6, 11.0, 13.4, 15.6, 17.4, 19.5, 23.5, 26.5,
         29.5, 31.5, 33.5, 36.0]


def grab(video: Path, times: list[float]) -> list[tuple[str, np.ndarray]]:
    out = []
    for t in times:
        res = subprocess.run(
            [M.FFMPEG, "-v", "error", "-ss", f"{t:.3f}", "-i", str(video),
             "-frames:v", "1", "-f", "image2pipe", "-vcodec", "png", "-"],
            capture_output=True)
        if res.returncode != 0 or not res.stdout:
            print(f"  frame at {t}s failed: {res.stderr[:200]}")
            continue
        buf = np.frombuffer(res.stdout, np.uint8)
        img = cv2.imdecode(buf, cv2.IMREAD_COLOR)
        if img is None:
            print(f"  frame at {t}s would not decode")
            continue
        out.append((f"{t:.1f}s", img))
    return out


def sheet(frames, cols: int, tile_w: int, path: Path, title: str) -> None:
    tiles = []
    for label, img in frames:
        h, w = img.shape[:2]
        th = int(tile_w * h / w)
        small = cv2.resize(img, (tile_w, th), interpolation=cv2.INTER_AREA)
        cv2.putText(small, label, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (255, 255, 255), 1, cv2.LINE_AA)
        tiles.append(small)
    rows = []
    for i in range(0, len(tiles), cols):
        chunk = tiles[i:i + cols]
        while len(chunk) < cols:
            chunk.append(np.zeros_like(chunk[0]))
        rows.append(np.hstack(chunk[:cols]))
    grid = np.vstack(rows)
    grid = cv2.copyMakeBorder(grid, 32, 6, 6, 6, cv2.BORDER_CONSTANT, value=(18, 14, 12))
    cv2.putText(grid, title, (10, 23), cv2.FONT_HERSHEY_SIMPLEX, 0.68,
                (240, 232, 62), 2, cv2.LINE_AA)
    cv2.imwrite(str(path), grid)
    print("wrote", path, grid.shape)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vertical", action="store_true")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    name = "VRX-promo-vertical-1080x1920.mp4" if args.vertical else "VRX-promo-1080p.mp4"
    video = PROMO / name
    if not video.exists():
        print(f"missing {video}")
        return 1
    frames = grab(video, TIMES)
    print(f"{len(frames)} frames pulled from {name}")
    if args.vertical:
        sheet(frames, 4, 260, OUT / "final_tall.png",
              "VRX promo 1080x1920 - frames from the delivered file")
    else:
        sheet(frames, 4, 470, OUT / "final_wide.png",
              "VRX promo 1920x1080 - frames from the delivered file")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
