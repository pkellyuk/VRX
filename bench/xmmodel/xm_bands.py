"""Depth banding ("ploughed field") in the stereo warp, and a sub-pixel fix.

VRX's warp shifts each source pixel by a WHOLE number of pixels (xrapp5.cpp kWarpHlsl:
`int r = lround(scaleFocal * eye * invZ)`). Across the whole depth range that is only
about 25 distinct shifts at 1920 px wide, so a smoothly receding surface (a field,
a road, a wall seen at an angle) is drawn as flat stripes with a 1 px step between
them. On uniform texture like grass the steps read as ridges.

This script measures the banding and compares two warps on the same frame:
  integer   what VRX does today
  subpixel  the same scatter and hole fill, but the colour is sampled at the
            fractional source position that maps exactly to the destination pixel
            (one extra scratch value per pixel, bilinear sample instead of a load)

usage (from bench/):
    .venv-dml/Scripts/python.exe xmmodel/xm_bands.py [<clip_dir> [<frame>]] [--sbs]
Writes xmmodel/out/bands/*.png and prints a banding measure for both warps. With
--sbs it also renders side-by-side 3D videos through the engine's own warp
(native/xmmodel/out/sbs_render.exe, with and without --subpixel) so the ridges can be
judged in a headset: the synthetic field, and the clip's own frames if one is given.
"""
import os
import sys

import subprocess

import cv2
import imageio_ffmpeg
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xm_fuse  # noqa: E402
import xm_sbs  # noqa: E402

# VRX's desktop calibration (xrapp5.cpp RunFrameLoop / screen_anchor.h).
SCREEN_W, DISTANCE, IPD = 5.7, 3.0, 0.063
INV_NEAR, INV_FAR = 1 / 1.2 - 1 / DISTANCE, 1 / 12.0 - 1 / DISTANCE
MIRROR_TOL = 0.08


def shifts(near, cw):
    """Per-pixel eye shift in pixels (positive = moves left in the left eye)."""
    focal = cw * DISTANCE / SCREEN_W
    return focal * (IPD / 2) * (INV_FAR + near * (INV_NEAR - INV_FAR))


def warp_row(colour_row, near_row, shift_row, subpixel):
    """One row, one eye: kWarpHlsl's scatter + mirror hole fill. With subpixel, the
    colour is sampled at the fractional source position instead of the integer one."""
    w = colour_row.shape[0]
    src = np.full(w, -1, np.int32)
    src_near = np.zeros(w, np.float32)
    dest_f = np.arange(w, dtype=np.float32) - shift_row          # float destination of each source pixel
    dest_i = np.floor(dest_f + 0.5).astype(np.int32)             # lround, as the shader does
    for x in range(w):
        dx = dest_i[x]
        if dx < 0 or dx >= w:
            continue
        if src[dx] < 0 or near_row[x] > src_near[dx]:
            src[dx] = x
            src_near[dx] = near_row[x]
    # mirror hole fill from the farther neighbour (xr_common.h FillHole)
    x = 0
    last_valid, last_near = -1, 0.0
    while x < w:
        if src[x] >= 0:
            last_valid, last_near = src[x], src_near[x]
            x += 1
            continue
        r = x + 1
        while r < w and src[r] < 0:
            r += 1
        right_valid = src[r] if r < w else -1
        right_near = src_near[r] if r < w else 0.0
        if last_valid < 0 and right_valid < 0:
            src[x:r] = x
            src_near[x:r] = near_row[x]
        else:
            use_left = last_valid >= 0 and not (right_valid >= 0 and right_near < last_near)
            anchor = last_valid if use_left else right_valid
            anchor_near = last_near if use_left else right_near
            good, good_near = anchor, anchor_near
            for k in range(1, r - x + 1):
                h = (x - 1 + k) if use_left else (r - k)
                cand = int(np.clip(anchor - k if use_left else anchor + k, 0, w - 1))
                if near_row[cand] <= anchor_near + MIRROR_TOL:
                    good, good_near = cand, near_row[cand]
                src[h] = good
                src_near[h] = good_near
        x = r
    if not subpixel:
        return colour_row[src]
    # Sub-pixel: source pixel `s` lands at dest_f[s]; the pixel that lands exactly on
    # this destination is about s + (dest - dest_f[s]) (the mapping's slope is ~1).
    dest = np.arange(w, dtype=np.float32)
    pos = np.clip(src + (dest - dest_f[src]), 0, w - 1)
    i0 = np.floor(pos).astype(np.int32)
    i1 = np.minimum(i0 + 1, w - 1)
    f = (pos - i0)[:, None]
    return (colour_row[i0] * (1 - f) + colour_row[i1] * f).astype(np.float32)


def warp_eye(colour, near_grid, eye_sign, subpixel):
    """colour: H x W x 3 float. near_grid: the 686x392 depth grid, sampled up."""
    ch, cw = colour.shape[:2]
    near = cv2.resize(near_grid, (cw, ch), interpolation=cv2.INTER_LINEAR)
    sh = shifts(near, cw) * eye_sign
    out = np.empty_like(colour)
    for y in range(ch):
        out[y] = warp_row(colour[y], near[y], sh[y], subpixel)
    return out


def banding(near_grid, cw):
    """How the rounded shift steps across the picture: distinct values and the share
    of pixels sitting on a step (a ridge)."""
    near = cv2.resize(near_grid, (cw, int(cw * near_grid.shape[0] / near_grid.shape[1])), interpolation=cv2.INTER_LINEAR)
    rounded = np.round(shifts(near, cw))
    steps = np.abs(np.diff(rounded, axis=0)) > 0                  # vertical steps: ridges across a field
    return int(len(np.unique(rounded))), float(steps.mean())


def field_scene(cw=1920, ch=1080):
    """A grass-like field receding to the horizon: uniform texture, smooth depth."""
    rng = np.random.default_rng(7)
    noise = rng.random((ch, cw)).astype(np.float32)
    grass = cv2.GaussianBlur(noise, (0, 0), 1.2)
    grass = (grass - grass.min()) / (grass.max() - grass.min())
    colour = np.stack([grass * 0.35, 0.25 + grass * 0.55, grass * 0.30], axis=2).astype(np.float32)
    # Ground plane: distance grows towards the horizon at 40% height.
    y = np.arange(ch, dtype=np.float32)[:, None]
    horizon = ch * 0.40
    z = np.clip(1.6 / np.maximum((y - horizon) / ch, 1e-3) * 0.25, 1.2, 12.0)
    z = np.repeat(z, cw, axis=1)
    near = np.clip((1 / z - 1 / 12.0) / (1 / 1.2 - 1 / 12.0), 0, 1).astype(np.float32)
    near[:int(horizon)] = 0.0
    return colour, cv2.resize(near, (686, 392), interpolation=cv2.INTER_AREA)


def render_sbs(name, frames, fps, subpixel, out_dir, label):
    """frames: iterable of (colour HxWx3 float 0..1, near grid). One SBS video."""
    path = os.path.join(out_dir, f'{name}_{"subpixel" if subpixel else "integer"}_SBS_LR.mp4')
    first = True
    ren = enc = None
    kernel = np.ones((2 * xm_sbs.DILATE + 1, 2 * xm_sbs.DILATE + 1), np.uint8)
    for colour, near in frames:
        ch, cw = colour.shape[:2]
        if first:
            args = [xm_sbs.RENDER, f'--size={cw}x{ch}'] + (['--subpixel'] if subpixel else [])
            ren = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            enc = subprocess.Popen([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                                    '-s', f'{cw * 2}x{ch}', '-r', f'{fps:.3f}', '-i', '-', '-c:v', 'libx264', '-preset', 'medium',
                                    '-crf', '17', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', path], stdin=subprocess.PIPE)
            first = False
        rgb8 = np.clip(colour * 255, 0, 255).astype(np.uint8)
        ren.stdin.write(np.ascontiguousarray(rgb8).tobytes())
        ren.stdin.write(np.ascontiguousarray(cv2.dilate(near, kernel).astype(np.float32)).tobytes())
        ren.stdin.flush()
        buf = ren.stdout.read(cw * 2 * ch * 3)
        sbs = np.frombuffer(buf, np.uint8).reshape(ch, cw * 2, 3).copy()
        xm_sbs.burn_label(sbs, label, cw)
        enc.stdin.write(sbs.tobytes())
    ren.stdin.close(); ren.wait(); enc.stdin.close(); enc.wait()
    print(f'  {path}')
    return path


def concat(paths, out_path):
    listing = out_path + '.txt'
    with open(listing, 'w') as f:
        f.writelines(f"file '{os.path.abspath(p)}'\n" for p in paths)
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', listing,
                    '-c', 'copy', out_path], check=True)
    os.remove(listing)
    print(f'  {out_path}')


def save(path, img):
    cv2.imwrite(path, cv2.cvtColor(np.clip(img * 255, 0, 255).astype(np.uint8), cv2.COLOR_RGB2BGR))


def main(argv):
    out_dir = os.path.join(HERE, 'out', 'bands')
    os.makedirs(out_dir, exist_ok=True)
    scenes = [('field', *field_scene())]
    if len(argv) > 1:
        clip = xm_fuse.load_clip(argv[1])
        frame = int(argv[2]) if len(argv) > 2 else len(clip['rgb']) // 2
        # the original frame at 1080p (the cached 686x392 copy is too soft to judge ridges)
        full = list(xm_sbs.colour_frames(clip['meta'], (1920, 1080)))[frame]
        colour = full.astype(np.float32) / 255.0
        raw = np.asarray(clip['zip'][frame])
        lo, hi = np.percentile(raw, (0.5, 99.5))
        scenes.append((f'{os.path.basename(os.path.normpath(argv[1]))}_{frame}', colour, xm_fuse.to_near(raw, float(lo), float(hi))))

    for name, colour, near in scenes:
        cw = colour.shape[1]
        levels, ridge = banding(near, cw)
        print(f'{name}: {levels} distinct pixel shifts, {ridge * 100:.1f}% of pixels sit on a 1 px step')
        for mode in (False, True):
            eye = warp_eye(colour, near, -1.0, mode)
            tag = 'subpixel' if mode else 'integer'
            save(os.path.join(out_dir, f'{name}_{tag}.png'), eye)
            # difference from the unwarped picture shows where content moved
            print(f'  {tag}: mean |eye - source| {np.mean(np.abs(eye - colour)):.4f}')
    print(f'images in {out_dir}')

    if '--sbs' in argv:
        print('side-by-side videos (4 s each):')
        for name, colour, near in scenes:
            parts = []
            for subpixel in (False, True):
                label = ('Sub-pixel warp (fix)' if subpixel else 'Whole-pixel warp (today)') + f' - {name}'
                frames = ((colour, near) for _ in range(120))          # a still scene: the ridges do not move
                parts.append(render_sbs(name, frames, 30.0, subpixel, out_dir, label))
            concat(parts, os.path.join(out_dir, f'{name}_BOTH_SBS_LR.mp4'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
