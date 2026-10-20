"""Per-stem level profile, to find which part changes across the film."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_score as S  # noqa: E402

_, stems = S.build(with_stems=True)
rate = S.RATE
win = rate // 2

names = ["pad", "bass", "pluck", "pulse", "hits"]
print("RMS per 0.5 s window, in dB, per stem")
print(f"{'t':>6s} " + " ".join(f"{n:>7s}" for n in names))
print(f"{'':>6s} " + " ".join(f"{S.BEAT_DEPTH:>7.1f}" for _ in names))
for i in range(0, len(stems["pad"]) - win, win):
    t = i / rate
    if t * 2 % 2:          # one row per 1.0 s is enough
        continue
    row = []
    for n in names:
        seg = stems[n][i:i + win]
        rms = float(np.sqrt(np.mean(seg ** 2)))
        row.append(20 * np.log10(max(rms, 1e-9)))
    print(f"{t:6.1f} " + " ".join(f"{v:7.1f}" for v in row))

print("\nARC value at the section boundaries:")
arc = S.curve(S.ARC)
for name, beat in (("flat", 0.0), ("depth", S.BEAT_DEPTH), ("room", S.BEAT_ROOM),
                   ("easy", S.BEAT_EASY), ("end", S.BEAT_END)):
    i = min(int(beat * rate), len(arc) - 1)
    print(f"  {name:6s} {beat:5.1f}s  {arc[i]:.3f}")
