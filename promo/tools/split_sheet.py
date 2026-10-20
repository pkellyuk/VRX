"""Split a contact sheet into viewable panels and write a page for each."""
from __future__ import annotations

import sys
from pathlib import Path

import cv2

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from film import vrx_brand as B  # noqa: E402

SHEETS = B.BUILD / "sheets"
VIEW = B.BUILD / "view"
VIEW.mkdir(parents=True, exist_ok=True)

which = sys.argv[1] if len(sys.argv) > 1 else "wide_sheet"
parts = int(sys.argv[2]) if len(sys.argv) > 2 else 3

img = cv2.imread(str(SHEETS / f"{which}.png"))
h, w = img.shape[:2]
names = []
step = h // parts
for i in range(parts):
    y0 = i * step
    y1 = h if i == parts - 1 else (i + 1) * step
    chunk = img[y0:y1]
    name = f"{which}_part{i+1}.png"
    cv2.imwrite(str(VIEW / name), chunk)
    names.append((name, chunk.shape))

cards = "\n".join(
    f'<figure><img src="{n}"><figcaption>{n} &middot; {s[0]}x{s[1]}</figcaption></figure>'
    for n, s in names)
page = f"""<!doctype html><meta charset="utf-8"><title>{which}</title>
<style>
 body {{ margin:0; background:#05070F; color:#E6E9F5;
        font:12px/1.3 "Segoe UI",system-ui,sans-serif; }}
 img {{ width:640px; display:block; border:1px solid #26314f; }}
 figure {{ margin:0 0 4px; }} figcaption {{ color:#AAB2CC; padding:1px 0; }}
</style>
{cards}
"""
(VIEW / f"{which}.html").write_text(page, encoding="utf-8")
print("wrote", VIEW / f"{which}.html", "parts:", [n for n, _ in names])
