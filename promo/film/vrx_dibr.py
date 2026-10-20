"""Depth-image-based rendering for the VRX promo.

Takes a real VRX capture and the app's real depth map for it, and renders new
views from a virtual camera: a proper perspective camera translation, solved per
pixel by fixed-point iteration so the parallax is sub-pixel and the near field
moves further than the far field, exactly as a stereo pair would.

How it works
------------
The app's depth map gives each pixel a distance. Every pixel is treated as a
point on a plane at that distance, in front of the camera, and the camera is
then moved by (tx, ty, tz). A point at normalised screen position (x, y) and
camera-space depth Z projects to

    u' = (x * Z - tx * Z0) / (Z - tz)
    v' = (y * Z - ty * Z0) / (Z - tz)

The renderer works backwards, from output pixel to source pixel, because a
backward map leaves no unfilled gaps by construction. The catch is that Z in
those equations is the depth at the *source* pixel, which is what we are trying
to find, so each pixel is solved by fixed-point iteration (a few passes are
enough; the depth field is smooth). Where the iteration cannot satisfy the
forward projection - the sliver revealed at the edge of a near object as the
camera moves - the pixel is marked as a disocclusion and filled by inpainting.

Nothing is invented: every frame is the real capture, repositioned by the real
depth the product computed for it.
"""
from __future__ import annotations

import math
from pathlib import Path

import cv2
import numpy as np

from . import vrx_brand as B


def _smoothstep(edge0: float, edge1: float, x: float) -> float:
    """Hermite ramp from 0 at edge0 to 1 at edge1, flat at both ends."""
    if edge1 <= edge0:
        return 1.0 if x >= edge1 else 0.0
    t = min(max((x - edge0) / (edge1 - edge0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def _cover_transform(src_w: int, src_h: int, out_w: int, out_h: int, bias: float):
    """Scale-and-crop so a source fills the output. Returns (scale, nw, nh, x0, y0)."""
    s = max(out_w / src_w, out_h / src_h)
    nw, nh = int(round(src_w * s)), int(round(src_h * s))
    x0 = (nw - out_w) // 2
    y0 = int(round((nh - out_h) * bias))
    return s, nw, nh, x0, y0


def load_depth_map(name: str) -> np.ndarray:
    """The app's depth map for a capture, as float32 in [0, 1] with 1 = nearest.

    The convention was measured, not assumed: tools/analyse_depth.py shows the
    brightest depth sits on the bottom rows (the ground under the camera) and the
    darkest in the upper middle (the sky), for all three published pairs.
    """
    p = B.ASSETS / f"{name}-depth.png"
    d = cv2.imread(str(p), cv2.IMREAD_GRAYSCALE)
    if d is None:
        raise FileNotFoundError(p)
    return d.astype(np.float32) / 255.0


def load_capture(name: str) -> np.ndarray:
    p = B.ASSETS / f"{name}.jpg"
    img = cv2.imread(str(p), cv2.IMREAD_COLOR)
    if img is None:
        raise FileNotFoundError(p)
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0


class DepthShot:
    """A capture plus its depth map, ready to render from any camera position.

    near/far are camera distances in units of the screen plane distance (Z0 = 1),
    so the depth range is expressed the way the app thinks about it: how far in
    front of the screen the nearest and furthest things sit.
    """

    def __init__(self, name: str, out_w: int, out_h: int, *, crop_bias: float = 0.5,
                 near: float = 0.55, far: float = 5.0, smooth: float = 1.1,
                 depth_curve: float = 1.0):
        self.name = name
        self.out_w, self.out_h = out_w, out_h

        colour = load_capture(name)
        depth = load_depth_map(name)
        ch, cw = colour.shape[:2]
        _, nw, nh, x0, y0 = _cover_transform(cw, ch, out_w, out_h, crop_bias)
        up = cv2.resize(depth, (nw, nh), interpolation=cv2.INTER_LINEAR)
        depth = up[y0:y0 + out_h, x0:x0 + out_w]
        colour = cv2.resize(colour, (nw, nh), interpolation=cv2.INTER_CUBIC)
        colour = colour[y0:y0 + out_h, x0:x0 + out_w]

        if smooth > 0:
            depth = cv2.GaussianBlur(depth, (0, 0), smooth)
        depth = np.clip(depth, 0.0, 1.0) ** depth_curve

        self.colour = np.ascontiguousarray(colour.astype(np.float32))
        self.depth = np.ascontiguousarray(depth.astype(np.float32))
        # Camera-space distance. Disparity is 1/Z, so blend in 1/Z, not in Z.
        inv = (1.0 - self.depth) * (1.0 / far) + self.depth * (1.0 / near)
        self.Z = np.ascontiguousarray((1.0 / inv).astype(np.float32))
        self.near, self.far = near, far

        yy, xx = np.mgrid[0:out_h, 0:out_w].astype(np.float32)
        self._px = xx
        self._py = yy
        # Normalised screen coordinates, both axes scaled by width so tx is in
        # units of screen widths and the aspect ratio is preserved.
        self._u = (xx - (out_w - 1) / 2.0) / out_w
        self._v = (yy - (out_h - 1) / 2.0) / out_w

    # -- camera ------------------------------------------------------------
    def maps(self, tx: float, ty: float, tz: float, *, iterations: int = 9,
             damping: float = 0.75, hole_tol: float = 1.2, tol: float = 0.03):
        """Solve the backward map for a camera translated by (tx, ty, tz).

        Returns (map_x, map_y, holes): source pixel coordinates per output pixel,
        and a boolean mask of the slivers revealed by the move.
        """
        u, v = self._u, self._v
        Z0 = 1.0

        # Start from the screen plane solved once at the output position: most
        # of a game frame is far away, so this is already close, and the loop
        # below then only has to correct for the near field.
        z = np.maximum(self._sample(self.Z, u, v), 1e-3)
        x = (u * (z - tz) + tx * Z0) / z
        y = (v * (z - tz) + ty * Z0) / z

        for _ in range(iterations):
            z = np.maximum(self._sample(self.Z, x, y), 1e-3)
            x_new = (u * (z - tz) + tx * Z0) / z
            y_new = (v * (z - tz) + ty * Z0) / z
            if damping < 1.0:
                x_new = damping * x_new + (1.0 - damping) * x
                y_new = damping * y_new + (1.0 - damping) * y
            shift = np.maximum(np.abs(x_new - x), np.abs(y_new - y)) * self.out_w
            x, y = x_new, y_new
            # Testing a strided subsample keeps this cheap: a full percentile of
            # two million values costs more than the remap it is deciding about.
            if np.percentile(shift[::4, ::4], 99.7) < tol:
                break

        # A pixel solved correctly must project forward to where it came from.
        z = np.maximum(self._sample(self.Z, x, y), 1e-3)
        den = np.where(np.abs(z - tz) < 1e-3, 1e-3, z - tz)
        u_fwd = (x * z - tx * Z0) / den
        v_fwd = (y * z - ty * Z0) / den
        err = np.maximum(np.abs(u_fwd - u), np.abs(v_fwd - v)) * self.out_w

        map_x = (x * self.out_w + (self.out_w - 1) / 2.0).astype(np.float32)
        map_y = (y * self.out_w + (self.out_h - 1) / 2.0).astype(np.float32)
        # Half a pixel of slack: the maps are float32 and a sample landing a
        # ten-thousandth of a pixel outside is not a disocclusion, it is rounding.
        inside = (map_x >= -0.5) & (map_x <= self.out_w - 0.5) & \
                 (map_y >= -0.5) & (map_y <= self.out_h - 0.5)
        holes = (err > hole_tol) | (~inside)
        return map_x, map_y, holes

    def _sample(self, img: np.ndarray, x, y) -> np.ndarray:
        """Bilinear sample `img` at normalised screen coordinates."""
        mx = (x * self.out_w + (self.out_w - 1) / 2.0).astype(np.float32)
        my = (y * self.out_w + (self.out_h - 1) / 2.0).astype(np.float32)
        return cv2.remap(img, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)

    # -- rendering ---------------------------------------------------------
    def render(self, tx: float, ty: float, tz: float, *, inpaint_radius: int = 3,
               **kw) -> np.ndarray:
        """Render one frame (RGB float32 in [0, 1]) from a camera position."""
        map_x, map_y, holes = self.maps(tx, ty, tz, **kw)
        out = cv2.remap(self.colour, map_x, map_y, cv2.INTER_LINEAR,
                        borderMode=cv2.BORDER_REPLICATE)
        n_holes = int(holes.sum())
        if n_holes:
            mask = holes.astype(np.uint8) * 255
            # A one-pixel dilation stops the fill from dragging the old edge in.
            mask = cv2.dilate(mask, np.ones((3, 3), np.uint8), iterations=1)
            bgr = cv2.cvtColor((np.clip(out, 0, 1) * 255).astype(np.uint8), cv2.COLOR_RGB2BGR)
            filled = cv2.inpaint(bgr, mask, inpaint_radius, cv2.INPAINT_TELEA)
            out = cv2.cvtColor(filled, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
        return out, n_holes

    def depth_colour(self) -> np.ndarray:
        """The app's depth map as a duotone, in the brand's own colours.

        Nearest is the cyan accent, the mid ground is the violet accent and the
        far field sinks into the deep background, so the picture stays readable
        as a depth map and still belongs to the product's palette.
        """
        inv = 1.0 / self.Z
        # Stretch to the depth that is actually present, or the map is a flat
        # wash: these maps put most of the frame in the far field.
        lo, hi = np.percentile(inv, [1.0, 99.0])
        t = np.clip((inv - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
        t = t[:, :, None]                        # 0 = nearest, 1 = furthest
        stops = ((0.0, B.CYAN), (0.55, B.VIOLET), (1.0, (16, 14, 38)))
        out = np.zeros_like(self.colour)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            a0 = np.array(c0, np.float32)[None, None, :] / 255.0
            a1 = np.array(c1, np.float32)[None, None, :] / 255.0
            span = max(t1 - t0, 1e-6)
            f = np.clip((t - t0) / span, 0.0, 1.0)
            seg = a0 * (1 - f) + a1 * f
            out = np.where((t >= t0) & (t <= t1), seg, out)
        out = np.where(t < 0.0, np.array(B.CYAN, np.float32)[None, None, :] / 255.0, out)
        return out.astype(np.float32)

    def shift_profile(self, tx: float, tz: float = 0.0) -> dict:
        """How far the picture actually moves at each depth, in output pixels.

        Bands are percentiles of the depth that is actually present, not absolute
        depth values: these maps are skewed towards the far field, so a fixed
        threshold would measure an empty band and report a meaningless zero.
        """
        map_x, _, holes = self.maps(tx, 0.0, tz)
        dx = np.abs(map_x - self._px)
        d = self.depth
        q = np.percentile(d, [0, 10, 45, 55, 95, 100])
        out: dict[str, float] = {}
        for label, lo, hi in [("nearest 5%", q[4], q[5] + 1e-6),
                              ("far 10%", q[0], q[1] + 1e-6),
                              ("middle 10%", q[2], q[3] + 1e-6)]:
            m = (d >= lo) & (d <= hi)
            out[label] = round(float(dx[m].mean()), 2) if m.any() else 0.0
        out["mean"] = round(float(dx.mean()), 2)
        out["max"] = round(float(dx.max()), 2)
        out["far_near_ratio"] = round(out["nearest 5%"] / max(out["far 10%"], 1e-6), 1)
        out["holes_pct"] = round(100.0 * holes.mean(), 3)
        return out

    # -- choosing the camera move ------------------------------------------
    def max_lateral(self, tz: float, margin: float = 0.85) -> float:
        """The largest lateral move that keeps every sample inside the capture.

        The source position for output u' is x = u' * (Z - tz) / Z + tx / Z, so at
        the output's right edge the sample sits at 0.5 + (tx - 0.5*tz)/Z. Moving
        the camera towards the screen (tz > 0) pulls that edge inward and pays for
        the lateral move; the binding case is the far field, where Z is large and
        the magnification has faded, giving |tx| <= 0.5 * tz. Staying under that
        means no strip at the frame edge is ever left without a source, so the
        only thing left to fill is the thin sliver behind a near object.
        """
        return margin * 0.5 * abs(tz)

    def camera_path(self, p: float, *, dolly_in: float = 0.085, dolly_out: float = 0.045,
                    drift: float = 0.020, waves: float = 1.0, lift: float = 0.007,
                    phase: float = 0.0) -> tuple[float, float, float]:
        """A camera move for a shot, as a function of progress p in [0, 1].

        A slow dolly towards the screen throughout - which is what keeps the frame
        edges supplied with real pixels - with a lateral drift, out and back, and
        a gentle lift. The drift is eased in and out so a shot arrives settled and
        leaves settled, and cuts between shots read as deliberate.
        """
        tz = dolly_out + (dolly_in - dolly_out) * p
        amp = min(drift, self.max_lateral(tz))
        env = _smoothstep(0.0, 0.20, p) * (1.0 - _smoothstep(0.80, 1.0, p))
        a = 2 * math.pi * waves * p + phase
        tx = amp * env * math.sin(a)
        ty = lift * env * math.sin(a + math.pi / 2)
        return tx, ty, tz
