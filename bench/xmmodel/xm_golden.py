"""Golden data for the C++ port of the fusion maths (bench/native/openxr/depth_fusion.h).

Exports one frame's inputs and xm_fuse.py's outputs as raw little-endian files, which
bench/native/xmmodel/out/fusion_golden.exe reads and compares against.

usage (from bench/):
    .venv-dml/Scripts/python.exe xmmodel/xm_golden.py <clip_dir> <out_dir> [--frame=N] [--lat=27]

Needs <clip_dir>/me_<lat>/vectors.bin and me_prev/vectors.bin. Inputs are normalised
per frame (no range smoothing): the test covers the maths, not the smoothers.
"""
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xm_fuse  # noqa: E402


def near(raw):
    lo, hi = np.percentile(raw, (0.5, 99.5))
    return xm_fuse.to_near(np.asarray(raw), float(lo), float(hi))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    clip_dir, out_dir = argv[1], argv[2]
    opts = dict(a[2:].split('=', 1) for a in argv[3:] if a.startswith('--') and '=' in a)
    lat = float(opts.get('lat', 27))
    clip = xm_fuse.load_clip(clip_dir)
    pairs = dict(xm_fuse.live_pairs(clip['times'], lat))
    frame = int(opts.get('frame', len(clip['rgb']) // 2))
    k = pairs.get(frame)
    if k is None or k == frame or frame < 1:
        raise ValueError(f'frame {frame} has no older DA-V2 result at {lat} ms')

    grey = lambda i: cv2.cvtColor(np.asarray(clip['rgb'][i]), cv2.COLOR_RGB2GRAY)
    z, z_prev, z_k = near(clip['zip'][frame]), near(clip['zip'][frame - 1]), near(clip['zip'][k])
    anchor = near(clip['dav2'][k])
    ga, gb = xm_fuse.global_fit(z_k, anchor)

    hw = xm_fuse.load_hw_mv(clip_dir, lat)
    prev = xm_fuse.load_hw_mv(clip_dir, None)
    mx, my = hw[(frame, k)]
    moved = cv2.remap(anchor, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    trust = xm_fuse.motion_trust(grey(frame), grey(k), mx, my)
    target = trust * moved + (1.0 - trust) * np.clip(ga * z + gb, 0.0, 1.0)
    a, b = xm_fuse.local_fit(z, target, 16)
    fused = np.clip(a * z + b, 0.0, 1.0).astype(np.float32)

    px, py = prev[(frame, frame - 1)]
    trust_prev = xm_fuse.motion_trust(grey(frame), grey(frame - 1), px, py)
    steady = xm_fuse.stabilise(z, z_prev, (frame, frame - 1), prev, grey(frame), grey(frame - 1))

    def vectors(lat_dir, key):
        d = os.path.join(clip_dir, lat_dir)
        with open(os.path.join(d, 'pairs.txt')) as f:
            keys = [tuple(int(x) for x in line.split()) for line in f if line.strip()]
        bw, bh = (xm_fuse.GW + 7) // 8, (xm_fuse.GH + 7) // 8
        raw = np.fromfile(os.path.join(d, 'vectors.bin'), np.int16).reshape(len(keys), bh, bw, 2)
        return raw[keys.index(key)]

    os.makedirs(out_dir, exist_ok=True)
    files = {
        'z.f32': z, 'z_prev.f32': z_prev, 'anchor.f32': anchor,
        'luma_cur.f32': grey(frame).astype(np.float32), 'luma_anchor.f32': grey(k).astype(np.float32),
        'luma_prev.f32': grey(frame - 1).astype(np.float32),
        'vec_anchor.i16': vectors(f'me_{int(lat)}', (frame, k)), 'vec_prev.i16': vectors('me_prev', (frame, frame - 1)),
        'expect_trust.f32': trust, 'expect_fused.f32': fused,
        'expect_trust_prev.f32': trust_prev, 'expect_steady.f32': steady,
    }
    for name, arr in files.items():
        np.ascontiguousarray(arr).tofile(os.path.join(out_dir, name))
    with open(os.path.join(out_dir, 'meta.json'), 'w') as f:
        json.dump({'w': xm_fuse.GW, 'h': xm_fuse.GH, 'frame': frame, 'anchor_frame': k, 'ga': ga, 'gb': gb,
                   'trust_mean': float(trust.mean()), 'trust_prev_mean': float(trust_prev.mean())}, f, indent=2)
    with open(os.path.join(out_dir, 'fit.txt'), 'w') as f:
        f.write(f'{ga:.9g} {gb:.9g}\n')
    print(f'golden: frame {frame}, anchor {k}, trust mean {trust.mean():.3f}, prev trust mean {trust_prev.mean():.3f} -> {out_dir}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
