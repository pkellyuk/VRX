"""Two-model depth fusion (xmmodel), step 4: side-by-side 3D videos for the headset.

Renders the same clip with several depth variants through VRX's own stereo warp
(native/xmmodel/out/sbs_render.exe, a CPU port of xrapp5.cpp kWarpHlsl), so they can
be compared in any VR video player that plays side-by-side 3D.

usage (from bench/):
    .venv-dml/Scripts/python.exe xmmodel/xm_sbs.py <clip_dir> [--lat=27] [--sigma=16]
        [--height=1080] [--strength=1]

Needs <clip_dir>/me_<lat>/vectors.bin and <clip_dir>/me_prev/vectors.bin (xm_fuse.py
--export-mv / --export-prev, then me_probe.exe --pairs). Writes, to xmmodel/out/sbs/:
    <clip>_<n>_<variant>_SBS_LR.mp4   one file per variant, its name burned into both eyes
    <clip>_ALL_SBS_LR.mp4             the variants back to back, in the same order
Each file is full side-by-side: left eye in the left half, right eye in the right.

As in the engine: the near map is dilated (DilateNear, ZipDepth's 3/3 radius) before
warping; the screen is VRX's default 5.7 m wide at 3 m; IPD 63 mm. A player usually
shows a smaller virtual screen than VRX's, so absolute depth looks weaker there - the
comparison between variants is what matters.
"""
import os
import subprocess
import sys
import time

import cv2
import imageio_ffmpeg
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from xm_cache import GH, GW  # noqa: E402
import xm_fuse  # noqa: E402

RENDER = os.path.join(HERE, '..', 'native', 'xmmodel', 'out', 'sbs_render.exe')
DILATE = 3                                     # kZipDepth dilateH / dilateV (xrapp5.cpp)


def log(msg):
    print(f'[{time.strftime("%H:%M:%S")}][xm_sbs] {msg}', flush=True)


def variants(sigma):
    """(key in simulate() output, label) in the order they are shown."""
    return [('zip', 'A: ZipDepth alone (today)'),
            ('zip_stab', 'B: ZipDepth steadied by motion vectors'),
            (f'mc_s{sigma}_hwv', 'C: Fused with Depth Anything V2'),
            (f'mc_s{sigma}_hwv_stab', 'D: Fused + steadied')]


def collect_depth(clip, clip_dir, lat, sigma):
    """Runs the simulation once and keeps every variant's near map per frame."""
    log(f'collect_depth: enter lat={lat} sigma={sigma}')
    hw = xm_fuse.load_hw_mv(clip_dir, lat)
    prev = xm_fuse.load_hw_mv(clip_dir, None)
    if hw is None or prev is None:
        raise FileNotFoundError('motion vectors missing: run xm_fuse.py --export-mv / --export-prev and me_probe.exe --pairs')
    keys = [k for k, _ in variants(sigma)]
    maps, frames = {k: {} for k in keys}, []

    def sink(i, out, _conf):
        frames.append(i)
        for k in keys:
            maps[k][i] = out[k].copy()

    xm_fuse.simulate(clip, lat, [sigma], sink, hw_mv=hw, prev_mv=prev)
    log(f'collect_depth: exit {len(frames)} frames (from frame {frames[0]})')
    return maps, frames


def colour_frames(meta, size):
    """The clip's frames again, at viewing resolution, in the same order as the cache."""
    cap = cv2.VideoCapture(meta['video'])
    if not cap.isOpened():
        raise RuntimeError(f'cannot open {meta["video"]}')
    start, end = meta['start'], meta['start'] + meta['seconds']
    cap.set(cv2.CAP_PROP_POS_MSEC, start * 1000.0)
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        t = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        if t < start - 1e-3:
            continue
        if t >= end:
            break
        if (bgr.shape[1], bgr.shape[0]) != size:
            bgr = cv2.resize(bgr, size, interpolation=cv2.INTER_AREA)
        yield cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    cap.release()


def burn_label(sbs, text, cw):
    """The same label at the same place in both eyes, so it sits at screen depth."""
    (tw, _), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.9, 2)
    for x0 in (0, cw):
        cv2.rectangle(sbs, (x0 + 30, 30), (x0 + 30 + tw + 24, 78), (0, 0, 0), -1)
        cv2.putText(sbs, text, (x0 + 42, 64), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2, cv2.LINE_AA)


def render_variant(clip, maps, frames, key, label, size, fps, strength, path):
    log(f'render_variant: enter {key} -> {path}')
    cw, ch = size
    ren = subprocess.Popen([RENDER, f'--size={cw}x{ch}', f'--strength={strength}'],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    enc = subprocess.Popen([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'rawvideo',
                            '-pix_fmt', 'rgb24', '-s', f'{cw * 2}x{ch}', '-r', f'{fps:.3f}', '-i', '-',
                            '-c:v', 'libx264', '-preset', 'medium', '-crf', '17', '-pix_fmt', 'yuv420p',
                            '-movflags', '+faststart', path], stdin=subprocess.PIPE)
    kernel = np.ones((2 * DILATE + 1, 2 * DILATE + 1), np.uint8)
    wanted, written = set(frames), 0
    sbs_bytes = cw * 2 * ch * 3
    for i, rgb in enumerate(colour_frames(clip['meta'], size)):
        if i not in wanted:
            continue
        near = cv2.dilate(maps[key][i], kernel).astype(np.float32)     # = DilateNear(3, 3)
        ren.stdin.write(np.ascontiguousarray(rgb).tobytes())
        ren.stdin.write(np.ascontiguousarray(near).tobytes())
        ren.stdin.flush()
        buf = ren.stdout.read(sbs_bytes)
        if len(buf) != sbs_bytes:
            raise RuntimeError(f'sbs_render returned {len(buf)} bytes at frame {i}')
        sbs = np.frombuffer(buf, np.uint8).reshape(ch, cw * 2, 3).copy()
        burn_label(sbs, label, cw)
        enc.stdin.write(sbs.tobytes())
        written += 1
    ren.stdin.close()
    ren.wait()
    enc.stdin.close()
    enc.wait()
    if written != len(frames):
        raise RuntimeError(f'{written} frames rendered, expected {len(frames)} - colour decode out of step with the cache')
    log(f'render_variant: exit {written} frames')


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    clip_dir = argv[1]
    opts = dict(a[2:].split('=', 1) for a in argv[2:] if a.startswith('--') and '=' in a)
    lat = float(opts.get('lat', 27))
    sigma = int(opts.get('sigma', 16))
    height = int(opts.get('height', 1080))
    strength = float(opts.get('strength', 1))
    log(f'main: enter {clip_dir} lat={lat} sigma={sigma} height={height} strength={strength}')
    if not os.path.isfile(RENDER):
        raise FileNotFoundError(f'{RENDER} missing: run native/xmmodel/build.bat')

    clip = xm_fuse.load_clip(clip_dir)
    probe = cv2.VideoCapture(clip['meta']['video'])
    sw, sh = int(probe.get(cv2.CAP_PROP_FRAME_WIDTH)), int(probe.get(cv2.CAP_PROP_FRAME_HEIGHT))
    probe.release()
    size = (int(round(sw * height / sh / 2)) * 2, height)
    maps, frames = collect_depth(clip, clip_dir, lat, sigma)

    out_dir = os.path.join(HERE, 'out', 'sbs')
    os.makedirs(out_dir, exist_ok=True)
    name = os.path.basename(os.path.normpath(clip_dir))
    parts = []
    for n, (key, label) in enumerate(variants(sigma)):
        path = os.path.join(out_dir, f'{name}_{n + 1}_{key}_SBS_LR.mp4')
        render_variant(clip, maps, frames, key, label, size, clip['fps'], strength, path)
        parts.append(path)

    listing = os.path.join(out_dir, f'{name}_concat.txt')
    with open(listing, 'w') as f:
        f.writelines(f"file '{os.path.abspath(p)}'\n" for p in parts)
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'concat', '-safe', '0',
                    '-i', listing, '-c', 'copy', os.path.join(out_dir, f'{name}_ALL_SBS_LR.mp4')], check=True)
    os.remove(listing)
    log(f'main: exit {len(parts)} variants + combined file in {out_dir}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
