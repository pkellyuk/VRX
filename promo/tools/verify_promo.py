"""Verify the VRX promo deliverables.

    python promo/tools/verify_promo.py                 # every check, all criteria
    python promo/tools/verify_promo.py --vertical      # just the 9:16 cut
    python promo/tools/verify_promo.py --poster        # just the poster still
    python promo/tools/verify_promo.py --reproducible  # golden-frame render check
    python promo/tools/verify_promo.py --write-reference
                                                       # record the golden frames

Exit code 0 means every check that ran passed. Each check prints what it saw, so
a failure says which property was wrong rather than just that something was.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

PROMO = Path(__file__).resolve().parents[1]
REPO = PROMO.parent
sys.path.insert(0, str(PROMO))

from film import vrxmedia as M  # noqa: E402
from film.vrx_shotlist import FPS, SHOTS, TOTAL_SECONDS  # noqa: E402

WIDE = PROMO / "VRX-promo-1080p.mp4"
TALL = PROMO / "VRX-promo-vertical-1080x1920.mp4"
POSTER = PROMO / "VRX-promo-poster.png"
REFERENCE = PROMO / "film" / "frame_hashes.json"
REFERENCE_FRAMES = [0, 150, 405, 630, 900, 1199]

failures: list[str] = []


def ok(label: str, message: str) -> None:
    print(f"  PASS  {label:14s} {message}")


def bad(label: str, message: str) -> None:
    print(f"  FAIL  {label:14s} {message}")
    failures.append(f"{label}: {message}")


def decodes_cleanly(path: Path) -> tuple[bool, str]:
    """Decode the whole file and report anything ffmpeg complains about."""
    res = subprocess.run(
        [M.FFMPEG, "-v", "error", "-i", str(path), "-f", "null", "-"],
        capture_output=True, text=True)
    noise = (res.stderr or "").strip()
    return (res.returncode == 0 and not noise), noise[:400]


def check_master(path: Path, width: int, height: int, label: str) -> None:
    if not path.exists():
        bad(label, f"{path.name} does not exist")
        return
    info = M.video_summary(path)
    checks = [
        ("size", (info["width"], info["height"]) == (width, height),
         f"{info['width']}x{info['height']} (want {width}x{height})"),
        ("video codec", info["vcodec"] == "h264", info["vcodec"]),
        ("pixel format", info["pix_fmt"] == "yuv420p", str(info["pix_fmt"])),
        ("audio codec", info["acodec"] == "aac", str(info["acodec"])),
        ("audio channels", info["achannels"] == 2, str(info["achannels"])),
        ("audio rate", info["asample_rate"] == 48000, str(info["asample_rate"])),
        ("duration", 30.0 <= info["duration"] <= 45.0, f"{info['duration']:.2f}s"),
        ("frame rate", abs(info["fps"] - FPS) < 0.01, f"{info['fps']} fps"),
    ]
    for name, passed, detail in checks:
        if passed:
            ok(f"{label} {name}", detail)
        else:
            bad(f"{label} {name}", detail)

    clean, noise = decodes_cleanly(path)
    if clean:
        ok(f"{label} decode", "no errors from ffmpeg over the whole file")
    else:
        bad(f"{label} decode", noise)

    # Every frame must actually be there. A truncated encode still has a
    # plausible header, so count what ffmpeg can really read out.
    expected = int(round(TOTAL_SECONDS * FPS))
    counted = subprocess.run(
        [M.FFPROBE, "-v", "error", "-count_frames", "-select_streams", "v:0",
         "-show_entries", "stream=nb_read_frames,pix_fmt,profile",
         "-of", "default=noprint_wrappers=1", str(path)],
        capture_output=True, text=True)
    fields = dict(
        line.split("=", 1) for line in (counted.stdout or "").splitlines() if "=" in line
    )
    read = fields.get("nb_read_frames", "")
    if read.isdigit() and abs(int(read) - expected) <= 2:
        ok(f"{label} frames", f"{read} frames decoded (want {expected})")
    else:
        bad(f"{label} frames", f"decoded {read or '?'} frames, want {expected}")
    for field in ("pix_fmt", "profile"):
        value = fields.get(field, "")
        if value and value != "unknown":
            ok(f"{label} {field}", value)
        else:
            bad(f"{label} {field}", f"unreadable ({value or 'missing'})")

    # The audio must not be silence.
    raw = subprocess.run(
        [M.FFMPEG, "-v", "error", "-i", str(path), "-map", "0:a:0",
         "-f", "s16le", "-ac", "1", "-ar", "8000", "-"],
        capture_output=True)
    samples = np.frombuffer(raw.stdout, dtype="<i2").astype(np.float32) / 32768.0
    if len(samples) < 8000:
        bad(f"{label} audio", "too few samples to measure")
        return
    rms = float(np.sqrt(np.mean(samples ** 2)))
    peak = float(np.abs(samples).max())
    if rms > 0.005 and peak > 0.05:
        ok(f"{label} audio", f"rms {20*np.log10(rms):.1f} dBFS, "
                             f"peak {20*np.log10(peak):.1f} dBFS")
    else:
        bad(f"{label} audio", f"looks silent (rms {rms:.5f}, peak {peak:.5f})")


def check_default() -> None:
    print("=== C1: 1920x1080 master with an AAC stereo soundtrack ===")
    check_master(WIDE, 1920, 1080, "master")


def check_vertical() -> None:
    print("=== C2: 1080x1920 vertical social cut ===")
    check_master(TALL, 1080, 1920, "vertical")

    # No letterboxing: the master composition must not simply sit in the middle
    # of the tall frame with black bars. Sample the top and bottom bands.
    if TALL.exists():
        import cv2
        cap = cv2.VideoCapture(str(TALL))
        bars = 0
        checked = 0
        for i in range(6):
            cap.set(cv2.CAP_PROP_POS_FRAMES, int(TOTAL_SECONDS * FPS * (0.2 + 0.12 * i)))
            got, frame = cap.read()
            if not got:
                continue
            checked += 1
            top = frame[:60].mean()
            bottom = frame[-60:].mean()
            if top < 4 and bottom < 4:
                bars += 1
        cap.release()
        if checked and bars == 0:
            ok("vertical bars", f"no black bars top and bottom in {checked} sampled frames")
        else:
            bad("vertical bars", f"{bars}/{checked} sampled frames are letterboxed")


def check_poster() -> None:
    print("=== C3: poster still ===")
    if not POSTER.exists():
        bad("poster", f"{POSTER.name} does not exist")
        return
    import cv2
    img = cv2.imread(str(POSTER))
    if img is None:
        bad("poster", "cannot be read as an image")
        return
    h, w = img.shape[:2]
    if (w, h) == (1920, 1080):
        ok("poster", f"{w}x{h}, {POSTER.stat().st_size/1e6:.2f} MB")
    else:
        bad("poster", f"{w}x{h} (want 1920x1080)")
    if float(img.mean()) > 6.0:
        ok("poster", f"has picture (mean level {img.mean():.1f})")
    else:
        bad("poster", "looks black")


def frame_digest(frame: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(frame).tobytes()).hexdigest()[:32]


def render_reference() -> dict:
    from film import vrx_film as F
    film = F.Film(1920, 1080, vertical=False)
    out = {}
    for n in REFERENCE_FRAMES:
        frame = film.frame_at(n / FPS)
        img = (np.clip(frame, 0, 1) * 255.0 + 0.5).astype(np.uint8)
        out[str(n)] = frame_digest(img)
    return out


def check_reproducible(write_reference: bool) -> None:
    print("=== C5: the film rebuilds from the committed assets ===")
    needed = [
        PROMO / "film" / "render_film.py",
        PROMO / "film" / "vrx_film.py",
        PROMO / "film" / "vrx_dibr.py",
        PROMO / "film" / "vrx_score.py",
        PROMO / "film" / "vrx_shotlist.py",
        PROMO / "film" / "vrx_draw.py",
        PROMO / "film" / "vrx_brand.py",
        PROMO / "README.md",
    ]
    missing = [p.name for p in needed if not p.exists()]
    if missing:
        bad("reproducible", f"missing build files: {missing}")
        return
    ok("reproducible", "every build script is present")

    for shot in SHOTS:
        for asset in shot.assets:
            p = REPO / asset
            if not p.exists():
                bad("reproducible", f"shot {shot.key} needs {asset}, which is missing")
                return
    ok("reproducible", f"all {sum(len(s.assets) for s in SHOTS)} referenced assets exist")

    digests = render_reference()
    if write_reference:
        REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        REFERENCE.write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
        ok("reproducible", f"recorded {len(digests)} golden frames in {REFERENCE.name}")
        return

    if not REFERENCE.exists():
        bad("reproducible", f"{REFERENCE.name} has not been recorded yet")
        return
    want = json.loads(REFERENCE.read_text(encoding="utf-8"))
    differ = [k for k, v in digests.items() if want.get(k) != v]
    if not differ:
        ok("reproducible", f"{len(digests)} golden frames render identically "
                           f"to the recorded build")
    else:
        bad("reproducible", f"frames {differ} no longer match the recorded build")


def check_colour() -> None:
    """The finished files must carry the film's colours, not a channel-swapped copy.

    Nothing else catches this: declaring the raw pipe as bgr24 while feeding it
    RGB produces a file that is valid, the right size, the right length and
    decodes cleanly, with red and blue exchanged throughout. So compare frames
    decoded from the delivered file against freshly rendered ones, and confirm
    the test is actually sensitive by checking the swapped version scores worse.
    """
    print("=== colour order: the files must match the renderer ===")
    import cv2
    from film import vrx_film as F

    for path, vertical in ((WIDE, False), (TALL, True)):
        if not path.exists():
            bad("colour", f"{path.name} is missing")
            continue
        width, height = (1080, 1920) if vertical else (1920, 1080)
        film = F.Film(width, height, vertical=vertical)
        worst, worst_swap = 0.0, 0.0
        for n in (156, 630, 1035):
            want = (np.clip(film.frame_at(n / FPS), 0, 1) * 255.0)
            res = subprocess.run(
                [M.FFMPEG, "-v", "error", "-ss", f"{n / FPS:.4f}", "-i", str(path),
                 "-frames:v", "1", "-f", "image2pipe", "-vcodec", "png", "-"],
                capture_output=True)
            got = cv2.imdecode(np.frombuffer(res.stdout, np.uint8), cv2.IMREAD_COLOR)
            if got is None:
                bad("colour", f"could not decode frame {n} from {path.name}")
                break
            have = got[:, :, ::-1].astype(np.float32)      # decoded BGR -> RGB

            def diff(a, b, size=(96, 54)):
                return float(np.abs(cv2.resize(a, size, interpolation=cv2.INTER_AREA)
                                    - cv2.resize(b, size, interpolation=cv2.INTER_AREA)).mean())

            # Tolerate a one-frame seek difference.
            candidates = [diff(have, want)]
            for shift in (-1, 1):
                alt = (np.clip(film.frame_at((n + shift) / FPS), 0, 1) * 255.0)
                candidates.append(diff(have, alt))
            best = min(candidates)
            swapped = diff(have, want[:, :, ::-1])
            worst = max(worst, best)
            worst_swap = max(worst_swap, swapped)
        else:
            if worst < 3.0:
                ok("colour", f"{path.name}: frames match the renderer "
                             f"(worst {worst:.2f}/255; a swap would score "
                             f"{worst_swap:.1f})")
            else:
                bad("colour", f"{path.name} differs from the renderer by "
                              f"{worst:.2f}/255")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vertical", action="store_true")
    ap.add_argument("--colour", action="store_true",
                    help="check the delivered file's colours against the renderer")
    ap.add_argument("--poster", action="store_true")
    ap.add_argument("--reproducible", action="store_true")
    ap.add_argument("--write-reference", action="store_true",
                    help="record the golden frames instead of comparing")
    args = ap.parse_args()

    only = (args.vertical or args.poster or args.reproducible
            or args.write_reference or args.colour)
    if not only:
        check_default()
        check_colour()
        check_vertical()
        check_poster()
        check_reproducible(False)
    else:
        if args.colour:
            check_colour()
        if args.vertical:
            check_vertical()
        if args.poster:
            check_poster()
        if args.reproducible or args.write_reference:
            check_reproducible(args.write_reference)

    print()
    if failures:
        print(f"{len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
