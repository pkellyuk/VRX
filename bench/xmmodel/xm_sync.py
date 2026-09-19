"""Game-frame / depth synchronisation on one GPU (xsync): score and render the options.

ZipDepth on a busy single GPU: each depth result is ready `lat` ms after its frame, and
the model runs back to back. Per displayed frame (at each captured frame's time):

  A latest    newest game frame + newest finished depth (VRX's default)
  B synced    "Match game frames to depth": the frame the newest depth came from,
              with that depth. Exact alignment, but the game image is older and only
              updates at the depth rate.
  C every Nth the game stream delayed by the depth latency, so each depth result is
              exactly synced to one frame and the frames between reuse it (depth
              every 2-3 frames = "synced every other / third frame")
  D moved     newest game frame + newest depth moved forward to it by the hardware
              motion vectors (me_<lat>), where the motion is verified
              (xm_fuse.motion_trust); elsewhere the unmoved depth, as A

Scores (lower is better except edge; ages in ms):
  align       scale/shift-invariant mean abs error of the depth shown against the depth
              of the game frame shown (0 = perfectly synced)
  align_near  the same where that depth is near (> 0.6)
  edge        strongest depth edges within 2 px of an edge in the game frame shown
  flicker     motion-compensated change between consecutive displayed depth maps
  image_age   how old the game image is when shown
  updates/s   how often the game image changes

usage (from bench/):
    .venv-dml/Scripts/python.exe xmmodel/xm_sync.py <clip_dir> [--lat=33,67] [--render-lat=67] [--no-video]
Needs <clip_dir>/me_<lat>/vectors.bin (xm_fuse.py --export-mv=LAT, then me_probe --pairs).
Writes <clip_dir>/sync_results.json and xmmodel/out/sbs/<clip>_sync_*.mp4.
"""
import json
import os
import subprocess
import sys
import time

import cv2
import imageio_ffmpeg
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xm_fuse  # noqa: E402
import xm_sbs  # noqa: E402

VARIANTS = [('A', 'A: Latest depth (today)'), ('B', 'B: Synced to depth (frame matching)'),
            ('C', 'C: Synced every Nth frame (delayed)'), ('D', 'D: Depth moved to every frame')]


def log(msg):
    print(f'[{time.strftime("%H:%M:%S")}][xm_sync] {msg}', flush=True)


def near_all(clip):
    """ZipDepth near maps for every frame with the engine's smoothed range (the depth
    each frame would have if it were synced)."""
    sm = xm_fuse.RangeSmoother(xm_fuse.TAU_RANGE)
    out = []
    for i, raw in enumerate(clip['zip']):
        lo, hi = sm.update(np.asarray(raw), float(clip['times'][i]))
        out.append(xm_fuse.to_near(np.asarray(raw), lo, hi))
    return out


def plan(times, lat_ms):
    """Per display index i: (colour frame, depth frame) for each variant, or None
    before that variant has anything to show."""
    events = xm_fuse.anchor_schedule(times, lat_ms)
    shown = []
    for i, t in enumerate(times):
        done = [k for arrival, k in events if arrival <= float(t) + 1e-9]
        if not done:
            shown.append(None)
            continue
        k = max(done)
        # C: the game stream delayed by the depth latency; its depth is the newest
        # finished result for a frame at or before the (delayed) frame shown.
        c = int(np.searchsorted(times, float(t) - lat_ms / 1000.0 + 1e-9, side='right')) - 1
        kc = max([kk for kk in done if kk <= c], default=None) if c >= 0 else None
        if kc is None:
            shown.append(None)
            continue
        shown.append({'A': (i, k), 'B': (k, k), 'C': (c, kc), 'D': (i, k)})
    return shown


def simulate(clip, clip_dir, lat_ms, frame_sink=None):
    log(f'simulate: enter lat={lat_ms}')
    times, rgb = clip['times'], clip['rgb']
    near = near_all(clip)
    hw = xm_fuse.load_hw_mv(clip_dir, lat_ms)
    if hw is None:
        raise FileNotFoundError(f'no hardware vectors for {lat_ms} ms: run xm_fuse.py --export-mv={int(lat_ms)} and me_probe --pairs')
    grey = [cv2.cvtColor(np.asarray(f), cv2.COLOR_RGB2GRAY) for f in rgb]
    shown = plan(times, lat_ms)
    first = next(i for i, s in enumerate(shown) if s is not None)
    scores = {v: {'align': [], 'align_near': [], 'edge': [], 'flicker': [], 'age': []} for v, _ in VARIANTS}
    changes = {v: 0 for v, _ in VARIANTS}
    prev = {}
    flows = {}
    trust_sum, trust_n = 0.0, 0
    for i in range(first, len(times)):
        s = shown[i]
        if s is None:
            continue
        maps = {}
        for v, _ in VARIANTS:
            c, k = s[v]
            if v == 'D' and k != c:
                mx, my = hw[(c, k)]
                moved = cv2.remap(near[k], mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                trust = xm_fuse.motion_trust(grey[c], grey[k], mx, my)
                trust_sum += float(trust.mean()); trust_n += 1
                maps[v] = (c, (trust * moved + (1.0 - trust) * near[k]).astype(np.float32))
            else:
                maps[v] = (c, near[k])
        if frame_sink is not None:
            frame_sink(i, maps)
        if float(times[i]) < xm_fuse.WARMUP_S + float(times[first]):
            prev = maps
            continue
        flows = {}                                       # only this frame's pairs (variants often share one)
        for v, _ in VARIANTS:
            c, d = maps[v]
            acc = scores[v]
            ref = near[c]
            acc['align'].append(xm_fuse.ssi_mae(d, ref))
            acc['align_near'].append(xm_fuse.ssi_mae(d, ref, ref > xm_fuse.NEAR_THRESHOLD))
            edges = cv2.dilate(cv2.Canny(grey[c], 50, 150), np.ones((5, 5), np.uint8)) > 0
            acc['edge'].append(xm_fuse.edge_precision(d, edges))
            acc['age'].append((float(times[i]) - float(times[c])) * 1000.0)
            if v in prev:
                pc, pd = prev[v]
                if pc != c:
                    changes[v] += 1
                key = (pc, c)
                if key not in flows:
                    flows[key] = xm_fuse.FlowPair(grey[pc], grey[c])
                acc['flicker'].append(flows[key].change(pd, d))
        prev = maps
    duration = float(times[-1]) - float(times[first]) - xm_fuse.WARMUP_S
    result = {v: {k: float(np.nanmean(x)) if x else float('nan') for k, x in acc.items()} for v, acc in scores.items()}
    for v, _ in VARIANTS:
        result[v]['updates_per_s'] = changes[v] / max(duration, 1e-6)
    result['_meta'] = {'lat_ms': lat_ms, 'depth_per_s': len(xm_fuse.anchor_schedule(times, lat_ms)) / float(times[-1]),
                       'capture_fps': clip['fps'], 'motion_trust': trust_sum / trust_n if trust_n else float('nan')}
    log(f'simulate: exit lat={lat_ms}')
    return result


def render(clip, clip_dir, lat_ms):
    """One side-by-side video per variant (game frame and depth as shown), plus all four back to back."""
    log(f'render: enter lat={lat_ms}')
    meta = clip['meta']
    probe = cv2.VideoCapture(meta['video'])
    sw, sh = int(probe.get(cv2.CAP_PROP_FRAME_WIDTH)), int(probe.get(cv2.CAP_PROP_FRAME_HEIGHT))
    probe.release()
    size = (int(round(sw * 1080 / sh / 2)) * 2, 1080)
    colour = list(xm_sbs.colour_frames(meta, size))                  # full-res game frames, in cache order
    if len(colour) != len(clip['rgb']):
        raise RuntimeError(f'{len(colour)} colour frames decoded, cache has {len(clip["rgb"])}')
    sequence = {v: [] for v, _ in VARIANTS}

    def sink(i, maps):
        for v, _ in VARIANTS:
            sequence[v].append(maps[v])

    simulate(clip, clip_dir, lat_ms, sink)
    out_dir = os.path.join(HERE, 'out', 'sbs')
    os.makedirs(out_dir, exist_ok=True)
    name = os.path.basename(os.path.normpath(clip_dir))
    kernel = np.ones((2 * xm_sbs.DILATE + 1, 2 * xm_sbs.DILATE + 1), np.uint8)
    cw, ch = size
    parts = []
    for n, (v, labelText) in enumerate(VARIANTS):
        path = os.path.join(out_dir, f'{name}_sync{int(lat_ms)}_{n + 1}_{v}_SBS_LR.mp4')
        ren = subprocess.Popen([xm_sbs.RENDER, f'--size={cw}x{ch}'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        enc = subprocess.Popen([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                                '-s', f'{cw * 2}x{ch}', '-r', f'{clip["fps"]:.3f}', '-i', '-', '-c:v', 'libx264',
                                '-preset', 'medium', '-crf', '17', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', path],
                               stdin=subprocess.PIPE)
        for c, d in sequence[v]:
            ren.stdin.write(np.ascontiguousarray(colour[c]).tobytes())
            ren.stdin.write(np.ascontiguousarray(cv2.dilate(d, kernel).astype(np.float32)).tobytes())
            ren.stdin.flush()
            buf = ren.stdout.read(cw * 2 * ch * 3)
            sbs = np.frombuffer(buf, np.uint8).reshape(ch, cw * 2, 3).copy()
            xm_sbs.burn_label(sbs, labelText, cw)
            enc.stdin.write(sbs.tobytes())
        ren.stdin.close(); ren.wait(); enc.stdin.close(); enc.wait()
        parts.append(path)
        log(f'render: {path}')
    listing = os.path.join(out_dir, f'{name}_sync_concat.txt')
    with open(listing, 'w') as f:
        f.writelines(f"file '{os.path.abspath(p)}'\n" for p in parts)
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', listing,
                    '-c', 'copy', os.path.join(out_dir, f'{name}_sync{int(lat_ms)}_ALL_SBS_LR.mp4')], check=True)
    os.remove(listing)
    log('render: exit')


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    clip_dir = argv[1]
    opts = dict(a[2:].split('=', 1) for a in argv[2:] if a.startswith('--') and '=' in a)
    lats = [float(x) for x in opts.get('lat', '33,67').split(',')]
    render_lat = float(opts.get('render-lat', 67))
    log(f'main: enter {clip_dir} lats={lats}')
    clip = xm_fuse.load_clip(clip_dir)
    results = {'meta': clip['meta'], 'runs': {str(int(l)): simulate(clip, clip_dir, l) for l in lats}}
    with open(os.path.join(clip_dir, 'sync_results.json'), 'w') as f:
        json.dump(results, f, indent=2)
    if '--no-video' not in argv:
        render(clip, clip_dir, render_lat)
    log('main: exit')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
