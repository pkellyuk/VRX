"""Faithful fixed-shape ONNX export of ZipDepth for VRX.

ZipDepth's own scripts/export.py replaces three forward() methods before export.
Two of them (StripPoolingAttention, MinimalCrossScale) are exact rewrites when the
input sides are multiples of 32. The third replaces GlobalContextBlock's learned,
softmax-weighted attention pooling with a plain average pool - an approximation
that changed outputs by ~3% mean / up to ~19% on test images (measured with
zipdepth_validate.py). This script keeps the two exact rewrites and exports the
original GlobalContextBlock, so the ONNX model is the model the authors evaluated.

One-time setup (from the repository root; model files and venvs are git-ignored):
    git clone --depth 1 https://github.com/fabiotosi92/ZipDepth bench/models/zipdepth
    py -3.12 -m venv bench/.venv-zipdepth
    bench/.venv-zipdepth/Scripts/python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
    bench/.venv-zipdepth/Scripts/python -m pip install onnx onnxscript onnxsim onnxruntime onnxconverter-common numpy pillow

Export the model VRX uses (set PYTHONUTF8=1 on Windows consoles):
    bench/.venv-zipdepth/Scripts/python bench/zipdepth_export.py bench/models/zipdepth/checkpoints/zipdepth_base.pth
        bench/models/zipdepth_faithful_fp16_672x384.onnx 384 672 --fp16

usage:
    zipdepth_export.py <ckpt> <out.onnx> <height> <width> [--npu] [--fp16]

Validate against PyTorch and Depth Anything V2 on real images: bench/zipdepth_validate.py.
Benchmark on DirectML: bench/native/dmlgpu/out/dmlgpu.exe <model> 384 672 20 200 in=image out=depth

Measured on an RTX 3090 (DirectML, GPU-resident input): DA-V2 Small 686x392 13.1 ms,
ZipDepth FP16 672x384 2.0 ms. Output is relative inverse depth, like DA-V2.
"""
import os
import sys
import types

import numpy as np
import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'models', 'zipdepth'))
sys.path.insert(0, os.path.join(HERE, 'models', 'zipdepth', 'scripts'))

from export import load_model  # noqa: E402


def apply_exact_rewrites(model, h, w):
    """The two export rewrites that are mathematically identical at h, w % 32 == 0."""
    s16 = (h // 16, w // 16)
    for m in model.modules():
        if type(m).__name__ == 'StripPoolingAttention':
            def _fwd(self_m, x):
                _, _, hh, ww = x.shape
                return x * self_m.gate_conv(F.adaptive_avg_pool2d(x, (hh, 1)) + F.adaptive_avg_pool2d(x, (1, ww)))
            m.forward = types.MethodType(_fwd, m)

    cs = model.encoder.cross_scale

    def _cs_fwd(self_cs, x_high, x_low, _s=s16):
        lo = F.interpolate(self_cs.low_to_high(x_low), size=_s, mode='nearest')
        hi = F.avg_pool2d(self_cs.high_to_low(x_high), 2, 2)
        return x_high + lo * 0.3, x_low + hi * 0.3
    cs.forward = types.MethodType(_cs_fwd, cs)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 4:
        print(__doc__)
        return 1
    ckpt, out, h, w = args[0], args[1], int(args[2]), int(args[3])
    npu = '--npu' in sys.argv
    fp16 = '--fp16' in sys.argv
    if h % 32 or w % 32:
        print(f'height and width must be multiples of 32 (got {w}x{h})')
        return 1

    model = load_model(ckpt, 'base', 'balanced', 'cpu', upsample_unfold=not npu)
    x = torch.rand(1, 3, h, w)
    with torch.no_grad():
        reference = model(x).numpy()
    apply_exact_rewrites(model, h, w)
    with torch.no_grad():
        rewritten = model(x).numpy()
    print(f'exact-rewrite check: max |diff| {np.abs(reference - rewritten).max():.2e}')

    raw = out.replace('.onnx', '_raw.onnx')
    with torch.no_grad():
        torch.onnx.export(model, x, raw, input_names=['image'], output_names=['depth'],
                          opset_version=18, do_constant_folding=True, dynamo=True)

    import onnx
    from onnxsim import simplify
    model_onnx, ok = simplify(onnx.load(raw))
    if not ok:
        print('onnxsim failed - keeping the raw export')
        model_onnx = onnx.load(raw)

    if fp16:
        # FP16 weights and arithmetic, float32 input/output (same contract as the
        # current Depth Anything export, so VRX's prep and readback are unchanged).
        from onnxconverter_common import float16
        model_onnx = float16.convert_float_to_float16(model_onnx, keep_io_types=True)

    onnx.save(model_onnx, out)
    os.remove(raw)
    if os.path.exists(raw + '.data'):
        os.remove(raw + '.data')

    import onnxruntime as ort
    sess = ort.InferenceSession(out, providers=['CPUExecutionProvider'])
    got = sess.run(None, {'image': x.numpy()})[0]
    diff = np.abs(got.reshape(reference.shape) - reference)
    print(f'{out}: {os.path.getsize(out) / 1e6:.1f} MB, nodes {len(model_onnx.graph.node)}, '
          f'vs original model max {diff.max():.2e} mean {diff.mean():.2e} (output range {reference.min():.3f}..{reference.max():.3f})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
