"""Check the score's shape: does it build, do the hits land, is it clean?"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_score as S  # noqa: E402

audio = S.build()
mono = audio.mean(axis=1)
rate = S.RATE

print("=== level envelope (RMS per 0.5 s) ===")
win = rate // 2
bars = []
for i in range(0, len(mono) - win, win):
    seg = mono[i:i + win]
    rms = float(np.sqrt(np.mean(seg ** 2)))
    db = 20 * np.log10(max(rms, 1e-9))
    bars.append((i / rate, db))
lo = min(b[1] for b in bars)
hi = max(b[1] for b in bars)
for t, db in bars:
    n = int((db - lo) / max(hi - lo, 1e-6) * 52)
    mark = ""
    for name, beat in (("DEPTH", S.BEAT_DEPTH), ("ROOM", S.BEAT_ROOM),
                       ("EASY", S.BEAT_EASY), ("END", S.BEAT_END)):
        if abs(t - beat) < 0.25:
            mark = f"  <-- {name} beat"
    print(f"  {t:5.1f}s {db:6.1f} dB |{'#' * n:<52}|{mark}")

print("\n=== spectral centroid per 2 s (where the energy sits) ===")
n_fft = 4096
for i in range(0, len(mono) - rate * 2, rate * 2):
    seg = mono[i:i + rate * 2]
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1 / rate)
    centroid = float(np.sum(freqs * spec) / max(np.sum(spec), 1e-9))
    print(f"  {i/rate:5.1f}s  centroid {centroid:7.1f} Hz")

print("\n=== cleanliness ===")
print(f"  peak {np.abs(audio).max():.4f}   clipped {int(np.sum(np.abs(audio) > 1.0))}")
d = np.abs(np.diff(audio, axis=0)).max(axis=1)
p999 = float(np.percentile(d, 99.9))
print(f"  largest sample-to-sample step {d.max():.4f}  "
      f"(99.9th percentile {p999:.4f}, ratio {d.max()/max(p999,1e-9):.2f})")
print("  a click would be an isolated spike far above the 99.9th percentile;")
print("  a ratio near 1 means the big steps are just wideband content.")
print(f"  DC offset  L {audio[:,0].mean():+.5f}  R {audio[:,1].mean():+.5f}")
corr = float(np.corrcoef(audio[:, 0], audio[:, 1])[0, 1])
print(f"  stereo correlation {corr:.3f}  (1.0 would be mono)")
for k, v in S.loudness_report(audio).items():
    print(f"  {k:18s} {v}")
