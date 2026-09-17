"""Benchmark: Depth-Anything-V2-Small ONNX inference latency for VR real-time depth.

Measures per-frame inference latency (the critical budget for VR) and compares
against VR frame budgets (72/90/120/144 Hz).

Usage:
  python bench_depth.py --model models/.../onnx/model_fp16.onnx --ep cuda
  python bench_depth.py --model ... --ep cpu
"""
import argparse
import os
import time

import numpy as np
import onnxruntime as ort

VR_BUDGETS_MS = [
    (144, 1000.0 / 144),
    (120, 1000.0 / 120),
    (90, 1000.0 / 90),
    (72, 1000.0 / 72),
]


def make_session(model_path: str, ep: str) -> ort.InferenceSession:
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    if ep == "cuda":
        so.add_session_config_entry("cuda.device_id", "0")
        so.add_session_config_entry("cuda.graph_level", "5")
        providers = [
            ("CUDAExecutionProvider", {"device_id": 0, "cudnn_conv_algo_search": "HEURISTIC"}),
            "CPUExecutionProvider",
        ]
    elif ep == "dml":
        providers = [
            ("DmlExecutionProvider", {"device_id": 0, "gpu_performance_level": "Ultimate"}),
            "CPUExecutionProvider",
        ]
    elif ep == "trt":
        providers = ["TensorrtExecutionProvider", "CPUExecutionProvider"]
    else:
        providers = ["CPUExecutionProvider"]
    return ort.InferenceSession(model_path, sess_options=so, providers=providers)


def percentile(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * (p / 100.0)
    f = int(np.floor(k))
    c = min(f + 1, len(xs) - 1)
    if f == c:
        return xs[f]
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--ep", default="cuda", choices=["cuda", "dml", "trt", "cpu"])
    ap.add_argument("--warmup", type=int, default=30)
    ap.add_argument("--iters", type=int, default=300)
    ap.add_argument("--display-w", type=int, default=1080)
    ap.add_argument("--display-h", type=int, default=1200)
    ap.add_argument("--full-pipeline", action="store_true",
                    help="also time resize+normalize+infer+resize-back at display res")
    ap.add_argument("--size", type=int, default=518,
                    help="square model input size (H=W). Lower = faster.")
    args = ap.parse_args()

    sess = make_session(args.model, args.ep)
    active = [p[0] if isinstance(p, tuple) else p for p in sess.get_providers()]
    print(f"providers active: {active}")
    inp = sess.get_inputs()[0]
    out = sess.get_outputs()[0]
    print(f"input : {inp.name} {inp.shape} {inp.type}")
    print(f"output: {out.name} {out.shape} {out.type}")

    H = args.size
    # synthetic 'rendered frame': smooth gradients + structure, like a VR scene
    rng = np.random.default_rng(0)
    base = rng.normal(0.5, 0.2, size=(3, H, H)).clip(0, 1)
    x = base.astype(np.float32)[None]  # NCHW

    # warmup
    for _ in range(args.warmup):
        sess.run(None, {inp.name: x})

    # steady-state inference only
    ts = []
    for _ in range(args.iters):
        t0 = time.perf_counter()
        sess.run(None, {inp.name: x})
        ts.append((time.perf_counter() - t0) * 1000.0)

    mean = sum(ts) / len(ts)
    p50, p95, p99 = percentile(ts, 50), percentile(ts, 95), percentile(ts, 99)
    print(f"\n=== inference only ({H}x{H}) ===")
    print(f"n={len(ts)}  min={min(ts):.2f}  mean={mean:.2f}  p50={p50:.2f}  "
          f"p95={p95:.2f}  p99={p99:.2f}  max={max(ts):.2f} ms")
    print(f"achieved: {1000.0 / mean:.1f} fps (mean), {1000.0 / p95:.1f} fps (p95)")
    print("\nVR frame budgets:")
    for hz, ms in VR_BUDGETS_MS:
        fits = "OK " if p95 <= ms else "X  "
        margin = ms / p50 if p50 > 0 else float("inf")
        print(f"  {fits} {hz:4.0f} Hz -> {ms:5.2f} ms/frame  (p50={p50:.2f}, margin {margin:.2f}x)")

    if args.full_pipeline:
        from PIL import Image
        W, Dh = args.display_w, args.display_h
        big = rng.normal(0.5, 0.25, size=(Dh, W, 3)).clip(0, 1)
        big_img = (big * 255).astype(np.uint8)
        mean_f = np.array([0.485, 0.456, 0.406], np.float32)
        std_f = np.array([0.229, 0.224, 0.225], np.float32)

        def preprocess(img_u8):
            im = Image.fromarray(img_u8).resize((H, H), Image.BILINEAR)
            a = np.asarray(im, np.float32) / 255.0
            a = (a - mean_f[None, None]) / std_f[None, None]
            return a.transpose(2, 0, 1)[None]

        # warm
        for _ in range(10):
            xi = preprocess(big_img)
            sess.run(None, {inp.name: xi})
        ts2 = []
        for _ in range(100):
            t0 = time.perf_counter()
            xi = preprocess(big_img)
            d = sess.run(None, {inp.name: xi})[0][0]  # [1,1,H,H] or [1,H,H]
            dimg = Image.fromarray(d.squeeze().astype(np.float32)).resize(
                (W, Dh), Image.BILINEAR)
            ts2.append((time.perf_counter() - t0) * 1000.0)
        mean2 = sum(ts2) / len(ts2)
        p50b, p95b = percentile(ts2, 50), percentile(ts2, 95)
        print(f"\n=== full pipeline ({W}x{Dh} display -> {H}x{H} model -> back) ===")
        print(f"n={len(ts2)}  mean={mean2:.2f}  p50={p50b:.2f}  p95={p95b:.2f}  "
              f"max={max(ts2):.2f} ms")
        print(f"achieved: {1000.0 / mean2:.1f} fps (mean), {1000.0 / p95b:.1f} fps (p95)")
        for hz, ms in VR_BUDGETS_MS:
            fits = "OK " if p95b <= ms else "X  "
            print(f"  {fits} {hz:4.0f} Hz -> {ms:5.2f} ms/frame")


if __name__ == "__main__":
    main()
