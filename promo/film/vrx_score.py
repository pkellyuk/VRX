"""The promo's score, synthesised to picture.

A warm cinematic-ambient bed rather than a song. The structure follows the film:

    0.0 s   a low bed, unresolved - the flat picture
    3.4 s   a plucked figure enters and a riser starts moving
    7.2 s   the hit: the picture gains depth, the pulse and bass arrive, full bed
   17.6 s   the room: the chord opens to C, warmer and wider, pulse thins out
   25.5 s   easy mode: simpler and more intimate, pluck drops away
   32.1 s   the end card: resolve and hold, then let it go

Two things this deliberately avoids, both of which it got wrong first time:
the pad is *continuous* across the whole film with chords crossfaded between
sections, so a section change never leaves a hole in the music; and the level
arc is set explicitly, so no section is quieter than the one before it by
accident.

Everything is generated - oscillators, envelopes, filters and a convolution
reverb built from decaying noise. No samples, so the score carries no third-party
rights and rebuilds identically.
"""
from __future__ import annotations

import math

import numpy as np
from scipy import signal

RATE = 48000
DURATION = 40.0

# The film's beats, from vrx_shotlist.py.
BEAT_FLAT = 0.0
BEAT_DEPTH = 7.2
BEAT_ROOM = 17.6
BEAT_EASY = 25.5
BEAT_END = 32.1

# One chord per section, crossfaded into the next. A minor -> F -> C -> G -> C.
BED = [
    (BEAT_FLAT, (110.00, 164.81, 220.00, 261.63)),   # Am
    (BEAT_DEPTH, (87.31, 130.81, 174.61, 220.00)),   # F
    (BEAT_ROOM, (65.41, 98.00, 130.81, 164.81)),     # C
    (BEAT_EASY, (98.00, 123.47, 146.83, 196.00)),    # G
    (BEAT_END, (65.41, 98.00, 130.81, 196.00)),      # C, wide
]
ROOTS = [55.00, 43.65, 65.41, 49.00, 65.41]          # one per bed entry

# The dynamic arc, as a gain curve read by every sustained part. Control points
# are interpolated smoothly. It spans about 4 dB from quietest to peak: a promo
# that is watched on a phone cannot afford a wide dynamic range, so the shape is
# carried by density and brightness rather than by level.
ARC = [
    (0.0, 0.66),
    (2.8, 0.72),
    (BEAT_DEPTH - 0.6, 0.80),
    (BEAT_DEPTH, 1.00),
    (BEAT_DEPTH + 2.0, 0.92),
    (BEAT_ROOM, 0.84),
    (BEAT_ROOM + 2.5, 0.80),
    (BEAT_EASY, 0.86),
    (BEAT_EASY + 1.5, 0.84),
    (BEAT_END, 0.88),
    (BEAT_END + 2.5, 0.80),
    (DURATION - 2.5, 0.72),
]

_rng = np.random.default_rng(20240925)


def n_samples(seconds: float) -> int:
    return int(round(seconds * RATE))


def curve(points, total: float = DURATION) -> np.ndarray:
    """A smooth envelope through (time, value) control points, one per sample."""
    t = np.arange(n_samples(total), dtype=np.float32) / RATE
    xs = np.array([p[0] for p in points], np.float32)
    ys = np.array([p[1] for p in points], np.float32)
    return np.interp(t, xs, ys).astype(np.float32)


def adsr(n: int, attack: float, decay: float, sustain: float,
         release: float) -> np.ndarray:
    a, d, r = min(n_samples(attack), n), min(n_samples(decay), n), min(n_samples(release), n)
    env = np.full(n, sustain, np.float32)
    if a:
        env[:a] = np.linspace(0, 1, a) ** 1.4
    if d:
        env[a:a + d] = np.linspace(env[a - 1] if a else 1.0, sustain, d)
    if r:
        env[n - r:] = np.linspace(env[n - r - 1], 0.0, r)
    return env


def lowpass(x: np.ndarray, cutoff: float, order: int = 2) -> np.ndarray:
    wn = min(max(cutoff / (RATE / 2), 1e-5), 0.999)
    b, a = signal.butter(order, wn, btype="low")
    return signal.filtfilt(b, a, x).astype(np.float32)


def highpass(x: np.ndarray, cutoff: float, order: int = 2) -> np.ndarray:
    wn = min(max(cutoff / (RATE / 2), 1e-5), 0.999)
    b, a = signal.butter(order, wn, btype="high")
    return signal.filtfilt(b, a, x).astype(np.float32)


def sine(freq: float, n: int, phase: float = 0.0) -> np.ndarray:
    t = np.arange(n, dtype=np.float64) / RATE + phase
    return np.sin(2 * np.pi * freq * t).astype(np.float32)


def saw(freq: float, n: int, phase: float = 0.0) -> np.ndarray:
    t = np.arange(n, dtype=np.float64) / RATE + phase
    return (2.0 * ((freq * t) % 1.0) - 1.0).astype(np.float32)


def pad_note_stack(notes: tuple[float, ...], n: int, detune: float = 0.0035) -> np.ndarray:
    """Detuned saws, one stack per note, summed and averaged."""
    out = np.zeros(n, np.float32)
    for i, f in enumerate(notes):
        for k, d in enumerate((-detune, 0.0, detune)):
            out += saw(f * (1 + d), n, phase=(i * 0.31 + k * 0.17) % 1.0)
    out /= max(len(notes) * 3, 1)
    return out


def pad_track() -> np.ndarray:
    """The continuous bed: chords crossfaded between sections, never a hole.

    The fade zones of neighbouring chords must occupy the *same* stretch of time,
    or both chords play at full level across the join and the bed bumps up by
    several dB at every section change. So each segment runs from `fade` before
    its section to the start of the next one, and rises over its first `fade` and
    falls over its last `fade`.
    """
    n = n_samples(DURATION)
    out = np.zeros(n, np.float32)
    fade = 2.4
    for i, (start, notes) in enumerate(BED):
        nxt = BED[i + 1][0] if i + 1 < len(BED) else DURATION
        seg_start = max(0.0, start - (fade if i > 0 else 0.0))
        seg_end = nxt
        length = n_samples(seg_end - seg_start)
        if length <= 0:
            continue
        seg = pad_note_stack(notes, length)
        seg = lowpass(seg, 620.0 + 70.0 * i, 2)
        # Even the chords out: a voicing change should be heard as a change of
        # chord, not as a change of volume.
        seg /= max(float(np.sqrt(np.mean(seg ** 2))), 1e-9)

        env = np.ones(length, np.float32)
        if i > 0:
            f = min(n_samples(fade), length)
            env[:f] = 0.5 - 0.5 * np.cos(np.linspace(0, math.pi, f))
        if i < len(BED) - 1:
            f = min(n_samples(fade), length)
            env[-f:] *= 0.5 + 0.5 * np.cos(np.linspace(0, math.pi, f))

        i0 = n_samples(seg_start)
        out[i0:i0 + length] += seg * env
    # A slow tremolo, so the bed breathes rather than sits still.
    lfo = 1.0 + 0.07 * np.sin(2 * np.pi * 0.065 * np.arange(n) / RATE)
    out *= lfo.astype(np.float32)
    return (out * curve(ARC) * 0.60).astype(np.float32)


def bass_track() -> np.ndarray:
    """A note per beat from the depth reveal, so the low end pumps, never gaps."""
    n = n_samples(DURATION)
    out = np.zeros(n, np.float32)

    # Before the reveal there is still a floor: one long low root, held under the
    # opening so the film never starts on nothing.
    intro = sine(ROOTS[0] / 2.0, n_samples(BEAT_DEPTH + 1.5))
    intro = lowpass(intro, 180.0, 2)[:n_samples(BEAT_DEPTH + 0.4)]
    intro *= adsr(len(intro), 1.6, 0.4, 0.86, 0.9)
    out[:len(intro)] += intro * 0.55

    beat = 0.75
    t = BEAT_DEPTH
    while t < DURATION - 0.5:
        section = 0
        for k, (start, _) in enumerate(BED):
            if t >= start:
                section = k
        root = ROOTS[section]
        note = sine(root, n_samples(beat + 0.35))
        note = lowpass(note, 240.0, 2)
        note /= max(float(np.sqrt(np.mean(note ** 2))), 1e-9)
        env = adsr(len(note), 0.012, 0.22, 0.72, 0.30)
        seg = (note * env)[: n_samples(beat + 0.35)]
        i0 = n_samples(t)
        take = min(len(seg), n - i0)
        out[i0:i0 + take] += seg[:take]
        t += beat
    # Sits well under the pad: this is the floor of the mix, not the tune.
    return (out * curve(ARC) * 0.20).astype(np.float32)


def pluck(freq: float, seconds: float) -> np.ndarray:
    n = n_samples(seconds)
    t = np.arange(n, dtype=np.float64) / RATE
    body = (np.sin(2 * np.pi * freq * t)
            + 0.26 * np.sin(2 * np.pi * 2 * freq * t)
            + 0.10 * np.sin(2 * np.pi * 3 * freq * t))
    return (body.astype(np.float32) * np.exp(-t * 5.0).astype(np.float32))


def pluck_track() -> np.ndarray:
    """A quiet figure over the window section only, stepping with the chords."""
    n = n_samples(DURATION)
    out = np.zeros(n, np.float32)
    figure = (0, 4, 7, 4, 0, 7, 4, 9)
    step = 0.375
    t = BEAT_DEPTH + 0.75
    k = 0
    while t < BEAT_ROOM - 0.25:
        root = ROOTS[1]
        note = root * 4 * (2 ** (figure[k % len(figure)] / 12.0))
        seg = pluck(note, 0.85) * (0.10 if k % 2 else 0.14)
        i0 = n_samples(t)
        take = min(len(seg), n - i0)
        out[i0:i0 + take] += seg[:take]
        t += step
        k += 1
    return out


def tick(seconds: float, centre: float, level: float) -> np.ndarray:
    n = n_samples(seconds)
    noise = _rng.normal(0, 1, n).astype(np.float32)
    env = np.exp(-np.arange(n) / RATE * 55).astype(np.float32)
    band = highpass(lowpass(noise * env, centre * 1.4, 2), centre * 0.6, 2)
    return band * level


def thump(seconds: float, level: float) -> np.ndarray:
    """A soft kick: a sine dropping from 96 Hz to 40 Hz."""
    n = n_samples(seconds)
    t = np.arange(n, dtype=np.float64) / RATE
    freq = 40.0 + 56.0 * np.exp(-t * 24)
    phase = np.cumsum(2 * np.pi * freq / RATE)
    return (np.sin(phase).astype(np.float32) * np.exp(-t * 4.2).astype(np.float32) * level)


def pulse_track() -> np.ndarray:
    """Ticks on the beat once depth arrives, with a kick every fourth beat."""
    n = n_samples(DURATION)
    out = np.zeros(n, np.float32)
    beat = 0.75
    t = BEAT_DEPTH
    beat_no = 0
    while t < DURATION - 0.6:
        # The pulse thins after the room reveal but never drops out: it is what
        # carries the sense of energy, so losing it costs more level than the
        # gain curve alone accounts for.
        section = 2 if t >= BEAT_ROOM else 1
        gain = 1.0 if t < BEAT_ROOM else (0.92 if t < BEAT_EASY else 0.86)
        seg = tick(0.14, 2400.0, 0.085 * gain)
        i0 = n_samples(t)
        take = min(len(seg), n - i0)
        out[i0:i0 + take] += seg[:take]
        if beat_no % 2 == 1:
            seg = tick(0.10, 4200.0, 0.045 * gain)
            out[i0:i0 + min(len(seg), n - i0)] += seg[:min(len(seg), n - i0)]
        if beat_no % 4 == 0 and t >= BEAT_DEPTH:
            seg = thump(0.85, 0.42 * gain)
            out[i0:i0 + min(len(seg), n - i0)] += seg[:min(len(seg), n - i0)]
        t += beat
        beat_no += 1
    return out


def impact(level: float = 1.0) -> np.ndarray:
    """The reveal hit: a short sub drop, a noise stab and a bright ping."""
    n = n_samples(1.9)
    t = np.arange(n, dtype=np.float64) / RATE
    freq = 160.0 * np.exp(-t * 6.5) + 36.0
    phase = np.cumsum(2 * np.pi * freq / RATE)
    drop = np.sin(phase).astype(np.float32) * np.exp(-t * 2.6).astype(np.float32)

    stab = highpass(_rng.normal(0, 1, n).astype(np.float32), 700.0, 2)
    stab *= np.exp(-t * 6.0).astype(np.float32)
    ping = (sine(1567.98, n) * 0.30 + sine(2349.32, n) * 0.16).astype(np.float32)
    ping *= np.exp(-t * 5.5).astype(np.float32)
    return (drop * 0.85 + stab * 0.34 + ping * 0.22) * level


def riser(seconds: float, level: float) -> np.ndarray:
    """Filtered noise opening upward, ending at full tilt - no fade-out."""
    n = n_samples(seconds)
    noise = _rng.normal(0, 1, n).astype(np.float32)
    steps = 40
    out = np.zeros(n, np.float32)
    edges = np.linspace(0, n, steps + 1).astype(int)
    for s in range(steps):
        a, b = edges[s], edges[s + 1]
        if b <= a:
            continue
        cut = 160.0 * (35.0 ** (s / max(steps - 1, 1)))
        out[a:b] = lowpass(noise[a:b], cut, 2)
    env = (np.linspace(0.0, 1.0, n) ** 1.7).astype(np.float32)
    return out * env * level


def reverb_ir(seconds: float = 2.4, decay: float = 3.6,
              pre_delay: float = 0.018) -> np.ndarray:
    """A stereo impulse response: noise decaying to silence, darker over time."""
    n = n_samples(seconds)
    t = np.arange(n, dtype=np.float64) / RATE
    env = np.exp(-t * decay).astype(np.float32)
    ir = _rng.normal(0, 1, (n, 2)).astype(np.float32) * env[:, None]
    ir = np.stack([lowpass(ir[:, c], 4600.0, 2) for c in range(2)], axis=1)
    pre = n_samples(pre_delay)
    ir = np.vstack([np.zeros((pre, 2), np.float32), ir])
    return ir / max(np.abs(ir).max(), 1e-9)


_IR: np.ndarray | None = None


def reverb(stereo: np.ndarray, mix: float = 0.28) -> np.ndarray:
    global _IR
    if _IR is None:
        _IR = reverb_ir()
    wet = np.stack([
        signal.fftconvolve(stereo[:, c], _IR[:, c])[:len(stereo)]
        for c in range(2)
    ], axis=1).astype(np.float32)
    peak = float(np.abs(wet).max())
    if peak > 0:
        wet *= float(np.abs(stereo).max()) / peak
    return (stereo * (1 - mix) + wet * mix).astype(np.float32)


def _add(buf: np.ndarray, clip: np.ndarray, at: float, gain: float = 1.0,
         pan: float = 0.0) -> None:
    """Mix a mono clip into the stereo buffer at a time, with equal-power pan."""
    i = n_samples(at)
    if i >= len(buf) or i < 0:
        return
    seg = clip[: len(buf) - i]
    left = math.cos((pan + 1) * math.pi / 4)
    right = math.sin((pan + 1) * math.pi / 4)
    buf[i:i + len(seg), 0] += seg * gain * left
    buf[i:i + len(seg), 1] += seg * gain * right


def build(with_stems: bool = False):
    """Render the score as stereo float32 at RATE."""
    n = n_samples(DURATION)
    pad = pad_track()
    bass = bass_track()
    plucks = pluck_track()
    pulse = pulse_track()

    buf = np.zeros((n, 2), np.float32)
    # The bed is widened by a few milliseconds between the channels.
    _add(buf, pad, 0.0, 1.0, -0.22)
    _add(buf, pad, 0.011, 0.9, 0.22)
    _add(buf, bass, 0.0, 1.0, 0.0)
    _add(buf, plucks, 0.0, 1.0, 0.0)
    _add(buf, pulse, 0.0, 1.0, 0.0)

    hits = np.zeros((n, 2), np.float32)
    _add(hits, riser(2.0, 0.20), BEAT_DEPTH - 2.0)
    _add(hits, riser(2.2, 0.13), BEAT_END - 2.2)
    for at, level in ((BEAT_DEPTH, 1.0), (BEAT_ROOM, 0.62), (BEAT_END, 0.74)):
        _add(hits, impact(level), at)
    buf += hits

    # Clear a little room either side of each hit, the way a mix engineer would.
    duck = np.ones(n, np.float32)
    for at in (BEAT_DEPTH, BEAT_ROOM, BEAT_END):
        i = n_samples(at)
        length = n_samples(0.7)
        shape = np.linspace(0.62, 1.0, length).astype(np.float32) ** 1.5
        seg = min(length, n - i)
        duck[i:i + seg] = np.minimum(duck[i:i + seg], shape[:seg])
    buf *= duck[:, None]

    buf = reverb(buf, mix=0.30)

    # Start and end in silence.
    top = n_samples(0.30)
    buf[:top] *= np.linspace(0, 1, top, dtype=np.float32)[:, None]
    tail = n_samples(1.2)
    buf[-tail:] *= np.linspace(1, 0, tail, dtype=np.float32)[:, None]

    peak = float(np.abs(buf).max())
    if peak > 0:
        buf *= 0.90 / peak

    if with_stems:
        return buf, {"pad": pad, "bass": bass, "pluck": plucks,
                     "pulse": pulse, "hits": hits, "duck": duck}
    return buf


def loudness_report(stereo: np.ndarray) -> dict:
    """Peak, RMS and a crude programme-loudness estimate for a level check."""
    peak = float(np.abs(stereo).max())
    rms = float(np.sqrt(np.mean(stereo ** 2)))
    band = np.stack([highpass(stereo[:, c], 60.0, 2) for c in range(2)], axis=1)
    weighted = np.stack([band[:, c] + 1.4 * highpass(band[:, c], 1800.0, 1)
                         for c in range(2)], axis=1)
    k = float(np.sqrt(np.mean(weighted ** 2)))
    return {
        "peak_dbfs": round(20 * math.log10(max(peak, 1e-9)), 2),
        "rms_dbfs": round(20 * math.log10(max(rms, 1e-9)), 2),
        "loudness_dbfs": round(20 * math.log10(max(k, 1e-9)), 2),
        "crest_db": round(20 * math.log10(max(peak, 1e-9) / max(rms, 1e-9)), 2),
        "clipped_samples": int(np.sum(np.abs(stereo) > 1.0)),
        "seconds": round(len(stereo) / RATE, 2),
    }


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from film import vrxmedia as M  # noqa: E402

    audio = build()
    out = M.ensure_build_dir() / "score_preview.wav"
    M.write_wav(out, audio, RATE)
    print("wrote", out, audio.shape)
    for k, v in loudness_report(audio).items():
        print(f"  {k:18s} {v}")
