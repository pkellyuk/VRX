"""Smoke-test the drawing helpers and report what they produce."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_brand as B  # noqa: E402
from film import vrx_draw as D  # noqa: E402

W, H = 1920, 1080
frame = B.gradient(W, H)
print("gradient", frame.shape, frame.dtype, float(frame.min()), float(frame.max()))

big = D.text_rgba(["Your screen becomes a window."], 64, weight="bold", colour=B.TEXT)
print("headline overlay", big.shape, "opaque px", int((big[:, :, 3] > 0.5).sum()))

small = D.text_rgba(["VRX estimates depth", "for every frame."], 46,
                    colour=B.MUTED, leading=1.16)
print("two-line overlay", small.shape)

cy = D.text_rgba(["HELLDIVERS 2"], 26, weight="mono", colour=B.CYAN, tracking=0.12)
print("tracked label", cy.shape, "tracking applied:", cy.shape[1] > 300)

D.put(frame, big, 96, 900)
D.put(frame, small, 96, 120)
D.put(frame, cy, 96, 60)
D.rect(frame, 1200, 200, 420, 260, B.PANEL, radius=18, alpha=0.9)
D.rect(frame, 1200, 200, 420, 260, B.CYAN, radius=18, alpha=0.0)
D.hairline(frame, 96, 108, 700, 108, B.CYAN, 0.8, 2)

m = D.rounded_mask(200, 120, 16)
print("rounded mask", m.shape, float(m.mean()))
print("vignette", B.vignette(W, H).shape, float(B.vignette(W, H).min()))

# Alpha blend must never leave the frame out of range.
print("frame range after compositing",
      round(float(frame.min()), 4), round(float(frame.max()), 4))

out = B.BUILD / "draw_test.png"
out.parent.mkdir(parents=True, exist_ok=True)
import cv2  # noqa: E402
cv2.imwrite(str(out), cv2.cvtColor((np.clip(frame, 0, 1) * 255).astype(np.uint8),
                                   cv2.COLOR_RGB2BGR))
print("wrote", out)
