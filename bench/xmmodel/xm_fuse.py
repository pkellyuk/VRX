"""Two-model depth fusion (xmmodel), step 2: simulate fusion schemes and score them.

Replays a clip cached by xm_cache.py the way the engine would see it live:
  * ZipDepth (2 ms) delivers depth for EVERY frame, with no lag;
  * Depth Anything V2 (13 ms on the 3090; slower on a second GPU or when the game
    is busy) runs continuously in the background: when it finishes, it starts on the
    newest captured frame, and each result becomes usable `lat` ms after its frame.

Methods (all end in the engine's 0..1 "near" domain that drives disparity):
  zip            ZipDepth alone (today's default)
  dav2_ideal     DA-V2 on every frame with zero lag - unachievable; the quality target
  dav2_live      DA-V2 alone as it would really look: its newest result, held
  avg            naive fusion: ZipDepth globally matched to DA-V2, averaged with it
  anchor_g       ZipDepth with ONE scale/shift fitted to the newest DA-V2 result
  anchor_sN      ZipDepth with a smooth per-region scale/shift field (Gaussian
                 window sigma N px on the 686x392 grid) fitted to the newest DA-V2
                 result - a guided filter with ZipDepth as the guide. Fine detail and
                 edges come from the current ZipDepth frame; the large-scale layout
                 comes from DA-V2.
  anchor_sN_conf anchor_sN, plus: where the two models still disagree after the fit,
                 local relief is flattened towards a blurred base (less depth where
                 neither model is trustworthy).
  curve          ZipDepth through ONE monotonic tone curve fitted to the newest DA-V2
                 result ("ZipDepth says x -> DA-V2 says y"). It is not tied to screen
                 positions, so it cannot lag behind moving objects.
  dav2_mc        DA-V2 alone, but its lagged result is moved to the current frame
                 with block motion vectors (8x8 blocks - what D3D12's hardware
                 ID3D12VideoMotionEstimator provides; simulated with Farneback flow
                 averaged per block)
  mc_sN          ZipDepth for the current frame fitted, EVERY frame, to the motion-
                 compensated DA-V2 (dav2_mc) with a per-region field of sigma N px
  mc_sN_conf     mc_sN with uncertain regions flattened (as anchor_sN_conf)
  dav2_mc_hw,    as dav2_mc / mc_sN, but with REAL hardware motion vectors from the
  mc_sN_hw       GPU's motion estimator (bench/native/xmmodel/me_probe.exe --pairs),
                 present when <clip>/me_<lat>/vectors.bin exists (see --export-mv)
  mc_sN_hwv      mc_sN_hw, VERIFIED: the older frame's picture is moved with the same
                 vectors and compared with the current picture. Where they match, the
                 fit follows the moved DA-V2; where they don't (fast pans, occlusions,
                 blocks the estimator got wrong) it follows ZipDepth mapped globally
                 onto DA-V2's scale; if most of the frame fails (a whip-pan or cut) the
                 whole frame does.
  zip_stab       ZipDepth steadied against ITSELF: the previous output is moved to the
                 current frame with hardware vectors (frame i vs i-1) and blended in
                 where motion trust is high. No second model. Present when
                 <clip>/me_prev/vectors.bin exists (see --export-prev).
  mc_sN_hwv_stab mc_sN_hwv steadied the same way.

Scores (lower is better except edge):
  struct     scale/shift-invariant mean abs error against dav2_ideal (near units)
  struct_nr  the same, only where dav2_ideal is near (> 0.6): characters, weapons
  flicker    motion-compensated frame-to-frame change (Farneback flow, forward/
             backward consistent pixels only) divided by the frame's depth spread
             (95th - 5th percentile), so squashing depth cannot score as steadiness
  edge       fraction of the strongest depth edges lying within 2 px of an edge in
             the CURRENT colour frame: lagged or blurred depth scores lower

usage (from bench/):
    .venv-dml/Scripts/python.exe xmmodel/xm_fuse.py <clip_dir> [--lat=33,67,133]
        [--sigma=24,48,96] [--show-lat=67] [--show-sigma=48] [--no-video] [--render-only]
    .venv-dml/Scripts/python.exe xmmodel/xm_fuse.py <clip_dir> --export-mv=LAT
        writes <clip_dir>/me_LAT/frames.y + pairs.txt for
        native/xmmodel/out/me_probe.exe --pairs=<clip_dir>/me_LAT [--adapter=N]
    .venv-dml/Scripts/python.exe xmmodel/xm_fuse.py <clip_dir> --export-prev=1
        the same for consecutive frames (i vs i-1), into <clip_dir>/me_prev
Writes <clip_dir>/results.json, sheet.png and compare.mp4 (for the show-* setting).
"""
import json
import math
import os
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xm_cache import GH, GW  # noqa: E402

WARMUP_S = 0.5            # ignore the first half second (smoothers settling)
TAU_RANGE = 0.4           # engine default --tau (xrapp5.cpp Options::smoothTau)
TAU_FIELD = 0.1           # smoothing of the fitted scale/shift field between anchors
EPS = 2e-3                # guided-filter regulariser (variance units of 0..1 depth)
FIT_SCALE = 4             # fit the field at 1/4 resolution; it is smooth by design
CONF_PCT = (85, 98)       # residual percentiles mapped to 0..1 disagreement: only the
                          # worst-agreeing ~15% of each frame is treated as unsure
CONF_STRENGTH = 0.75      # how much local relief is removed at full disagreement
CONF_BASE_SIGMA = 12      # px, the "flattened" base
CURVE_BINS = 32           # tone-curve resolution over 0..1 ZipDepth near values
CURVE_MIN_PX = 200        # bins with fewer pixels are interpolated from neighbours
MV_BLOCK = 8              # motion-vector block size (px on the depth grid)
VERIFY_LO = 0.04          # luma mismatch (0..1) still fully trusted (~10 grey levels)
VERIFY_HI = 0.12          # luma mismatch treated as a failed match (~30 grey levels)
VERIFY_CUT = 0.5          # if the mean trust is below this, trust nothing this frame
STAB_ALPHA = 0.5          # weight of the motion-compensated previous output (at full trust)
NEAR_THRESHOLD = 0.6


def log(msg):
    print(f'[{time.strftime("%H:%M:%S")}][xm_fuse] {msg}', flush=True)


# ------------------------------------------------------------ engine behaviour
class RangeSmoother:
    """Port of xrapp5.cpp SmoothRange: 0.5/99.5 percentiles, exponential smoothing
    with time constant tau, and an immediate snap on a large jump (scene cut)."""

    def __init__(self, tau):
        self.tau = tau
        self.have = False
        self.lo = self.hi = self.last = 0.0

    def update(self, raw, now):
        if raw is None or raw.size == 0:
            return 0.0, 1.0
        new_lo, new_hi = np.percentile(raw, (0.5, 99.5))
        if new_hi - new_lo < 1e-6:
            new_lo, new_hi = float(raw.min()), float(raw.max())
        rng = self.hi - self.lo
        cut = not self.have or (abs(new_lo - self.lo) + abs(new_hi - self.hi)) > 0.6 * rng
        if cut:
            self.lo, self.hi, self.have = float(new_lo), float(new_hi), True
        else:
            a = min(1.0, max(0.0, 1.0 - math.exp(-(now - self.last) / self.tau)))
            self.lo += a * (new_lo - self.lo)
            self.hi += a * (new_hi - self.hi)
        self.last = now
        return self.lo, self.hi


def to_near(raw, lo, hi):
    return np.clip((raw - lo) / max(hi - lo, 1e-6), 0.0, 1.0).astype(np.float32)


def anchor_schedule(times, lat_ms):
    """DA-V2 runs back to back: each run takes the newest captured frame and its
    result is usable lat_ms later. Returns a list of (arrival_time, frame_index)."""
    if times is None or len(times) == 0 or lat_ms <= 0:
        raise ValueError('bad schedule parameters')
    events, start, end = [], 0.0, float(times[-1])
    while start <= end:
        k = int(np.searchsorted(times, start + 1e-9, side='right')) - 1
        finish = start + lat_ms / 1000.0
        events.append((finish, max(k, 0)))
        start = finish
    return events


# ------------------------------------------------------------------ fusion
def blur(x, sigma):
    return cv2.GaussianBlur(x, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)


def global_fit(z, d):
    """Least-squares d ~ a*z + b over the whole map."""
    zm, dm = float(z.mean()), float(d.mean())
    var = float(((z - zm) ** 2).mean())
    a = float(((z - zm) * (d - dm)).mean()) / max(var, 1e-6)
    return a, dm - a * zm


def local_fit(z, d, sigma):
    """Smooth per-pixel scale/shift fields so that a*z + b ~ d locally (a guided
    filter with z as the guide), regularised towards the global fit where z is flat."""
    a_g, b_g = global_fit(z, d)
    if sigma is None:
        return np.full_like(z, a_g), np.full_like(z, b_g)
    sw, sh = GW // FIT_SCALE, GH // FIT_SCALE
    zs = cv2.resize(z, (sw, sh), interpolation=cv2.INTER_AREA)
    ds = cv2.resize(d, (sw, sh), interpolation=cv2.INTER_AREA)
    s = sigma / FIT_SCALE
    mz, md = blur(zs, s), blur(ds, s)
    var = blur(zs * zs, s) - mz * mz
    cov = blur(zs * ds, s) - mz * md
    a = (cov + EPS * a_g) / (var + EPS)
    b = md - a * mz
    a, b = blur(a, s), blur(b, s)
    return (cv2.resize(a, (GW, GH), interpolation=cv2.INTER_LINEAR),
            cv2.resize(b, (GW, GH), interpolation=cv2.INTER_LINEAR))


CURVE_X = (np.arange(CURVE_BINS, dtype=np.float32) + 0.5) / CURVE_BINS


def curve_fit(z, d):
    """Monotonic tone curve (CURVE_BINS points) so that curve(z) ~ d: per-bin mean
    of d, gaps interpolated, then forced non-decreasing so depth order is kept."""
    idx = np.clip((z * CURVE_BINS).astype(np.int32), 0, CURVE_BINS - 1).ravel()
    counts = np.bincount(idx, minlength=CURVE_BINS)
    sums = np.bincount(idx, weights=d.ravel(), minlength=CURVE_BINS)
    ok = counts >= CURVE_MIN_PX
    if ok.sum() < 2:
        a, b = global_fit(z, d)
        return (a * CURVE_X + b).astype(np.float32)
    y = np.interp(CURVE_X, CURVE_X[ok], sums[ok] / counts[ok])
    return np.maximum.accumulate(y).astype(np.float32)


def curve_apply(lut, z):
    return np.interp(z, CURVE_X, lut).astype(np.float32)


class AnchorState:
    """The fitted correction, smoothed across DA-V2 arrivals, plus the disagreement
    map. use_curve: a tone curve first; sigma: then an affine field (None = global)."""

    def __init__(self, sigma, use_curve=False):
        self.sigma = sigma
        self.use_curve = use_curve
        self.lut = self.a = self.b = self.conf = None
        self.last = 0.0

    def _ema(self, old, new, w):
        return new if old is None else old + w * (new - old)

    def update(self, z_k, d_k, now):
        w = 1.0 - math.exp(-(now - self.last) / TAU_FIELD)
        c_k = z_k
        if self.use_curve:
            lut = curve_fit(z_k, d_k)
            self.lut = self._ema(self.lut, lut, w)
            c_k = curve_apply(lut, z_k)
        if self.use_curve and self.sigma is None:
            a, b = np.ones_like(z_k), np.zeros_like(z_k)
        else:
            a, b = local_fit(c_k, d_k, self.sigma)
        self.a, self.b = self._ema(self.a, a, w), self._ema(self.b, b, w)
        self.last = now
        residual = np.abs(d_k - (a * c_k + b))
        r = blur(residual, 4)
        lo, hi = np.percentile(r, CONF_PCT)
        self.conf = np.clip((r - lo) / max(hi - lo, 1e-4), 0.0, 1.0).astype(np.float32)

    def apply(self, z_i):
        c = curve_apply(self.lut, z_i) if self.use_curve else z_i
        return np.clip(self.a * c + self.b, 0.0, 1.0)


def flatten_uncertain(f, conf):
    base = blur(f, CONF_BASE_SIGMA)
    return (base + (f - base) * (1.0 - CONF_STRENGTH * conf)).astype(np.float32)


# ------------------------------------------------------------------ scoring
def ssi_mae(out, ref, mask=None):
    """Mean abs error after the best global scale/shift of out onto ref."""
    o, r = (out, ref) if mask is None else (out[mask], ref[mask])
    if o.size < 100:
        return float('nan')
    a, b = global_fit(o, r)
    return float(np.abs(a * o + b - r).mean())


def edge_precision(out, colour_edges):
    gx = cv2.Sobel(out, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(out, cv2.CV_32F, 0, 1, ksize=3)
    mag = np.sqrt(gx * gx + gy * gy)
    thr = max(float(np.percentile(mag, 95)), 0.02)
    strong = mag > thr
    if strong.sum() < 50:
        return float('nan')
    return float(colour_edges[strong].mean())


FLOW_PARAMS = dict(pyr_scale=0.5, levels=4, winsize=21, iterations=3, poly_n=5, poly_sigma=1.1, flags=0)
GRID_X, GRID_Y = np.meshgrid(np.arange(GW, dtype=np.float32), np.arange(GH, dtype=np.float32))


def block_motion(g_cur, g_old):
    """Where each pixel of the current frame was in an older frame, as remap maps,
    at MV_BLOCK x MV_BLOCK resolution (one vector per block, bilinearly spread),
    standing in for hardware block motion estimation."""
    flow = cv2.calcOpticalFlowFarneback(g_cur, g_old, None, **FLOW_PARAMS)
    small = cv2.resize(flow, (GW // MV_BLOCK, GH // MV_BLOCK), interpolation=cv2.INTER_AREA)
    flow = cv2.resize(small, (GW, GH), interpolation=cv2.INTER_LINEAR)
    return GRID_X + flow[..., 0], GRID_Y + flow[..., 1]


def motion_trust(grey_cur, grey_ref, mx, my):
    """0..1 per pixel: how well the reference picture, moved by the vectors, matches
    the current picture. The whole map is 0 when most of the frame fails."""
    moved = cv2.remap(grey_ref, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    r = blur(np.abs(grey_cur.astype(np.float32) - moved.astype(np.float32)) / 255.0, 4)
    w = np.clip((VERIFY_HI - r) / (VERIFY_HI - VERIFY_LO), 0.0, 1.0).astype(np.float32)
    return w if float(w.mean()) >= VERIFY_CUT else np.zeros_like(w)


def live_pairs(times, lat_ms):
    """(display frame, newest DA-V2 frame) for every frame that has a DA-V2 result,
    exactly as simulate() walks the schedule."""
    events = anchor_schedule(times, lat_ms)
    pairs, ev, live_k = [], 0, None
    for i, t in enumerate(times):
        while ev < len(events) and events[ev][0] <= float(t) + 1e-9:
            live_k = events[ev][1]
            ev += 1
        if live_k is not None:
            pairs.append((i, live_k))
    return pairs


def export_mv(clip, clip_dir, lat_ms):
    """Luma of every frame + the (current, reference) pairs the estimator must match.
    lat_ms None: consecutive frames (i, i-1), into me_prev."""
    log(f'export_mv: enter lat={lat_ms}')
    out = os.path.join(clip_dir, 'me_prev' if lat_ms is None else f'me_{int(lat_ms)}')
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'frames.y'), 'wb') as f:
        for rgb in clip['rgb']:
            f.write(np.ascontiguousarray(cv2.cvtColor(np.asarray(rgb), cv2.COLOR_RGB2YUV)[..., 0]).tobytes())
    if lat_ms is None:
        pairs = [(i, i - 1) for i in range(1, len(clip['rgb']))]
    else:
        pairs = [(i, k) for i, k in live_pairs(clip['times'], lat_ms) if i != k]
    with open(os.path.join(out, 'pairs.txt'), 'w') as f:
        f.writelines(f'{i} {k}\n' for i, k in pairs)
    log(f'export_mv: exit {len(clip["rgb"])} frames, {len(pairs)} pairs -> {out}')
    return out


def load_hw_mv(clip_dir, lat_ms):
    """{(current, reference): (mx, my) remap} from me_probe's vectors.bin, or None.
    lat_ms None: the consecutive-frame vectors in me_prev."""
    d = os.path.join(clip_dir, 'me_prev' if lat_ms is None else f'me_{int(lat_ms)}')
    vb, pt = os.path.join(d, 'vectors.bin'), os.path.join(d, 'pairs.txt')
    if not (os.path.isfile(vb) and os.path.isfile(pt)):
        return None
    with open(pt) as f:
        pairs = [tuple(int(x) for x in line.split()) for line in f if line.strip()]
    bw, bh = (GW + MV_BLOCK - 1) // MV_BLOCK, (GH + MV_BLOCK - 1) // MV_BLOCK
    raw = np.fromfile(vb, np.int16)
    if raw.size != len(pairs) * bh * bw * 2:
        log(f'load_hw_mv: {vb} has {raw.size} values, expected {len(pairs) * bh * bw * 2}; ignoring')
        return None
    vec = raw.reshape(len(pairs), bh, bw, 2).astype(np.float32) / 4.0       # quarter pel -> px
    # Each vector belongs to its block's centre; spread bilinearly to every pixel.
    bu = ((GRID_X + 0.5) / MV_BLOCK - 0.5).astype(np.float32)
    bv = ((GRID_Y + 0.5) / MV_BLOCK - 0.5).astype(np.float32)
    out = {}
    for n, key in enumerate(pairs):
        flow = cv2.remap(vec[n], bu, bv, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        out[key] = (GRID_X + flow[..., 0], GRID_Y + flow[..., 1])
    log(f'load_hw_mv: {len(out)} pairs from {vb}')
    return out


class FlowPair:
    """Backward flow (frame i -> i-1) as a remap, with a forward/backward consistency
    mask, so the previous output can be compared with the current one."""

    def __init__(self, g_prev, g_cur):
        fb = cv2.calcOpticalFlowFarneback(g_cur, g_prev, None, **FLOW_PARAMS)
        ff = cv2.calcOpticalFlowFarneback(g_prev, g_cur, None, **FLOW_PARAMS)
        self.mx, self.my = GRID_X + fb[..., 0], GRID_Y + fb[..., 1]
        back = cv2.remap(ff, self.mx, self.my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        err = np.linalg.norm(fb + back, axis=2)
        m = 8
        inside = (self.mx > m) & (self.mx < GW - m) & (self.my > m) & (self.my < GH - m)
        self.mask = (err < 1.0) & inside
        self.motion = float(np.linalg.norm(fb, axis=2)[self.mask].mean()) if self.mask.any() else 0.0

    def change(self, prev_out, cur_out):
        warped = cv2.remap(prev_out, self.mx, self.my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        lo, hi = np.percentile(cur_out, (5, 95))
        return float(np.abs(cur_out - warped)[self.mask].mean()) / max(float(hi - lo), 1e-3)


# --------------------------------------------------------------- simulation
def stabilise(cur, prev_out, key, prev_mv, grey, grey_prev):
    """cur blended with prev_out moved to this frame, where the motion is verified."""
    if prev_out is None or prev_mv is None or key not in prev_mv:
        return cur
    mx, my = prev_mv[key]
    moved = cv2.remap(prev_out, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    w = STAB_ALPHA * motion_trust(grey, grey_prev, mx, my)
    return (cur + w * (moved - cur)).astype(np.float32)


def simulate(clip, lat_ms, sigmas, frame_sink=None, hw_mv=None, prev_mv=None):
    """Runs every method over the clip; returns {method: {score: value}}.
    frame_sink(i, outputs, conf) is called per frame when visuals are wanted.
    hw_mv: load_hw_mv() output, adds the *_hw methods.
    prev_mv: load_hw_mv(clip_dir, None) output, adds the *_stab methods."""
    log(f'simulate: enter lat={lat_ms} ms sigmas={sigmas} hw_mv={hw_mv is not None}')
    if clip is None:
        raise ValueError('no clip')
    rgb, zraw, draw, times = clip['rgb'], clip['zip'], clip['dav2'], clip['times']
    n = len(rgb)
    duration = max(float(times[-1]), 1e-6)
    events = anchor_schedule(times, lat_ms)
    log(f'simulate: {len(events)} DA-V2 results over {n} frames ({len(events) / duration:.1f} per s)')

    zip_sm, ideal_sm, live_sm = RangeSmoother(TAU_RANGE), RangeSmoother(TAU_RANGE), RangeSmoother(TAU_RANGE)
    zip_near = [None] * n
    anchors = {s: AnchorState(s) for s in [None] + list(sigmas)}
    curves = {None: AnchorState(None, use_curve=True)}
    live_near, g_fit, ev = None, (1.0, 0.0), 0
    ages, scores, prev_out, prev_grey = [], {}, None, None
    trust_sum, trust_n = 0.0, 0
    stab_prev = {}

    for i in range(n):
        t = float(times[i])
        lo, hi = zip_sm.update(zraw[i], t)
        zip_near[i] = to_near(zraw[i], lo, hi)
        lo, hi = ideal_sm.update(draw[i], t)
        ideal = to_near(draw[i], lo, hi)

        while ev < len(events) and events[ev][0] <= t + 1e-9:
            arrival, k = events[ev]
            lo, hi = live_sm.update(draw[k], arrival)
            live_near = to_near(draw[k], lo, hi)
            g_fit = global_fit(zip_near[k], live_near)
            for st in list(anchors.values()) + list(curves.values()):
                st.update(zip_near[k], live_near, arrival)
            live_k = k
            ev += 1
        if live_near is None:
            continue                                   # no DA-V2 result yet
        ages.append((t - float(times[live_k])) * 1000.0)

        z = zip_near[i]
        out = {'zip': z, 'dav2_ideal': ideal, 'dav2_live': live_near,
               'avg': np.clip(0.5 * (g_fit[0] * z + g_fit[1] + live_near), 0, 1).astype(np.float32)}
        out['anchor_g'] = anchors[None].apply(z)
        for s in sigmas:
            f = anchors[s].apply(z)
            out[f'anchor_s{s}'] = f
            out[f'anchor_s{s}_conf'] = flatten_uncertain(f, anchors[s].conf)
        out['curve'] = curves[None].apply(z)

        grey = cv2.cvtColor(rgb[i], cv2.COLOR_RGB2GRAY)
        if live_k == i:
            mc = live_near
        else:
            mx, my = block_motion(grey, cv2.cvtColor(rgb[live_k], cv2.COLOR_RGB2GRAY))
            mc = cv2.remap(live_near, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        out['dav2_mc'] = mc
        for s in sigmas:
            a, b = local_fit(z, mc, s)
            f = np.clip(a * z + b, 0.0, 1.0).astype(np.float32)
            out[f'mc_s{s}'] = f
            r = blur(np.abs(mc - f), 4)
            lo, hi = np.percentile(r, CONF_PCT)
            out[f'mc_s{s}_conf'] = flatten_uncertain(f, np.clip((r - lo) / max(hi - lo, 1e-4), 0.0, 1.0))
        if hw_mv is not None:
            if live_k == i:
                mc_hw = live_near
            else:
                if (i, live_k) not in hw_mv:
                    raise KeyError(f'no hardware vectors for pair {i} -> {live_k}; re-export with --export-mv={int(lat_ms)}')
                mx, my = hw_mv[(i, live_k)]
                mc_hw = cv2.remap(live_near, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            out['dav2_mc_hw'] = mc_hw
            for s in sigmas:
                a, b = local_fit(z, mc_hw, s)
                out[f'mc_s{s}_hw'] = np.clip(a * z + b, 0.0, 1.0).astype(np.float32)
            if live_k == i:
                trust = np.ones_like(z)
            else:
                trust = motion_trust(grey, cv2.cvtColor(rgb[live_k], cv2.COLOR_RGB2GRAY), mx, my)
            trust_sum += float(trust.mean())
            trust_n += 1
            target = trust * mc_hw + (1.0 - trust) * np.clip(g_fit[0] * z + g_fit[1], 0.0, 1.0)
            out['trust'] = trust
            for s in sigmas:
                a, b = local_fit(z, target, s)
                out[f'mc_s{s}_hwv'] = np.clip(a * z + b, 0.0, 1.0).astype(np.float32)
        if prev_mv is not None:
            grey_prev = cv2.cvtColor(rgb[i - 1], cv2.COLOR_RGB2GRAY) if i > 0 else None
            bases = {'zip_stab': z}
            if hw_mv is not None:
                bases[f'mc_s{sigmas[0]}_hwv_stab'] = out[f'mc_s{sigmas[0]}_hwv']
            for name, cur in bases.items():
                out[name] = stabilise(cur, stab_prev.get(name), (i, i - 1), prev_mv, grey, grey_prev)
                stab_prev[name] = out[name]

        if t >= WARMUP_S:
            edges = cv2.dilate(cv2.Canny(grey, 50, 150), np.ones((5, 5), np.uint8)) > 0
            flow = FlowPair(prev_grey, grey) if prev_out is not None else None
            near_mask = ideal > NEAR_THRESHOLD
            for name, o in out.items():
                if name == 'trust':
                    continue
                acc = scores.setdefault(name, {'struct': [], 'struct_nr': [], 'flicker': [], 'edge': []})
                acc['struct'].append(ssi_mae(o, ideal))
                acc['struct_nr'].append(ssi_mae(o, ideal, near_mask))
                acc['edge'].append(edge_precision(o, edges))
                if flow is not None:
                    acc['flicker'].append(flow.change(prev_out[name], o))
        if frame_sink is not None:
            frame_sink(i, out, anchors[sigmas[0]].conf if sigmas else anchors[None].conf)
        prev_out, prev_grey = out, grey

    result = {name: {k: float(np.nanmean(v)) if v else float('nan') for k, v in acc.items()} for name, acc in scores.items()}
    result['_timing'] = {'lat_ms': lat_ms, 'anchors_per_s': len(events) / duration,
                         'anchor_age_ms_mean': float(np.mean(ages)), 'anchor_age_ms_max': float(np.max(ages)),
                         'motion_trust_mean': trust_sum / trust_n if trust_n else float('nan')}
    log(f'simulate: exit lat={lat_ms} ms, mean anchor age {result["_timing"]["anchor_age_ms_mean"]:.0f} ms')
    return result


# ------------------------------------------------------------------ visuals
def colour(near):
    return cv2.cvtColor(cv2.applyColorMap((near * 255).astype(np.uint8), cv2.COLORMAP_INFERNO), cv2.COLOR_BGR2RGB)


def label(img, text):
    img = img.copy()
    cv2.rectangle(img, (0, 0), (len(text) * 11 + 12, 26), (0, 0, 0), -1)
    cv2.putText(img, text, (6, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def panels(rgb, out, sigma):
    """The six views shown in sheet.png and compare.mp4, labelled. The motion-
    compensated views use the hardware vectors when they were loaded."""
    if 'trust' in out:
        fused, fifth = out[f'mc_s{sigma}_hwv'], label(cv2.cvtColor((out['trust'] * 255).astype(np.uint8), cv2.COLOR_GRAY2RGB),
                                                      'Motion trust (white = verified)')
    else:
        fused, fifth = out[f'mc_s{sigma}'], label(colour(out['dav2_mc']), 'DA-V2 moved by motion vectors')
    return [label(rgb, 'Game frame'),
            label(colour(out['zip']), 'ZipDepth alone (today)'),
            label(colour(out['dav2_live']), 'DA-V2 alone, live (lagged)'),
            label(colour(fused), 'Fused: ZipDepth + moved DA-V2'),
            fifth,
            label(colour(out['dav2_ideal']), 'DA-V2 every frame (target)')]


def render(clip, clip_dir, lat_ms, sigma, video):
    """sheet.png with three moments of the clip; compare.mp4 with the whole clip."""
    log(f'render: enter lat={lat_ms} sigma={sigma} video={video}')
    n = len(clip['rgb'])
    picks = {int(n * f) for f in (0.3, 0.55, 0.8)}
    rows = []
    enc = None
    if video:
        # MPEG-4 part 2: the only mp4 encoder in the opencv-python wheel without an
        # extra OpenH264 DLL. Played at the clip's mean rate.
        enc = cv2.VideoWriter(os.path.join(clip_dir, 'compare.mp4'), cv2.VideoWriter_fourcc(*'mp4v'),
                              clip['fps'], (GW * 3, GH * 2))
        if not enc.isOpened():
            log('render: video writer unavailable; writing the sheet only')
            enc = None

    def sink(i, out, conf):
        p = panels(clip['rgb'][i], out, sigma)
        if enc is not None:
            enc.write(cv2.cvtColor(np.vstack([np.hstack(p[:3]), np.hstack(p[3:])]), cv2.COLOR_RGB2BGR))
        if i in picks:
            rows.append(np.hstack(p))

    simulate(clip, lat_ms, [sigma], sink, hw_mv=load_hw_mv(clip_dir, lat_ms))
    if enc is not None:
        enc.release()
    if rows:
        sheet = np.vstack(rows)
        sheet = cv2.resize(sheet, (sheet.shape[1] // 2, sheet.shape[0] // 2), interpolation=cv2.INTER_AREA)
        cv2.imwrite(os.path.join(clip_dir, 'sheet.png'), cv2.cvtColor(sheet, cv2.COLOR_RGB2BGR))
    log('render: exit')


def load_clip(clip_dir):
    if not clip_dir or not os.path.isdir(clip_dir):
        raise FileNotFoundError(f'clip dir not found: {clip_dir}')
    with open(os.path.join(clip_dir, 'meta.json')) as f:
        meta = json.load(f)
    return {'rgb': np.load(os.path.join(clip_dir, 'rgb.npy'), mmap_mode='r'),
            'zip': np.load(os.path.join(clip_dir, 'zip.npy'), mmap_mode='r'),
            'dav2': np.load(os.path.join(clip_dir, 'dav2.npy'), mmap_mode='r'),
            'times': np.load(os.path.join(clip_dir, 'times.npy')),
            'fps': float(meta['fps']), 'meta': meta}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    clip_dir = argv[1]
    opts = dict(a[2:].split('=', 1) for a in argv[2:] if a.startswith('--') and '=' in a)
    lats = [float(x) for x in opts.get('lat', '33,67,133').split(',')]
    sigmas = [int(x) for x in opts.get('sigma', '24,48,96').split(',')]
    show_lat = float(opts.get('show-lat', 67))
    show_sigma = int(opts.get('show-sigma', 48))
    log(f'main: enter clip={clip_dir} lats={lats} sigmas={sigmas}')

    clip = load_clip(clip_dir)
    if 'export-mv' in opts or 'export-prev' in opts:
        export_mv(clip, clip_dir, float(opts['export-mv']) if 'export-mv' in opts else None)
        log('main: exit')
        return 0
    if '--render-only' not in argv:
        results = {'meta': clip['meta'],
                   'runs': {str(int(l)): simulate(clip, l, sigmas, hw_mv=load_hw_mv(clip_dir, l),
                                                  prev_mv=load_hw_mv(clip_dir, None)) for l in lats}}
        with open(os.path.join(clip_dir, 'results.json'), 'w') as f:
            json.dump(results, f, indent=2)
    render(clip, clip_dir, show_lat, show_sigma, '--no-video' not in argv)
    log('main: exit')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
