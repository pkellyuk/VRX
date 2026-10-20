"""Drawing helpers for the VRX promo: text, panels, glows, masks.

Frames are RGB float32 in [0, 1], shape (H, W, 3). Overlays are RGBA float32.
Everything is rasterised once and cached, because the film renders 1200 frames.
"""
from __future__ import annotations

import cv2
import numpy as np
from PIL import Image, ImageDraw

from . import vrx_brand as B

_TEXT_CACHE: dict[tuple, np.ndarray] = {}
_MASK_CACHE: dict[tuple, np.ndarray] = {}


def ease(v: float) -> float:
    """Smoothstep on a 0..1 ramp."""
    v = min(max(v, 0.0), 1.0)
    return v * v * (3.0 - 2.0 * v)


def clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, v))


def lerp(a, b, t):
    return a + (b - a) * t


def ramp(t: float, start: float, end: float) -> float:
    """0 before `start`, 1 after `end`, eased in between."""
    if end <= start:
        return 1.0 if t >= end else 0.0
    return ease((t - start) / (end - start))


def blot(t: float, at: float, fade_in: float, hold: float, fade_out: float) -> float:
    """A pulse: fade in, hold, fade out, around time `at`."""
    if t < at - hold / 2 - fade_in or t > at + hold / 2 + fade_out:
        return 0.0
    a = ramp(t, at - hold / 2 - fade_in, at - hold / 2)
    b = 1.0 - ramp(t, at + hold / 2, at + hold / 2 + fade_out)
    return min(a, b)


def fit_size(lines: list[str], size: int, *, weight: str = "regular",
             max_width: int, leading: float = 1.18, align: str = "left",
             tracking: float = 0.0, min_size: int = 12) -> int:
    """Largest type size at or below `size` whose widest line fits `max_width`.

    Fonts do not let you know a string's width without measuring it, so this
    measures and steps down. Cheap: the rasteriser caches, and this runs once per
    distinct call during the render.
    """
    while size > min_size:
        layer = text_rgba(lines, size, weight=weight, leading=leading,
                          align=align, tracking=tracking)
        if layer.shape[1] <= max_width:
            return size
        size = int(size * 0.95)
    return min_size


def text_rgba(lines: list[str], size: int, *, weight: str = "regular",
              colour=(255, 255, 255), leading: float = 1.18, align: str = "left",
              tracking: float = 0.0) -> np.ndarray:
    """Rasterise text into an RGBA array sized to fit, cached by its arguments."""
    key = (tuple(lines), size, weight, tuple(colour), leading, align, tracking)
    hit = _TEXT_CACHE.get(key)
    if hit is not None:
        return hit

    font = B.font(size, weight)
    probe = Image.new("L", (8, 8))
    pd = ImageDraw.Draw(probe)
    spacing = int(size * tracking)
    widths = []
    for line in lines:
        box = pd.textbbox((0, 0), line, font=font)
        widths.append(box[2] - box[0])
    w = max(widths) + 8 + spacing * max(len(l) for l in lines)
    line_h = int(size * leading)
    h = line_h * len(lines) + 8

    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, line in enumerate(lines):
        if align == "center":
            x = (w - widths[i] - spacing * len(line)) / 2
        elif align == "right":
            x = w - widths[i] - spacing * len(line)
        else:
            x = 4
        y = i * line_h
        if spacing:
            for ch in line:                       # manual tracking
                d.text((x, y), ch, font=font, fill=tuple(colour) + (255,))
                x += pd.textlength(ch, font=font) + spacing
        else:
            d.text((x, y), line, font=font, fill=tuple(colour) + (255,))
    arr = np.asarray(img).astype(np.float32) / 255.0
    _TEXT_CACHE[key] = arr
    return arr


def text_size(overlay: np.ndarray) -> tuple[int, int]:
    return overlay.shape[1], overlay.shape[0]


def put(frame: np.ndarray, overlay: np.ndarray, x: int, y: int,
        opacity: float = 1.0) -> None:
    """Alpha-blend an RGBA overlay onto an RGB float frame, in place."""
    if opacity <= 0.002:
        return
    h, w = overlay.shape[:2]
    fh, fw = frame.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(fw, x + w), min(fh, y + h)
    if x0 >= x1 or y0 >= y1:
        return
    src = overlay[y0 - y:y1 - y, x0 - x:x1 - x]
    a = np.clip(src[:, :, 3:4] * opacity, 0.0, 1.0)
    if a.max() <= 0.002:
        return
    dst = frame[y0:y1, x0:x1]
    dst *= 1.0 - a
    dst += src[:, :, :3] * a


def rounded_mask(w: int, h: int, radius: int) -> np.ndarray:
    radius = max(0, min(radius, w // 2, h // 2))
    key = (w, h, radius)
    hit = _MASK_CACHE.get(key)
    if hit is None:
        m = np.zeros((h, w), np.float32)
        if radius == 0:
            m[:] = 1.0
        else:
            cv2.rectangle(m, (radius, 0), (w - radius - 1, h - 1), 1.0, -1)
            cv2.rectangle(m, (0, radius), (w - 1, h - radius - 1), 1.0, -1)
            for cx, cy in [(radius, radius), (w - radius - 1, radius),
                           (radius, h - radius - 1), (w - radius - 1, h - radius - 1)]:
                cv2.circle(m, (cx, cy), radius, 1.0, -1, cv2.LINE_AA)
        hit = m
        _MASK_CACHE[key] = hit
    return hit


def composite_rgba(frame: np.ndarray, img: np.ndarray, alpha: np.ndarray,
                   x: int, y: int, opacity: float = 1.0) -> None:
    """Blend an RGBA patch over the frame, honouring its own transparency."""
    if opacity <= 0.002:
        return
    fh, fw = frame.shape[:2]
    ih, iw = img.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(fw, x + iw), min(fh, y + ih)
    if x0 >= x1 or y0 >= y1:
        return
    src = img[y0 - y:y1 - y, x0 - x:x1 - x]
    a = (alpha[y0 - y:y1 - y, x0 - x:x1 - x] * opacity)[:, :, None]
    dst = frame[y0:y1, x0:x1]
    dst *= 1.0 - a
    dst += src * a


def fit_cover(img: np.ndarray, w: int, h: int, bias: float = 0.5) -> np.ndarray:
    """Scale a source to fill (w, h) and centre-crop it."""
    ih, iw = img.shape[:2]
    s = max(w / iw, h / ih)
    nw, nh = int(round(iw * s)), int(round(ih * s))
    interp = cv2.INTER_AREA if s < 1 else cv2.INTER_LANCZOS4
    r = cv2.resize(img, (nw, nh), interpolation=interp)
    x0 = (nw - w) // 2
    y0 = int(round((nh - h) * bias))
    return r[y0:y0 + h, x0:x0 + w]


def fit_inside(img: np.ndarray, w: int, h: int) -> np.ndarray:
    """Scale a source to fit inside (w, h) without cropping or stretching."""
    ih, iw = img.shape[:2]
    s = min(w / iw, h / ih)
    nw, nh = max(1, int(round(iw * s))), max(1, int(round(ih * s)))
    interp = cv2.INTER_AREA if s < 1 else cv2.INTER_LANCZOS4
    return cv2.resize(img, (nw, nh), interpolation=interp)


def big_blur(img: np.ndarray, sigma: float) -> np.ndarray:
    """A very wide Gaussian blur, cheaply.

    Blurring a 1080p frame directly with a sigma of several hundred pixels costs
    seconds, because the kernel is thousands of pixels across. Filtering a
    downsampled copy and scaling back up is visually identical for a soft glow,
    which is the only thing this is used for.
    """
    if sigma <= 0:
        return img.copy()
    k = max(1, int(sigma / 4))
    if k == 1:
        return cv2.GaussianBlur(img, (0, 0), sigma)
    h, w = img.shape[:2]
    sw, sh = max(1, w // k), max(1, h // k)
    small = cv2.resize(img, (sw, sh), interpolation=cv2.INTER_AREA)
    small = cv2.GaussianBlur(small, (0, 0), sigma / k)
    return cv2.resize(small, (w, h), interpolation=cv2.INTER_LINEAR)


def glow(frame: np.ndarray, sigma: float, strength: float) -> None:
    """Add a blurred copy of the frame back onto itself, for bloom."""
    if strength <= 0:
        return
    soft = big_blur(frame, sigma)
    cv2.addWeighted(frame, 1.0, soft, strength, 0.0, frame)


def bloom_mask(frame: np.ndarray, mask: np.ndarray, sigma: float,
               strength: float, colour=None) -> None:
    """Bleed `mask` outward around itself - the ambilight behaviour."""
    soft = big_blur(mask, sigma)
    soft = big_blur(soft, sigma)
    soft = np.clip(soft * strength, 0, 1)[:, :, None]
    if colour is None:
        frame += soft * frame
    else:
        c = np.array(colour, np.float32)[None, None, :] / 255.0
        frame *= 1.0 - soft
        frame += c * soft


def border_colours(img: np.ndarray, depth: int = 14) -> np.ndarray:
    """The colours around the edge of an image, as an ambilight ring.

    Returns a downsampled frame whose border carries the edge colours and whose
    interior is black, ready to be blurred into an ambient glow.
    """
    h, w = img.shape[:2]
    ring = np.zeros_like(img)
    ring[:depth] = img[:depth]
    ring[-depth:] = img[-depth:]
    ring[:, :depth] = img[:, :depth]
    ring[:, -depth:] = img[:, -depth:]
    return ring


def rect(frame: np.ndarray, x: int, y: int, w: int, h: int, colour,
         radius: int = 0, alpha: float = 1.0) -> None:
    """Blend a solid rounded rectangle into the frame, in place."""
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(frame.shape[1], x + w), min(frame.shape[0], y + h)
    if x0 >= x1 or y0 >= y1:
        return
    c = np.array(colour, np.float32) / 255.0
    sub = frame[y0:y1, x0:x1]
    if radius <= 0:
        sub *= 1.0 - alpha
        sub += c * alpha
        return
    m = rounded_mask(x1 - x0, y1 - y0, radius)[:, :, None] * alpha
    sub *= 1.0 - m
    sub += c * m


def hairline(frame: np.ndarray, x0: int, y0: int, x1: int, y1: int, colour,
             alpha: float = 1.0, thickness: int = 1) -> None:
    """A single crisp line, blended rather than drawn over."""
    c = np.array(colour, np.float32) / 255.0
    h, w = frame.shape[:2]
    x0c, x1c = max(0, min(x0, x1)), min(w - 1, max(x0, x1))
    y0c, y1c = max(0, min(y0, y1)), min(h - 1, max(y0, y1))
    if x0c > x1c or y0c > y1c:
        return
    sub = frame[y0c:y1c + thickness, x0c:x1c + 1]
    sub *= 1.0 - alpha
    sub += c * alpha


def paste_fit(frame: np.ndarray, img: np.ndarray, x: int, y: int,
              opacity: float = 1.0) -> None:
    """Blend an RGB float patch into the frame at (x, y), clipping at the edges."""
    if opacity <= 0.002:
        return
    fh, fw = frame.shape[:2]
    ih, iw = img.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(fw, x + iw), min(fh, y + ih)
    if x0 >= x1 or y0 >= y1:
        return
    src = img[y0 - y:y1 - y, x0 - x:x1 - x]
    if opacity >= 0.998:
        frame[y0:y1, x0:x1] = src
    else:
        frame[y0:y1, x0:x1] = frame[y0:y1, x0:x1] * (1 - opacity) + src * opacity


def paste_masked(frame: np.ndarray, img: np.ndarray, x: int, y: int,
                 mask: np.ndarray, opacity: float = 1.0) -> None:
    """Blend a patch through a coverage mask (0..1), used for rounded corners."""
    fh, fw = frame.shape[:2]
    ih, iw = img.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(fw, x + iw), min(fh, y + ih)
    if x0 >= x1 or y0 >= y1:
        return
    src = img[y0 - y:y1 - y, x0 - x:x1 - x]
    m = mask[y0 - y:y1 - y, x0 - x:x1 - x, None] * opacity
    dst = frame[y0:y1, x0:x1]
    dst *= 1.0 - m
    dst += src * m


def rgb_to_frame(img: np.ndarray) -> np.ndarray:
    """uint8 RGB or BGRA PIL artwork to float RGB in [0, 1]."""
    if img.dtype == np.uint8:
        img = img.astype(np.float32) / 255.0
    return np.ascontiguousarray(img[:, :, :3])


def load_rgb(path) -> np.ndarray:
    """Load artwork as RGB float, flattening any alpha over black."""
    from PIL import Image
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
        bg.alpha_composite(im)
        im = bg.convert("RGB")
    else:
        im = im.convert("RGB")
    return np.asarray(im).astype(np.float32) / 255.0


def load_rgba(path) -> tuple[np.ndarray, np.ndarray]:
    """Load artwork as (RGB float, alpha float), preserving transparency."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im).astype(np.float32) / 255.0
    return np.ascontiguousarray(a[:, :, :3]), np.ascontiguousarray(a[:, :, 3])
