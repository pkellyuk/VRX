"""The VRX promo film: six shots, composed from the product's own assets.

Every picture on screen comes from the repository: the real game captures, the
app's real depth maps for those captures, the real headset capture, the real
desktop UI and the real logo. Text is the film's own, and every claim it makes is
one the repository already makes (see vrx_shotlist.py for the exact source of
each).

The same module composes both the 16:9 master and the 9:16 social cut: the
`vertical` flag moves the screen into a panel and the captions above and below it,
rather than letterboxing a wide image.
"""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np

from . import vrx_brand as B
from . import vrx_dibr as DIBR
from . import vrx_draw as D
from .vrx_shotlist import FPS, SHOTS, TOTAL_SECONDS


@dataclass(frozen=True)
class Geo:
    """Where things go in the frame, for one aspect ratio."""
    w: int
    h: int
    vertical: bool
    text: float          # a scale factor for type sizes
    stage: tuple[int, int, int, int]     # the screen, when it is not full bleed
    cap: tuple[int, int]                 # caption anchor (left, baseline top)
    cap_size: int
    label: tuple[int, int]
    label_size: int
    margin: int

    @property
    def full_bleed(self) -> bool:
        return self.stage == (0, 0, self.w, self.h)


def geometry(w: int, h: int, vertical: bool) -> Geo:
    if vertical:
        sw = int(w * 0.926)
        sh = int(sw * 9 / 16)
        stage = ((w - sw) // 2, int(h * 0.345), sw, sh)
        return Geo(w, h, True, 1.32, stage, (int(w * 0.065), int(h * 0.115)),
                   74, (int(w * 0.065), int(h * 0.055)), 30, int(w * 0.065))
    return Geo(w, h, False, 1.0, (0, 0, w, h), (96, 762), 62, (96, 62), 26, 96)


class Film:
    """Renders the promo one frame at a time, at any of the two aspect ratios."""

    def __init__(self, w: int = 1920, h: int = 1080, *, vertical: bool = False,
                 verbose: bool = False):
        self.w, self.h = w, h
        self.vertical = vertical
        self.g = geometry(w, h, vertical)
        self.verbose = verbose
        self._cache: dict = {}
        self._static: dict = {}

        # Parallax content is rendered at the size it will be shown at.
        sw, sh = self.g.stage[2], self.g.stage[3]
        self.shots_depth = {
            name: DIBR.DepthShot(name, sw, sh, crop_bias=0.60)
            for name in ("cyberpunk", "helldivers", "system-shock")
        }
        self.headset = D.load_rgb(B.DOCS / "vrx-headset.png")
        self.logo_rgb, self.logo_a = D.load_rgba(B.DOCS / "vrx-logo.png")
        self.ui_rgb, self.ui_a = D.load_rgba(B.ASSETS / "easy-mode.png")
        self._bg = B.gradient(w, h)
        self._vig = B.vignette(w, h, 0.50, 2.2)
        # Every headline records the size it asked for and the size it got, so a
        # build can prove nothing overflowed instead of hoping.
        self.audit: dict[str, tuple[int, int]] = {}

    # -- shared pieces -----------------------------------------------------
    def background(self) -> np.ndarray:
        return self._bg.copy()

    def scrim(self, frame: np.ndarray, x: int, y: int, w: int, h: int,
              strength: float = 0.72) -> None:
        """A soft dark wash so light type stays legible over picture."""
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(frame.shape[1], x + w), min(frame.shape[0], y + h)
        if x0 >= x1 or y0 >= y1:
            return
        mask = cv2.GaussianBlur(cv2.rectangle(
            np.zeros((y1 - y0, x1 - x0), np.float32), (1, 1), (x1 - x0 - 2, y1 - y0 - 2),
            1.0, -1), (0, 0), 26)
        if y0 > 0:                       # fade the top edge into the picture
            t = np.linspace(0, 1, min(200, y1 - y0), dtype=np.float32)[:, None]
            mask[:t.shape[0]] *= t
        m = (mask * strength)[:, :, None]
        frame[y0:y1, x0:x1] *= 1.0 - m

    def label(self, frame: np.ndarray, text: str, *, opacity: float = 1.0,
              colour=None, at: tuple[int, int] | None = None) -> None:
        x, y = at if at is not None else self.g.label
        layer = D.text_rgba([text], self.g.label_size, weight="mono",
                            colour=colour or B.CYAN, tracking=0.10)
        D.put(frame, layer, x, y, opacity)

    def caption(self, frame: np.ndarray, lines: list[str], *, opacity: float = 1.0,
                rise: float = 0.0, colour=None, size: int | None = None,
                at: tuple[int, int] | None = None, align: str = "left",
                max_width: int | None = None) -> None:
        """Draw the headline, shrinking it until it fits the space it has."""
        g = self.g
        x, y = at if at is not None else g.cap
        size = size or g.cap_size
        if max_width is None:
            max_width = g.w - 2 * g.margin if align != "center" else int(g.w * 0.9)
            if align == "left":
                max_width = g.w - x - g.margin
        size = D.fit_size(lines, size, weight="bold", max_width=max_width)
        self.audit[lines[0]] = (size, max_width)
        layer = D.text_rgba(lines, size, weight="bold", colour=colour or B.TEXT,
                            leading=1.20, align=align)
        if align == "center":
            x -= layer.shape[1] // 2
        D.put(frame, layer, int(x), int(y + rise * size * 2.2), opacity)

    def accent_rule(self, frame: np.ndarray, x: int, y: int, length: float,
                    *, opacity: float = 1.0, colour=None) -> None:
        """A short brand-coloured rule, used as a title marker."""
        c = colour or B.CYAN
        n = max(1, int(length))
        D.rect(frame, x, y, n, max(2, self.g.label_size // 9), c, radius=0,
               alpha=0.9 * opacity)

    def room(self, frame: np.ndarray, rect: tuple[int, int, int, int],
             picture: np.ndarray, *, radius: int = 6, glow: float = 0.55,
             bezel: float = 0.0) -> None:
        """Put a picture on a screen in a dark room, lit by the picture itself."""
        x, y, w, h = rect
        if bezel > 0:
            D.rect(frame, x - int(bezel), y - int(bezel),
                   w + 2 * int(bezel), h + 2 * int(bezel), (4, 5, 10),
                   radius=radius + 2, alpha=0.96)
        # The picture lights the wall around it, exactly as the ambilight does:
        # the border colours of the frame, blurred outward.
        ring = D.border_colours(picture, max(6, h // 40))
        canvas = np.zeros_like(frame)
        canvas[y:y + h, x:x + w] = ring
        for sigma, weight in ((h * 0.09, 0.70), (h * 0.26, 0.50), (h * 0.55, 0.30)):
            if weight * glow < 0.01:
                continue
            frame += D.big_blur(canvas, sigma) * (weight * glow)
        mask = D.rounded_mask(w, h, radius)
        D.paste_masked(frame, picture, x, y, mask)

    # -- shots -------------------------------------------------------------
    def _flat(self, t: float) -> np.ndarray:
        """The problem: a game capture with no depth at all."""
        shot = self.shots_depth["cyberpunk"]
        base, _ = shot.render(0.0, 0.0, 0.045)
        x, y, w, h = self.g.stage

        if not self.g.vertical:
            # A monitor switching on: the picture opens out from a bright line.
            reveal = D.ease(t / 0.62)
            if reveal < 0.999:
                vis = max(2, int(self.h * reveal))
                strip = cv2.resize(base, (self.w, vis), interpolation=cv2.INTER_LANCZOS4)
                frame = self.background()
                y = (self.h - vis) // 2
                frame[y:y + vis] = strip
                edge = int(18 * (1 - reveal)) + 1
                for dy in range(edge):
                    a = 0.5 * (1 - dy / max(edge, 1))
                    for yy in (y - dy - 1, y + vis + dy):
                        if 0 <= yy < self.h:
                            frame[yy] *= 1 - a
                            frame[yy] += np.array(B.CYAN, np.float32) / 255.0 * a
            else:
                frame = base.copy()
        else:
            frame = self.background()
            grow = D.ease(min(t / 0.62, 1.0))
            sw = int(w * (0.35 + 0.65 * grow))
            sh = int(h * (0.35 + 0.65 * grow))
            pic = cv2.resize(base, (sw, sh), interpolation=cv2.INTER_LANCZOS4)
            self.room(frame, (x + (w - sw) // 2, y + (h - sh) // 2, sw, sh),
                      pic, radius=4, glow=0.45 * grow)

        frame *= self._vig
        if not self.g.vertical:
            self.scrim(frame, 0, self.h - 340, self.w, 340, 0.80)
        self.label(frame, "CYBERPUNK 2077", opacity=D.ramp(t, 0.5, 1.1))
        self.accent_rule(frame, self.g.label[0], self.g.label[1] + self.g.label_size + 12,
                         42 * D.ramp(t, 0.5, 1.1))
        self.caption(frame, [self.caps["flat"][0]], opacity=D.ramp(t, 0.85, 1.5),
                     rise=1.0 - D.ramp(t, 0.85, 1.5))
        return frame

    def _depth(self, t: float) -> np.ndarray:
        """Depth estimation: the wipe that shows the app's real depth map."""
        shot = self.shots_depth["cyberpunk"]
        base, _ = shot.render(0.0, 0.0, 0.045)
        dep = shot.depth_colour()
        sw = base.shape[1]

        wipe_in = D.ease(D.clamp((t - 0.35) / 1.15))
        wipe_out = D.ease(D.clamp((t - 2.60) / 1.10))
        keep = D.clamp(1.0 - wipe_in + wipe_out)      # how much depth is showing
        split = int(keep * sw)

        picture = base.copy()
        if split < sw:
            picture[:, split:] = dep[:, split:]
            if 0 < split < sw:
                picture[:, max(0, split - 2):split + 2] = \
                    np.array(B.CYAN, np.float32)[None, None, :] / 255.0

        # The depth edges stay a moment after the map has gone, then fade.
        linger = D.ease(D.clamp((t - 2.70) / 0.50)) * \
            (1.0 - D.ease(D.clamp((t - 3.30) / 0.50)))
        if linger > 0.01:
            gx = cv2.Sobel(shot.depth, cv2.CV_32F, 1, 0, ksize=3)
            gy = cv2.Sobel(shot.depth, cv2.CV_32F, 0, 1, ksize=3)
            mag = cv2.GaussianBlur(np.hypot(gx, gy), (0, 0), 1.2)
            mag = np.clip(mag / max(float(np.percentile(mag, 97)), 1e-5), 0, 1)
            c = np.array(B.CYAN, np.float32)[None, None, :] / 255.0
            picture += mag[:, :, None] * c * (0.85 * linger)

        if self.g.vertical:
            frame = self.background()
            x, y, w, h = self.g.stage
            self.room(frame, (x, y, w, h), picture, radius=4, glow=0.28)
        else:
            frame = picture

        frame *= self._vig
        if not self.g.vertical:
            self.scrim(frame, 0, self.h - 330, self.w, 330, 0.76)
        g = self.g
        if keep > 0.06:
            pair = [("GAME", 0.055, B.TEXT), ("DEPTH", 0.80, B.CYAN)]
            for txt, frac, colour in pair:
                lay = D.text_rgba([txt], g.label_size, weight="mono",
                                  colour=colour, tracking=0.12)
                x = int(g.stage[0] + g.stage[2] * frac)
                D.put(frame, lay, x, g.stage[1] + g.stage[3] - int(46 * g.text),
                      0.9 * min(keep * 2.2, 1.0))
        self.label(frame, "VRX · DEPTH", opacity=D.ramp(t, 0.2, 0.7))
        self.caption(frame, list(self.caps["depth"]), opacity=D.ramp(t, 0.25, 0.95),
                     rise=1.0 - D.ramp(t, 0.25, 0.95))
        # A real, checkable figure from the project's own measurements.
        note = D.text_rgba(["ZipDepth — 2.0 ms per estimate on an RTX 3090"],
                           int(24 * g.text), weight="regular", colour=B.DIM)
        op = D.ramp(t, 1.7, 2.3) * (1 - D.ramp(t, 3.4, 3.8))
        D.put(frame, note, g.label[0], self.h - int(96 * g.text), op)
        return frame

    def _window(self, t: float) -> np.ndarray:        return frame

    def _window(self, t: float) -> np.ndarray:
        """The payoff: three real captures, warped by their real depth maps."""
        keys = ["cyberpunk", "helldivers", "system-shock"]
        each = SHOTS[2].seconds / 3.0
        idx = min(2, int(t / each))
        local0 = t - idx * each
        frame = self._window_one(keys[idx], local0 / each)

        # Cross-dissolve the last 0.3 s of each block into the next.
        gap = 0.30
        if local0 > each - gap and idx < 2:
            nxt = self._window_one(keys[idx + 1], 0.0)
            a = (local0 - (each - gap)) / gap
            frame = frame * (1 - a) + nxt * a
        return frame

    def _window_one(self, key: str, p: float) -> np.ndarray:
        shot = self.shots_depth[key]
        tx, ty, tz = shot.camera_path(p)
        base, _ = shot.render(tx, ty, tz)

        # Over the last beat the camera pulls back and the picture resolves into
        # a screen standing in the dark room, which is where the next shot lives.
        if key == "system-shock":
            pb = D.ease(D.clamp((p - 0.74) / 0.26))
        else:
            pb = 0.0

        if self.g.vertical:
            frame = self.background()
            x, y, w, h = self.g.stage
            s = 1.0 - 0.10 * pb
            sw, sh = int(w * s), int(h * s)
            pic = cv2.resize(base, (sw, sh), interpolation=cv2.INTER_AREA)
            self.room(frame, (x + (w - sw) // 2, y + (h - sh) // 2, sw, sh), pic,
                      radius=4, glow=0.25 + 0.5 * pb)
            lb = self.g.label[0]
            self.caption(frame, [self.caps["window"][0]], opacity=D.ramp(p, 0.06, 0.20) *
                         (1 - D.ramp(p, 0.90, 1.0)), at=(lb, int(self.h * 0.115)))
        else:
            s = 1.0 - 0.42 * pb
            sw, sh = int(self.w * s), int(self.h * s)
            if pb < 0.01:
                frame = base
            else:
                pic = cv2.resize(base, (sw, sh), interpolation=cv2.INTER_AREA)
                frame = self.background()
                self.room(frame, ((self.w - sw) // 2, (self.h - sh) // 2, sw, sh),
                          pic, radius=4, glow=0.55)
            frame *= self._vig
            if pb < 0.6:
                self.scrim(frame, 0, self.h - 320, self.w, 320, 0.74 * (1 - 0.7 * pb))
            self.caption(frame, [self.caps["window"][0]],
                         opacity=D.ramp(p, 0.08, 0.24) * (1 - D.ramp(p, 0.86, 0.98)),
                         rise=1.0 - D.ramp(p, 0.08, 0.24))

        names = {"cyberpunk": "CYBERPUNK 2077", "helldivers": "HELLDIVERS 2",
                 "system-shock": "SYSTEM SHOCK"}
        self.label(frame, names[key],
                   opacity=D.ramp(p, 0.04, 0.16) * (1 - D.ramp(p, 0.92, 1.0)))
        return frame

    def _room(self, t: float) -> np.ndarray:
        """What it looks like in the headset: the real capture, whole."""
        g = self.g
        frame = self.background()
        # A slow push in, staying inside the capture's own resolution.
        zoom = 1.0 + 0.045 * D.ease(t / SHOTS[3].seconds)
        if g.vertical:
            box_w, box_h = int(g.w * 0.90), int(g.h * 0.42)
        else:
            # Leaves the right-hand column ~690 px for the headline.
            box_w, box_h = int(g.w * 0.48), int(g.h * 0.78)
        pic = D.fit_inside(self.headset, int(box_w * zoom), int(box_h * zoom))
        ph, pw = pic.shape[:2]
        if g.vertical:
            px, py = (g.w - pw) // 2, int(g.h * 0.335)
        else:
            px, py = g.margin, (g.h - ph) // 2
        self.room(frame, (px, py, pw, ph), pic, radius=8, glow=0.5)
        frame *= self._vig

        op = D.ramp(t, 0.15, 0.85)
        if g.vertical:
            cap_at = (g.margin, int(g.h * 0.075))
            size = int(g.cap_size * 0.92)
            chips_at = (g.margin, py + ph + int(g.h * 0.045))
        else:
            box_w, box_h = int(g.w * 0.48), int(g.h * 0.78)
            cap_at = (px + pw + 78, 250)
            size = int(g.cap_size * 0.80)
            chips_at = (px + pw + 78, 520)
        self.caption(frame, list(self.caps["room"]), opacity=op, at=cap_at, size=size)
        self.accent_rule(frame, cap_at[0], cap_at[1] - 34, 54 * op)

        chips = ["Curved screen", "Ambilight", "Room lighting", "Reflections",
                 "Glass walls"]
        cx, cy = chips_at
        for i, chip in enumerate(chips):
            a = D.ramp(t, 1.5 + i * 0.95, 2.2 + i * 0.95)
            if a <= 0.01:
                continue
            layer = D.text_rgba([chip], int(28 * g.text), weight="regular",
                                colour=B.TEXT if i % 2 == 0 else B.MUTED)
            D.put(frame, layer, cx, cy + i * int(52 * g.text), a)

        note = D.text_rgba(["Helldivers 2, seen through the headset"], int(24 * g.text),
                           colour=B.DIM)
        D.put(frame, note, px, py + ph + 22, D.ramp(t, 0.6, 1.2) * 0.95)
        self.label(frame, "IN THE HEADSET", opacity=D.ramp(t, 0.15, 0.6))
        return frame

    def _easy(self, t: float) -> np.ndarray:
        """Starting a game: the real Easy-mode window, untouched."""
        g = self.g
        frame = self.background()
        op = D.ramp(t, 0.1, 0.7)

        if g.vertical:
            box_w = int(g.w * 0.90)
            cap_at = (g.margin, int(g.h * 0.14))
            ui_y = int(g.h * 0.44)
        else:
            box_w = int(g.w * 0.52)
            cap_at = (g.margin, 240)
            ui_y = 430
        scale = box_w / self.ui_rgb.shape[1]
        uw = int(self.ui_rgb.shape[1] * scale)
        uh = int(self.ui_rgb.shape[0] * scale)
        ui = cv2.resize(self.ui_rgb, (uw, uh), interpolation=cv2.INTER_LANCZOS4)
        ua = cv2.resize(self.ui_a, (uw, uh), interpolation=cv2.INTER_LINEAR)
        ux = (g.w - uw) // 2 if g.vertical else g.margin
        ui_y = ui_y + int(26 * (1 - op))

        self.room(frame, (ux, ui_y, uw, uh), ui, radius=10, glow=0.55)
        # The capture carries its own rounded corners in its alpha channel; the
        # dark window frame behind them is what closes the shape off.
        mask = D.rounded_mask(uw, uh, 10) * ua
        D.paste_masked(frame, ui, ux, ui_y, mask)

        self.caption(frame, list(self.caps["easy"]), opacity=D.ramp(t, 0.25, 1.0), at=cap_at)
        self.accent_rule(frame, cap_at[0], cap_at[1] - 34,
                        54 * D.ramp(t, 0.25, 1.0))

        steps = ["START YOUR GAME", "VRX ATTACHES", "PLAY"]
        if g.vertical:
            sx, sy = g.margin, ui_y + uh + int(g.h * 0.055)
            spacing = int(g.w * 0.30)
        else:
            # Below the window, not across it: the window runs to ui_y + uh.
            sx, sy = g.margin, ui_y + uh + int(66 * g.text)
            spacing = int(g.w * 0.16)
        for i, step in enumerate(steps):
            a = D.ramp(t, 1.5 + i * 1.05, 2.1 + i * 1.05)
            colour = B.CYAN if i == 0 else (B.TEXT if a > 0.5 else B.MUTED)
            layer = D.text_rgba([step], int(26 * g.text), weight="mono",
                                colour=colour, tracking=0.08)
            D.put(frame, layer, sx + i * spacing, sy, a)
            dot_x = sx + i * spacing
            D.rect(frame, dot_x, sy - int(26 * g.text), int(10 * g.text),
                   int(10 * g.text), B.CYAN if a > 0.5 else B.DIM, radius=5, alpha=a)
            if i < 2:
                D.rect(frame, dot_x + int(150 * g.text), sy - int(21 * g.text),
                       int(spacing - 170 * g.text), 2, B.DIM, alpha=0.35 * a)
        self.label(frame, "EASY MODE", opacity=D.ramp(t, 0.1, 0.5))
        return frame

    def _end(self, t: float) -> np.ndarray:
        """The card: the real logo, the site's tagline, where to get it."""
        g = self.g
        frame = self.background()
        dur = SHOTS[5].seconds
        fade_in = D.ramp(t, 0.05, 0.75)
        fade_out = 1.0 - D.ramp(t, dur - 0.75, dur - 0.05)

        # A quiet glow behind the mark, from the logo's own colours.
        cy = int(g.h * (0.30 if g.vertical else 0.36))
        cx = g.w // 2
        logo_s = int(g.w * (0.30 if g.vertical else 0.16))
        lg = cv2.resize(self.logo_rgb, (logo_s, logo_s), interpolation=cv2.INTER_AREA)
        la = cv2.resize(self.logo_a, (logo_s, logo_s), interpolation=cv2.INTER_AREA)
        canvas = np.zeros_like(frame)
        canvas[cy:cy + logo_s, cx - logo_s // 2:cx - logo_s // 2 + logo_s] = lg * la[:, :, None]
        frame += D.big_blur(canvas, logo_s * 0.16) * 0.45 * fade_in
        D.paste_masked(frame, lg, cx - logo_s // 2, cy, la, fade_in)

        ty = cy + logo_s + int(g.h * (0.045 if g.vertical else 0.055))
        self.caption(frame, list(self.caps["end"]),
                     opacity=D.ramp(t, 0.45, 1.05) * fade_out,
                     colour=B.TEXT, size=int(g.cap_size * (0.86 if g.vertical else 1.0)),
                     at=(cx, ty), align="center")
        ty2 = ty + int(g.cap_size * 2.7)
        self.caption(frame, ["Free and open source · vrx3d.uk"],
                     opacity=D.ramp(t, 0.9, 1.5) * fade_out, colour=B.CYAN,
                     size=int(34 * g.text), at=(cx, ty2), align="center")
        self.caption(frame, ["Windows 10/11 · DirectX 12 GPU · OpenXR runtime"],
                     opacity=D.ramp(t, 1.3, 1.9) * fade_out, colour=B.MUTED,
                     size=int(27 * g.text), at=(cx, ty2 + int(58 * g.text)),
                     align="center")
        self.caption(frame, ["Download for Windows"],
                     opacity=D.ramp(t, 1.7, 2.3) * fade_out, colour=B.TEXT,
                     size=int(30 * g.text), at=(cx, ty2 + int(140 * g.text)),
                     align="center")
        frame *= self._vig
        return frame * fade_out

    # -- the film ----------------------------------------------------------
    @property
    def caps(self) -> dict:
        if not hasattr(self, "_caps"):
            self._caps = {s.key: s.caption for s in SHOTS}
        return self._caps

    def _shot(self, key: str, t_local: float) -> np.ndarray:
        return {
            "flat": self._flat, "depth": self._depth, "window": self._window,
            "room": self._room, "easy": self._easy, "end": self._end,
        }[key](t_local)

    def frame_at(self, t: float) -> np.ndarray:
        t = min(max(t, 0.0), TOTAL_SECONDS)
        starts, acc = {}, 0.0
        for s in SHOTS:
            starts[s.key] = acc
            acc += s.seconds

        # A short dissolve where a hard cut would jar; the depth and window shots
        # continue the same picture, so they cut.
        dissolves = {"room": 0.45, "easy": 0.45, "end": 0.55}
        for i, s in enumerate(SHOTS):
            if t < starts[s.key] + s.seconds:
                frame = self._shot(s.key, t - starts[s.key])
                d = dissolves.get(s.key)
                if d and t < starts[s.key] + d:
                    prev = SHOTS[i - 1]
                    a = (t - starts[s.key]) / d
                    before = self._shot(prev.key, prev.seconds - 0.001)
                    frame = before * (1 - a) + frame * a
                return np.clip(frame, 0.0, 1.0)
        return np.clip(self._shot("end", SHOTS[-1].seconds), 0.0, 1.0)
