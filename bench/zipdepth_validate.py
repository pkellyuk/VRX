"""Validate a ZipDepth ONNX export against the PyTorch checkpoint, and compare its
output with Depth Anything V2 Small on the same images.

usage (from bench/, with the zipdepth venv):
    .venv-zipdepth/Scripts/python.exe zipdepth_validate.py <out_dir> <image> [<image> ...]

Checks, per image:
  1. export fidelity  - ONNX (CPU) vs the unpatched PyTorch model: max/mean abs diff
  2. output semantics - correlation with DA-V2 (both "larger = nearer" inverse depth)
  3. writes a side-by-side PNG: input | DA-V2 | ZipDepth | ZipDepth-npu
"""
import os
import sys

import numpy as np
import onnxruntime as ort
import torch
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'models', 'zipdepth'))
sys.path.insert(0, os.path.join(HERE, 'models', 'zipdepth', 'scripts'))

from export import load_model  # noqa: E402  (ZipDepth's own loader, incl. fusing)

ZW, ZH = 672, 384      # ZipDepth export size (multiples of 32, 1.75 aspect)
DW, DH = 686, 392      # current Depth Anything V2 Small export
MODELS = os.path.join(HERE, 'models')


def load_rgb(path, w, h):
    return np.asarray(Image.open(path).convert('RGB').resize((w, h), Image.BILINEAR), dtype=np.float32) / 255.0


def to_nchw(rgb):
    return np.ascontiguousarray(rgb.transpose(2, 0, 1)[None])


def norm01(d):
    lo, hi = np.percentile(d, 0.5), np.percentile(d, 99.5)
    return np.clip((d - lo) / max(hi - lo, 1e-6), 0, 1)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out_dir, images = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)

    torch_models = {
        'zipdepth': load_model(os.path.join(MODELS, 'zipdepth', 'checkpoints', 'zipdepth_base.pth'),
                               'base', 'balanced', 'cpu', upsample_unfold=True),
        'zipdepth_npu': load_model(os.path.join(MODELS, 'zipdepth', 'checkpoints', 'zipdepth_base_npu.pth'),
                                   'base', 'balanced', 'cpu', upsample_unfold=False),
    }
    onnx_models = {
        'zipdepth': ort.InferenceSession(os.path.join(MODELS, 'zipdepth_faithful_672x384.onnx'), providers=['CPUExecutionProvider']),
        'zipdepth_npu': ort.InferenceSession(os.path.join(MODELS, 'zipdepth_npu_faithful_672x384.onnx'), providers=['CPUExecutionProvider']),
    }
    da = ort.InferenceSession(os.path.join(MODELS, 'model_fixed_686x392.onnx'), providers=['CPUExecutionProvider'])
    mean = np.array([0.485, 0.456, 0.406], np.float32)
    std = np.array([0.229, 0.224, 0.225], np.float32)

    ok = True
    for path in images:
        name = os.path.splitext(os.path.basename(path))[0]
        print(f'== {name}')
        zin = to_nchw(load_rgb(path, ZW, ZH))

        # Depth Anything V2 Small: ImageNet-normalised input, [1, H, W] output
        drgb = load_rgb(path, DW, DH)
        da_out = da.run(None, {'pixel_values': to_nchw((drgb - mean) / std)})[0][0]
        da_on_z = np.asarray(Image.fromarray(da_out).resize((ZW, ZH), Image.BILINEAR))

        tiles = [Image.open(path).convert('RGB').resize((ZW, ZH)), Image.fromarray((norm01(da_on_z) * 255).astype(np.uint8)).convert('RGB')]
        for key in ('zipdepth', 'zipdepth_npu'):
            with torch.no_grad():
                t_out = torch_models[key](torch.from_numpy(zin)).numpy().reshape(ZH, ZW)
            o_out = onnx_models[key].run(None, {'image': zin})[0].reshape(ZH, ZW)
            diff = np.abs(t_out - o_out)
            rel = diff.max() / max(np.abs(t_out).max(), 1e-6)
            corr = np.corrcoef(norm01(o_out).ravel(), norm01(da_on_z).ravel())[0, 1]
            faithful = rel < 1e-3
            ok = ok and faithful and corr > 0
            print(f'   {key:13s} torch vs onnx: max {diff.max():.2e} mean {diff.mean():.2e} (rel {rel:.2e}) {"OK" if faithful else "MISMATCH"}'
                  f' | range {o_out.min():.3f}..{o_out.max():.3f} | corr with DA-V2 {corr:+.3f}')
            tiles.append(Image.fromarray((norm01(o_out) * 255).astype(np.uint8)).convert('RGB'))

        sheet = Image.new('RGB', (ZW * 2, ZH * 2))
        for i, t in enumerate(tiles):
            sheet.paste(t, ((i % 2) * ZW, (i // 2) * ZH))
        sheet.save(os.path.join(out_dir, f'{name}_compare.png'))

    print('ALL OK' if ok else 'PROBLEMS FOUND')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
