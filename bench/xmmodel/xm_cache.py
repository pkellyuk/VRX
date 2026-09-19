"""Two-model depth fusion (xmmodel), step 1: build the offline cache.

Extracts a clip's frames at the engine's depth-grid size and runs BOTH depth models
on every frame, CPU only (so it never competes with other GPU work). The fusion
simulator (xm_fuse.py) then replays any timing scheme from this cache.

usage (from bench/, with the DirectML venv, which also has the CPU provider):
    .venv-dml/Scripts/python.exe xmmodel/xm_cache.py <clip_dir> <video> [--start=S] [--seconds=S]

Writes to <clip_dir>:
    rgb.npy    uint8 [N, 392, 686, 3]   frames on the depth grid
    zip.npy    float32 [N, 392, 686]    ZipDepth-base raw output (larger = nearer)
    dav2.npy   float32 [N, 392, 686]    Depth Anything V2 Small raw output (larger = nearer)
    times.npy  float64 [N]              frame timestamps, seconds from the first
    meta.json  source, mean fps, frame count, per-model CPU ms

Matches the engine (xrapp5.cpp ModelSpec + prep shader):
  * the capture is box-filtered to each model's own input size (INTER_AREA);
  * DA-V2 takes ImageNet-normalised RGB at 686x392, ZipDepth RGB 0..1 at 672x384
    (it normalises internally) using the same FP16 export the engine ships;
  * both outputs are bilinearly resampled onto the 686x392 depth grid
    (ResampleToGrid), which is a no-op for DA-V2.
"""
import json
import os
import sys
import time

import cv2
import numpy as np
import onnxruntime as ort

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, '..', 'models')
GW, GH = 686, 392                      # engine depth grid (W x H)
ZW, ZH = 672, 384                      # ZipDepth input
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def log(msg):
    print(f'[{time.strftime("%H:%M:%S")}][xm_cache] {msg}', flush=True)


def read_frames(video, start, seconds):
    """Decodes [start, start+seconds) of video to RGB frames at the grid size, box
    filtered, plus each frame's timestamp in seconds from the first one. Game
    captures are often variable-rate, so the simulator uses these, not a fixed fps."""
    log(f'read_frames: enter video={video} start={start} seconds={seconds}')
    if not video or not os.path.isfile(video):
        raise FileNotFoundError(f'video not found: {video}')
    if seconds <= 0:
        raise ValueError('seconds must be > 0')

    cap = cv2.VideoCapture(video)
    if not cap.isOpened():
        raise RuntimeError(f'cannot open {video}')
    cap.set(cv2.CAP_PROP_POS_MSEC, start * 1000.0)
    frames, stamps = [], []
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        t = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        if t < start - 1e-3:
            continue
        if t >= start + seconds:
            break
        frames.append(cv2.cvtColor(cv2.resize(bgr, (GW, GH), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB))
        stamps.append(t)
    cap.release()
    if not frames:
        raise RuntimeError(f'no frames decoded from {video} at {start}s')
    stamps = np.array(stamps) - stamps[0]
    log(f'read_frames: exit {len(frames)} frames over {stamps[-1]:.2f} s')
    return np.stack(frames), stamps


def session(file):
    if not file:
        raise ValueError('no model file')
    so = ort.SessionOptions()
    so.intra_op_num_threads = 12
    # The full CPU optimiser in ORT 1.24.4 fails on the DA-V2 graph
    # (SimplifiedLayerNormFusion); basic level gives identical results.
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    return ort.InferenceSession(os.path.join(MODELS, file), so, providers=['CPUExecutionProvider'])


def run_models(frames):
    """Both models on every frame; returns (zip, dav2, ms per model)."""
    log(f'run_models: enter {len(frames)} frames')
    if frames is None or len(frames) == 0:
        raise ValueError('no frames')

    dav2 = session('model_fixed_686x392.onnx')
    zipd = session('zipdepth_faithful_fp16_672x384.onnx')
    n = len(frames)
    out_zip = np.empty((n, GH, GW), np.float32)
    out_dav2 = np.empty((n, GH, GW), np.float32)
    t_zip = t_dav2 = 0.0
    for i, rgb8 in enumerate(frames):
        rgb = rgb8.astype(np.float32) / 255.0

        x = ((rgb - MEAN) / STD).transpose(2, 0, 1)[None]
        t = time.perf_counter()
        out_dav2[i] = dav2.run(None, {'pixel_values': np.ascontiguousarray(x)})[0].reshape(GH, GW)
        t_dav2 += time.perf_counter() - t

        small = cv2.resize(rgb8, (ZW, ZH), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0
        t = time.perf_counter()
        z = zipd.run(None, {'image': np.ascontiguousarray(small.transpose(2, 0, 1)[None])})[0].reshape(ZH, ZW)
        t_zip += time.perf_counter() - t
        out_zip[i] = cv2.resize(z, (GW, GH), interpolation=cv2.INTER_LINEAR)

        if i % 50 == 0 or i == n - 1:
            log(f'run_models: frame {i + 1}/{n}  dav2 {t_dav2 * 1000 / (i + 1):.0f} ms  zip {t_zip * 1000 / (i + 1):.0f} ms')
    log('run_models: exit')
    return out_zip, out_dav2, t_zip * 1000 / n, t_dav2 * 1000 / n


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    clip_dir, video = argv[1], argv[2]
    opts = dict(a[2:].split('=', 1) for a in argv[3:] if a.startswith('--') and '=' in a)
    start = float(opts.get('start', 0))
    seconds = float(opts.get('seconds', 8))
    log(f'main: enter clip_dir={clip_dir} video={video} start={start} seconds={seconds}')

    os.makedirs(clip_dir, exist_ok=True)
    frames, stamps = read_frames(video, start, seconds)
    zip_out, dav2_out, ms_zip, ms_dav2 = run_models(frames)
    np.save(os.path.join(clip_dir, 'rgb.npy'), frames)
    np.save(os.path.join(clip_dir, 'zip.npy'), zip_out)
    np.save(os.path.join(clip_dir, 'dav2.npy'), dav2_out)
    np.save(os.path.join(clip_dir, 'times.npy'), stamps)
    meta = {'video': os.path.abspath(video), 'start': start, 'seconds': seconds,
            'fps': round(float((len(stamps) - 1) / max(stamps[-1], 1e-6)), 3), 'frames': int(len(frames)),
            'cpu_ms': {'zip': round(ms_zip, 1), 'dav2': round(ms_dav2, 1)}}
    with open(os.path.join(clip_dir, 'meta.json'), 'w') as f:
        json.dump(meta, f, indent=2)
    log(f'main: exit {meta}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
