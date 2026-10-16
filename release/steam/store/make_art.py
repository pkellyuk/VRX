# Steam store and library art for VRX3D, drawn from code: a dark neon room, a glowing
# screen showing layered "depth" hills with cyan/magenta stereo fringes, and the VRX
# badge with a VRX3D wordmark. No game footage, so no other publisher's art.
#
#   python release/steam/store/make_art.py [out_dir]
#
# Sizes are Steam's (partner.steamgames.com/doc/store/assets, checked 2026-10-16).
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
LOGO = os.path.join(ROOT, 'docs', 'vrx-logo.png')
ICO = os.path.join(ROOT, 'desktop', 'VRX.Desktop', 'Assets', 'vrx.ico')
FONT = 'C:/Windows/Fonts/bahnschrift.ttf'

BG_TOP = np.array([6, 8, 20], float)
BG_HORIZON = np.array([40, 16, 70], float)
CYAN = (40, 230, 255)
MAGENTA = (255, 60, 200)
BLUE = np.array([80, 150, 255], float)
PURPLE = np.array([170, 70, 230], float)


def log(msg):
    print(f'make_art: {msg}', flush=True)


def glow(layer, radius, gain):
    """Additive glow: the layer blurred at a few radii."""
    if layer is None:
        raise ValueError('layer is None')
    out = np.zeros(np.asarray(layer).shape, float)
    for r, w in ((radius * 0.25, 0.6), (radius, 0.5), (radius * 2.5, 0.35)):
        out += w * np.asarray(layer.filter(ImageFilter.GaussianBlur(max(r, 0.5))), float)
    return out * gain


def hills(w, h, seed):
    """Layered ridgelines (far to near) inside a w x h screen, with stereo fringes."""
    rng = np.random.default_rng(seed)
    img = np.zeros((h, w, 3), float)
    # sky: deep blue to violet glow near the far ridge
    t = np.linspace(0, 1, h)[:, None]
    img[:] = (np.array([10, 14, 40]) * (1 - t) + np.array([90, 40, 140]) * t)[:, None, :][:, 0, :][:, None, :]
    x = np.linspace(0, 1, w)
    layers = 5
    for i in range(layers):
        near = i / (layers - 1)                      # 0 far .. 1 near
        base = 0.42 + 0.12 * i
        amp = 0.06 + 0.07 * near
        ridge = np.zeros(w)
        for k in range(1, 6):
            ridge += np.sin(x * np.pi * (1.3 + k * 1.7) + rng.uniform(0, 6.28)) * amp / k
        ridge = (base + ridge) * h
        col = (np.array([60, 30, 120]) * (1 - near) + np.array([20, 200, 230]) * near) * (0.55 + 0.45 * near)
        yy = np.arange(h)[:, None]
        mask = yy >= ridge[None, :]
        # stereo fringe: nearer layers get wider cyan (left) / magenta (right) edges
        shift = int(round(2 + 10 * near * w / 900))
        for dx, c in ((-shift, np.array(CYAN, float)), (shift, np.array(MAGENTA, float))):
            m2 = yy >= np.roll(ridge, dx)[None, :]
            edge = m2 & ~mask
            img[edge] = img[edge] * 0.3 + c * 0.7
        shade = (1 - 0.35 * (yy - ridge[None, :]).clip(0, h) / h)[..., None]
        img = np.where(mask[..., None], col[None, None, :] * shade, img)
    return img.clip(0, 255)


def scene(W, H, screen, horizon, seed=3):
    """The room: perspective grid floor, horizon glow, a curved glowing screen and its
    reflection. screen = (cx, cy, sw, sh) in pixels; horizon = y of the horizon."""
    log(f'scene enter {W}x{H} screen {screen} horizon {horizon}')
    img = np.zeros((H, W, 3), float)
    y = np.arange(H)[:, None]
    t = np.clip(y / max(horizon, 1), 0, 1)
    img[:] = (BG_TOP * (1 - t) ** 1.5 + BG_HORIZON * (1 - (1 - t) ** 1.5))[:, None, :][:, 0, :][:, None, :]
    below = y > horizon
    img = np.where(below[..., None], np.array([8, 6, 22], float), img)

    # grid floor
    grid = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(grid)
    vx = W / 2
    lw = max(1, int(round(W / 900)))
    for k in range(1, 40):
        z = k * 0.35
        gy = horizon + (H - horizon) * (0.35 / z) ** 0.9 * 1.0
        if gy >= H or gy <= horizon + 1:
            continue
        a = min(1.0, (gy - horizon) / (H - horizon) * 2.2)
        d.line([(0, gy), (W, gy)], fill=tuple(int(c * a) for c in (120, 50, 220)), width=lw)
    for k in range(-30, 31):
        x0 = vx + k * W / 14
        d.line([(vx + (x0 - vx) * 0.02, horizon), (x0 * 1.0 + (x0 - vx) * 1.6, H)], fill=(110, 45, 210), width=lw)
    g = np.asarray(grid, float)
    fade = np.clip((y - horizon) / (H - horizon + 1), 0, 1)[..., None] ** 0.8
    img += (g * 0.8 + glow(grid, W / 300, 0.7)) * fade

    # horizon glow
    hg = np.exp(-((y - horizon) / (H * 0.06)) ** 2)[..., None] * np.array([150, 50, 200], float) * 0.55
    img += hg

    # the screen, slightly curved: top/bottom edges bow outward
    cx, cy, sw, sh = screen
    sw, sh = int(sw), int(sh)
    content = hills(sw, sh, seed)
    bow = sh * 0.06
    xs = np.arange(sw)
    u = (xs / (sw - 1)) * 2 - 1
    off = bow * u ** 2                                  # edges taller: a wrap toward you
    x0, y0 = int(cx - sw / 2), int(cy - sh / 2)
    scr_mask = np.zeros((H, W), bool)
    outline = Image.new('L', (W, H))
    od = ImageDraw.Draw(outline)
    top_pts, bot_pts = [], []
    for i in range(sw):
        X = x0 + i
        if X < 0 or X >= W:
            continue
        top = int(round(y0 - off[i]))
        bot = int(round(y0 + sh + off[i]))
        ys = np.arange(top, bot)
        col = content[np.clip(((ys - top) * sh / max(bot - top, 1)).astype(int), 0, sh - 1), i]
        ok = (ys >= 0) & (ys < H)
        img[ys[ok], X] = col[ok] * 1.15
        scr_mask[max(top, 0):min(bot, H), X] = True
        top_pts.append((X, top)); bot_pts.append((X, bot))
        # floor reflection, faded
        ry = bot + (bot - ys) + int(sh * 0.04)
        ok2 = (ry >= 0) & (ry < H) & (ry > horizon)
        img[ry[ok2], X] = img[ry[ok2], X] * 0.6 + col[ok2] * 0.22
    if top_pts:
        od.line(top_pts, fill=255, width=max(2, int(W / 450)))
        od.line(bot_pts, fill=255, width=max(2, int(W / 450)))
        od.line([top_pts[0], bot_pts[0]], fill=255, width=max(2, int(W / 450)))
        od.line([top_pts[-1], bot_pts[-1]], fill=255, width=max(2, int(W / 450)))
    o = np.asarray(outline, float)[..., None] / 255.0
    ring = np.asarray(outline.filter(ImageFilter.GaussianBlur(W / 160)), float)[..., None] / 255.0
    img += o * np.array(CYAN, float) * 0.9 + ring * np.array([60, 160, 255], float) * 2.2
    # light spill from the screen onto the floor
    spill = np.asarray(Image.fromarray((scr_mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(W / 25)), float)[..., None] / 255.0
    img += spill * np.array([40, 60, 120], float) * 0.8 * (~scr_mask)[..., None]

    # vignette
    yy, xx = np.mgrid[0:H, 0:W]
    r = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2)
    img *= (1 - 0.45 * np.clip(r - 0.55, 0, 1))[..., None]
    log('scene exit')
    return img.clip(0, 255)


def wordmark(height):
    """'VRX3D' in the badge's colours: blue-to-purple fill, cyan and magenta offsets, glow.
    Returns an RGBA image."""
    if height <= 0:
        raise ValueError(f'height {height}')
    font = ImageFont.truetype(FONT, int(height * 1.2))
    font.set_variation_by_name('Bold')
    text = 'VRX3D'
    l, t, r, b = font.getbbox(text)
    pad = int(height * 0.35)
    w, h = r - l + 2 * pad, b - t + 2 * pad
    def mask(dx, dy):
        m = Image.new('L', (w, h))
        ImageDraw.Draw(m).text((pad - l + dx, pad - t + dy), text, font=font, fill=255)
        return m
    off = max(1, int(height * 0.035))
    main = np.asarray(mask(0, 0), float) / 255
    cy = np.asarray(mask(-off, 0), float) / 255
    mg = np.asarray(mask(off, off // 2), float) / 255
    ty = np.clip((np.arange(h)[:, None] - pad) / (b - t), 0, 1)
    fill = BLUE[None, None, :] * (1 - ty[..., None]) + PURPLE[None, None, :] * ty[..., None]
    rgb = np.zeros((h, w, 3)); a = np.zeros((h, w))
    for m, c in ((cy, np.array(CYAN, float)), (mg, np.array(MAGENTA, float))):
        rgb = rgb * (1 - m[..., None] * 0.9) + c * m[..., None] * 0.9; a = np.maximum(a, m * 0.9)
    rgb = rgb * (1 - main[..., None]) + fill * main[..., None]; a = np.maximum(a, main)
    halo = np.asarray(Image.fromarray((main * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(height * 0.12)), float) / 255
    ha = halo * 0.55 * (1 - a)
    rgb = (rgb * a[..., None] + np.array([90, 110, 255]) * ha[..., None]) / np.maximum(a + ha, 1e-6)[..., None]
    a = np.clip(a + ha, 0, 1)
    out = np.dstack([rgb.clip(0, 255), a * 255]).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def badge(size):
    im = Image.open(LOGO).convert('RGBA')
    return im.resize((size, size), Image.LANCZOS)


def lockup(height, stacked=False):
    """Badge and wordmark together (side by side, or badge over wordmark), RGBA."""
    if stacked:
        b = badge(int(height * 0.55))
        wm = wordmark(int(height * 0.24))
        W = max(b.width, wm.width); H = b.height + wm.height - int(height * 0.04)
        out = Image.new('RGBA', (W, H))
        out.alpha_composite(b, ((W - b.width) // 2, 0))
        out.alpha_composite(wm, ((W - wm.width) // 2, b.height - int(height * 0.04)))
        return out
    b = badge(int(height))
    wm = wordmark(int(height * 0.52))
    gap = -int(height * 0.08)
    out = Image.new('RGBA', (b.width + gap + wm.width, max(b.height, wm.height)))
    out.alpha_composite(b, (0, (out.height - b.height) // 2))
    out.alpha_composite(wm, (b.width + gap, (out.height - wm.height) // 2))
    return out


def place(base, item, cx, cy, max_w, max_h, shadow=True):
    """Fit item inside max_w x max_h centred at (cx, cy), with a soft dark plate behind."""
    s = min(max_w / item.width, max_h / item.height)
    it = item.resize((max(1, int(item.width * s)), max(1, int(item.height * s))), Image.LANCZOS)
    x, y = int(cx - it.width / 2), int(cy - it.height / 2)
    if shadow:
        m = int(max(it.height * 0.4, 4))
        a = Image.new('L', (it.width + 2 * m, it.height + 2 * m))
        a.paste(it.split()[3], (m, m))
        a = a.filter(ImageFilter.GaussianBlur(max(it.height * 0.18, 2))).point(lambda v: int(v * 0.85))
        plate = Image.new('RGBA', a.size, (0, 0, 8, 0)); plate.putalpha(a)
        layer = Image.new('RGBA', base.size); layer.paste(plate, (x - m, y - m))
        base.alpha_composite(layer)
    base.alpha_composite(it, (x, y))
    return base


def to_rgba(arr):
    return Image.fromarray(arr.astype(np.uint8), 'RGB').convert('RGBA')


def capsule(W, H, out, kind):
    log(f'capsule {kind} {W}x{H}')
    if kind == 'wide':
        img = to_rgba(scene(W, H, (W * 0.76, H * 0.40, W * 0.40, H * 0.44), H * 0.66))
        place(img, lockup(400), W * 0.29, H * 0.50, W * 0.52, H * 0.60)
    elif kind == 'small':
        img = to_rgba(scene(W, H, (W * 0.5, H * 0.40, W * 0.70, H * 0.60), H * 0.70))
        place(img, lockup(400), W * 0.5, H * 0.5, W * 0.90, H * 0.78)
    elif kind == 'tall':
        img = to_rgba(scene(W, H, (W * 0.5, H * 0.30, W * 0.82, H * 0.32), H * 0.50))
        place(img, lockup(900, stacked=True), W * 0.5, H * 0.72, W * 0.80, H * 0.44)
    elif kind == 'hero':
        img = to_rgba(scene(W, H, (W * 0.5, H * 0.44, W * 0.40, H * 0.52), H * 0.74))
    elif kind == 'background':
        a = scene(W, H, (W * 0.5, H * 0.38, W * 0.62, H * 0.42), H * 0.66) * 0.45
        img = to_rgba(a)
    else:
        raise ValueError(kind)
    img.convert('RGB').save(out, quality=95)
    log(f'wrote {out}')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'art')
    os.makedirs(out, exist_ok=True)
    log(f'enter, out {out}')
    j = lambda n: os.path.join(out, n)
    capsule(920, 430, j('header_capsule_920x430.jpg'), 'wide')
    capsule(462, 174, j('small_capsule_462x174.jpg'), 'small')
    capsule(1232, 706, j('main_capsule_1232x706.jpg'), 'wide')
    capsule(748, 896, j('vertical_capsule_748x896.jpg'), 'tall')
    capsule(1438, 810, j('page_background_1438x810.jpg'), 'background')
    capsule(600, 900, j('library_capsule_600x900.jpg'), 'tall')
    capsule(920, 430, j('library_header_920x430.jpg'), 'wide')
    capsule(3840, 1240, j('library_hero_3840x1240.jpg'), 'hero')
    # library logo: transparent PNG, 1280 wide
    lk = lockup(600)
    m = int(lk.height * 0.06)
    padded = Image.new('RGBA', (lk.width + 2 * m, lk.height + 2 * m)); padded.alpha_composite(lk, (m, m)); lk = padded
    s = 1280 / lk.width
    lk.resize((1280, int(lk.height * s)), Image.LANCZOS).save(j('library_logo_1280.png'))
    log('wrote library logo')
    # community icon 184x184 (jpg) and the client/shortcut icon (the app's own .ico)
    badge(184).convert('RGB').save(j('community_icon_184x184.jpg'), quality=95)
    Image.open(ICO).save(j('client_icon.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    log('exit ok')


if __name__ == '__main__':
    main()
