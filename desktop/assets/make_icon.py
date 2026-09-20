# Generates the VRX application icon: angular hand-built VRX letterforms with a
# neon cyan/magenta cyberpunk treatment. Run with any Python that has Pillow:
#   python desktop/assets/make_icon.py
# Writes desktop/VRX.Desktop/Assets/vrx.ico (multi-size) and a large PNG preview.
#
# Letterforms are polygons, not a font: full control of stroke weight per icon
# size, which is what keeps 16 px legible, and no font licensing to consider.
import io
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

# --- palette -----------------------------------------------------------------
BG_OUTER = (5, 8, 16)
BG_INNER = (17, 30, 52)
CYAN = (44, 246, 255)
MAGENTA = (255, 43, 214)
VIOLET = (122, 92, 255)
GRID = (44, 246, 255, 26)

SS = 8                      # supersample factor
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# --- letter geometry ---------------------------------------------------------
# Each letter lives in its own 0..1 x 0..1 box (y down) as a list of strokes.
# A stroke is (x0, y0, x1, y1); ends are cut perpendicular, which gives the
# chamfered, machined look.
V = [(0.06, 0.00, 0.50, 1.00), (0.94, 0.00, 0.50, 1.00)]
R = [(0.10, 0.00, 0.10, 1.00),          # stem
     (0.10, 0.08, 0.68, 0.08),          # top bar
     (0.68, 0.08, 0.90, 0.30),          # chamfered top-right corner
     (0.90, 0.30, 0.90, 0.52),          # bowl right edge, closing onto the mid bar
     (0.10, 0.52, 0.90, 0.52),          # mid bar closes the bowl
     (0.50, 0.52, 0.94, 1.00)]          # leg
X = [(0.06, 0.00, 0.94, 1.00), (0.94, 0.00, 0.06, 1.00)]
LETTERS = [V, R, X]

SLANT = 0.13                # rightward lean, applied about the vertical centre


def stroke_polygon(x0, y0, x1, y1, hw):
    dx, dy = x1 - x0, y1 - y0
    length = (dx * dx + dy * dy) ** 0.5
    if length <= 0:
        return None
    nx, ny = -dy / length * hw, dx / length * hw
    return [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]


def draw_letters(size, hw, pad_x, pad_y, gap):
    """White letter mask (L mode) at `size`, letters laid out across the box."""
    mask = Image.new('L', (size, size), 0)
    d = ImageDraw.Draw(mask)

    box = size - 2 * pad_x                      # width available for all letters
    lw = (box - gap * (len(LETTERS) - 1)) / len(LETTERS)
    lh = size - 2 * pad_y

    for i, letter in enumerate(LETTERS):
        ox = pad_x + i * (lw + gap)
        for (x0, y0, x1, y1) in letter:
            pts = stroke_polygon(x0 * lw, y0 * lh, x1 * lw, y1 * lh, hw)
            if not pts:
                continue
            out = []
            for (px, py) in pts:
                sy = py / lh - 0.5                      # -0.5 top .. +0.5 bottom
                out.append((ox + px - sy * SLANT * lw, pad_y + py))
            d.polygon(out, fill=255)
        # Fill the mitre gap where two strokes meet, with an octagon rather than a
        # circle: open stroke ends stay cut flat, so the letters read as machined
        # rather than rounded.
        ends = {}
        for (x0, y0, x1, y1) in letter:
            for pt in ((x0, y0), (x1, y1)):
                ends[pt] = ends.get(pt, 0) + 1
        for (px, py), count in ends.items():
            if count < 2:
                continue
            sy = py - 0.5
            cx, cy = ox + px * lw - sy * SLANT * lw, pad_y + py * lh
            r = hw * 1.08
            d.polygon([(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
                       for a in range(22, 382, 45)], fill=255)
    return mask


def vertical_gradient(size, *stops):
    """Vertical gradient through any number of colour stops."""
    grad = Image.new('RGB', (1, size))
    px = grad.load()
    spans = len(stops) - 1
    for y in range(size):
        t = y / max(1, size - 1) * spans
        i = min(spans - 1, int(t))
        f = t - i
        px[0, y] = tuple(int(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f) for c in range(3))
    return grad.resize((size, size), Image.BILINEAR)


def radial_background(size):
    bg = Image.new('RGB', (size, size), BG_OUTER)
    d = ImageDraw.Draw(bg)
    steps = 48
    for i in range(steps, 0, -1):
        t = i / steps
        r = size * 0.78 * t
        colour = tuple(int(BG_OUTER[c] + (BG_INNER[c] - BG_OUTER[c]) * (1 - t) ** 1.6) for c in range(3))
        d.ellipse([size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r], fill=colour)
    return bg


def chamfer_points(size, inset, cut):
    a, b = inset, size - inset
    return [(a + cut, a), (b - cut, a), (b, a + cut), (b, b - cut),
            (b - cut, b), (a + cut, b), (a, b - cut), (a, a + cut)]


def render(size, detailed):
    """One icon image at `size` px. `detailed` adds frame, grid, glow, glitch."""
    s = size * SS
    img = radial_background(s).convert('RGBA')

    if detailed:
        # faint tech grid, clipped to the plate later
        grid = Image.new('RGBA', (s, s), (0, 0, 0, 0))
        gd = ImageDraw.Draw(grid)
        step = s / 8
        for i in range(1, 8):
            gd.line([(i * step, 0), (i * step, s)], fill=GRID, width=max(1, SS // 3))
            gd.line([(0, i * step), (s, i * step)], fill=GRID, width=max(1, SS // 3))
        img = Image.alpha_composite(img, grid)

    # letters: heavier strokes and tighter margins when small, so 16 px still reads
    if detailed:
        hw, pad_x, pad_y, gap = s * 0.034, s * 0.115, s * 0.300, s * 0.070
    else:
        hw, pad_x, pad_y, gap = s * 0.040, s * 0.060, s * 0.225, s * 0.055
    mask = draw_letters(s, hw, pad_x, pad_y, gap)

    fill = vertical_gradient(s, CYAN, VIOLET, MAGENTA)

    if detailed:
        # chromatic aberration: offset cyan/magenta copies under the main letters
        for colour, dx in ((CYAN, -int(s * 0.011)), (MAGENTA, int(s * 0.011))):
            ghost = Image.new('RGBA', (s, s), (0, 0, 0, 0))
            ghost.paste(Image.new('RGBA', (s, s), colour + (120,)), (0, 0), ImageChops.offset(mask, dx, 0))
            img = Image.alpha_composite(img, ghost)

        # bloom: screen-blend the blurred letters as emitted light (wide halo plus a
        # tight hot core). Screen, not alpha-composite, so it adds light instead of
        # laying a translucent colour over the plate.
        alpha = img.split()[3]
        rgb = img.convert('RGB')
        for radius, strength in ((s * 0.055, 0.85), (s * 0.016, 0.65)):
            glow = mask.filter(ImageFilter.GaussianBlur(radius)).point(lambda v, k=strength: int(v * k))
            bloom = Image.new('RGB', (s, s), (0, 0, 0))
            bloom.paste(fill, (0, 0), glow)
            rgb = ImageChops.screen(rgb, bloom)
        img = Image.merge('RGBA', (*rgb.split(), alpha))

    letters = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    letters.paste(fill, (0, 0), mask)
    letters.putalpha(ImageChops.lighter(letters.split()[3], mask))
    img = Image.alpha_composite(img, letters)

    if detailed:
        # scanlines
        lines = Image.new('RGBA', (s, s), (0, 0, 0, 0))
        ld = ImageDraw.Draw(lines)
        for y in range(0, s, max(2, int(SS * 1.5))):
            ld.line([(0, y), (s, y)], fill=(0, 0, 0, 38), width=max(1, SS // 2))
        img = Image.alpha_composite(img, lines)

        # neon frame: cyan octagon, with magenta accents on the vertical edges
        fd = ImageDraw.Draw(img)
        ring = chamfer_points(s, s * 0.055, s * 0.16)
        fd.line(ring + [ring[0]], fill=CYAN + (225,), width=max(1, int(s * 0.018)), joint='curve')
        tick = s * 0.11
        for (cx, cy) in ((0.055, 0.5), (0.945, 0.5)):
            fd.line([(cx * s, cy * s - tick), (cx * s, cy * s + tick)],
                    fill=MAGENTA + (230,), width=max(1, int(s * 0.026)))

    # rounded/chamfered plate: clip everything to the frame shape
    plate = Image.new('L', (s, s), 0)
    ImageDraw.Draw(plate).polygon(chamfer_points(s, s * 0.03, s * 0.15) if detailed else
                                  chamfer_points(s, 0, s * 0.10), fill=255)
    img.putalpha(ImageChops.multiply(img.split()[3], plate))
    return img.resize((size, size), Image.LANCZOS)


def write_ico(path, images):
    """ICO with one PNG-compressed entry per size (Vista+)."""
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, format='PNG', optimize=True)
        blobs.append(buf.getvalue())

    offset = 6 + 16 * len(blobs)
    out = [struct.pack('<HHH', 0, 1, len(blobs))]
    for im, blob in zip(images, blobs):
        w = 0 if im.width >= 256 else im.width
        h = 0 if im.height >= 256 else im.height
        out.append(struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(blob), offset))
        offset += len(blob)
    with open(path, 'wb') as f:
        f.write(b''.join(out) + b''.join(blobs))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    assets = os.path.join(here, '..', 'VRX.Desktop', 'Assets')
    os.makedirs(assets, exist_ok=True)

    images = [render(size, detailed=size >= 32) for size in SIZES]
    ico = os.path.normpath(os.path.join(assets, 'vrx.ico'))
    write_ico(ico, images)
    print(f'wrote {ico} ({os.path.getsize(ico)} bytes, sizes {SIZES})')

    preview_dir = sys.argv[1] if len(sys.argv) > 1 else here
    sheet = Image.new('RGBA', (256 + 16 + 128, 256), (12, 16, 26, 255))
    sheet.paste(images[-1], (0, 0))
    y = 0
    for size in (128, 64, 48, 32, 24, 16):
        sheet.paste(images[SIZES.index(size)], (256 + 16, y))
        y += size + 6
    out = os.path.join(preview_dir, 'vrx-icon-preview.png')
    sheet.save(out)
    large = render(512, detailed=True)
    large.save(os.path.join(preview_dir, 'vrx-icon-512.png'))
    print(f'wrote {out}')

    # The logo the README shows, tracked in the repository.
    docs = os.path.normpath(os.path.join(here, '..', '..', 'docs'))
    os.makedirs(docs, exist_ok=True)
    logo = os.path.join(docs, 'vrx-logo.png')
    large.save(logo, optimize=True)
    print(f'wrote {logo} ({os.path.getsize(logo)} bytes, 512x512)')


if __name__ == '__main__':
    main()
