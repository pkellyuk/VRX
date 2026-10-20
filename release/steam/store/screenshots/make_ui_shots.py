# Two Steam screenshots of VRX3D's desktop app: the real window (captured from the app)
# over a dimmed, blurred VR view, with a crisp framed panel of that VR view beside it and
# a cyan/magenta glow. No added text: Steam's screenshot rules exclude marketing copy.
#
#   python make_ui_shots.py      (reads work/expert.png, work/easy.png and the VR shots)
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1920, 1080
CYAN = (40, 230, 255)
MAGENTA = (255, 60, 200)


def log(msg):
    print(f'make_ui_shots: {msg}', flush=True)


def backdrop(path):
    """The VR shot, blurred and darkened, with a cyan-to-magenta wash."""
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    im = Image.open(path).convert('RGB').resize((W, H), Image.LANCZOS)
    a = np.asarray(im.filter(ImageFilter.GaussianBlur(14)), float) * 0.38
    x = np.linspace(0, 1, W)[None, :, None]
    y = np.linspace(0, 1, H)[:, None, None]
    wash = np.array(CYAN, float) * (1 - x) * 0.10 + np.array(MAGENTA, float) * x * 0.10
    a = a + wash * (0.6 + 0.4 * y)
    return Image.fromarray(a.clip(0, 255).astype(np.uint8)).convert('RGBA')


def rounded(im, radius):
    m = Image.new('L', im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius, fill=255)
    out = im.convert('RGBA'); out.putalpha(m)
    return out


def glow_under(base, box, radius, colour_a, colour_b, spread, strength):
    """A soft neon halo behind a rounded box, cyan on the left blending to magenta."""
    x0, y0, x1, y1 = box
    m = Image.new('L', base.size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius, fill=255)
    m = m.filter(ImageFilter.GaussianBlur(spread))
    t = np.clip((np.arange(W) - x0) / max(x1 - x0, 1), 0, 1)[None, :, None]
    col = np.array(colour_a, float) * (1 - t) + np.array(colour_b, float) * t
    a = np.asarray(m, float)[..., None] / 255 * strength
    rgb = np.asarray(base.convert('RGB'), float)
    rgb = rgb + col * a
    out = Image.fromarray(rgb.clip(0, 255).astype(np.uint8)).convert('RGBA')
    return out


def edge(base, box, radius, colour, width):
    d = ImageDraw.Draw(base)
    d.rounded_rectangle(box, radius, outline=colour + (255,), width=width)


def shadow(base, box, radius, offset, blur, opacity):
    m = Image.new('L', base.size, 0)
    x0, y0, x1, y1 = box
    ImageDraw.Draw(m).rounded_rectangle((x0 + offset, y0 + offset, x1 + offset, y1 + offset), radius, fill=int(255 * opacity))
    m = m.filter(ImageFilter.GaussianBlur(blur))
    black = Image.new('RGBA', base.size, (0, 0, 0, 255)); black.putalpha(m)
    base.alpha_composite(black)


def panel(base, src, box, radius=18):
    """Crisp VR view in a glowing rounded frame."""
    x0, y0, x1, y1 = box
    im = Image.open(src).convert('RGB').resize((x1 - x0, y1 - y0), Image.LANCZOS)
    base = glow_under(base, box, radius, CYAN, MAGENTA, 38, 0.55)
    shadow(base, box, radius, 10, 24, 0.6)
    base.alpha_composite(rounded(im, radius), (x0, y0))
    edge(base, box, radius, (110, 235, 255), 2)
    return base


def window(base, src, x, y, height=None, width=None, radius=14):
    """The real app window, scaled, with shadow, glow and a thin bright edge."""
    im = Image.open(src).convert('RGB')
    s = (height / im.height) if height else (width / im.width)
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    box = (x, y, x + im.width, y + im.height)
    base = glow_under(base, box, radius, MAGENTA, CYAN, 46, 0.65)
    shadow(base, box, radius, 14, 30, 0.75)
    base.alpha_composite(rounded(im, radius), (x, y))
    edge(base, box, radius, (120, 225, 240), 2)
    log(f'window {os.path.basename(src)} at {box}, scale {s:.3f}')
    return base


def main():
    log('enter')
    w = lambda n: os.path.join(HERE, 'work', n)
    s = lambda n: os.path.join(HERE, n)

    # 1. Expert: the full settings window on the left, the VR view it drives on the right.
    base = backdrop(s('vrx3d_bioshock.jpg'))
    base = panel(base, s('vrx3d_bioshock.jpg'), (860, 250, 860 + 980, 250 + 551))
    base = window(base, w('expert.png'), 90, 60, height=960)
    base.convert('RGB').save(s('vrx3d_ui_expert.jpg'), quality=95)
    log('wrote vrx3d_ui_expert.jpg')

    # 2. Easy: the compact window in front of a big framed VR view.
    base = backdrop(s('vrx3d_warhammer.jpg'))
    base = panel(base, s('vrx3d_warhammer.jpg'), (560, 70, 560 + 1270, 70 + 714))
    base = window(base, w('easy.png'), 90, 560, width=1000)
    base.convert('RGB').save(s('vrx3d_ui_easy.jpg'), quality=95)
    log('wrote vrx3d_ui_easy.jpg')
    log('exit ok')


if __name__ == '__main__':
    main()
