"""Render a short VRX3D promo from imagery published on vrx3d.uk."""

from pathlib import Path
import math
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
W, H, FPS = 1280, 720, 30
SCENE_SECONDS = 4.2
FADE_FRAMES = 10
BG = (10, 16, 29)
CYAN = (88, 231, 232)
WHITE = (243, 248, 251)
MUTED = (163, 183, 198)
FONT = Path(r"C:\Windows\Fonts\bahnschrift.ttf")
FONT_BOLD = Path(r"C:\Windows\Fonts\segoeuib.ttf")


def font(size, bold=False):
    return ImageFont.truetype(str(FONT_BOLD if bold else FONT), size)


def base():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    for y in range(H):
        a = y / H
        d.line((0, y, W, y), fill=(int(10 + 5*a), int(16 + 9*a), int(29 + 15*a)))
    for x in range(0, W, 64):
        d.line((x, 0, x, H), fill=(23, 36, 50), width=1)
    for y in range(0, H, 64):
        d.line((0, y, W, y), fill=(23, 36, 50), width=1)
    d.rectangle((0, 0, W, 8), fill=CYAN)
    return im


def photo(im, name, box, border=CYAN):
    src = Image.open(ASSETS / name).convert("RGB")
    x, y, w, h = box
    scale = max(w / src.width, h / src.height)
    src = src.resize((math.ceil(src.width*scale), math.ceil(src.height*scale)), Image.Resampling.LANCZOS)
    ox = (src.width - w) // 2
    oy = (src.height - h) // 2
    src = src.crop((ox, oy, ox+w, oy+h))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w-1, h-1), radius=20, fill=255)
    shadow = Image.new("RGBA", (w+60, h+60), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((24, 24, w+35, h+35), radius=24, fill=(0, 0, 0, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(22))
    im.paste(shadow, (x-30, y-20), shadow)
    im.paste(src, (x, y), mask)
    ImageDraw.Draw(im).rounded_rectangle((x-2, y-2, x+w+1, y+h+1), radius=22, outline=border, width=3)


def label(d, section, number):
    d.text((70, 49), "VRX3D  /  " + section, font=font(20, True), fill=CYAN)
    d.text((1171, 52), f"{number:02d} / 08", font=font(18), fill=MUTED)


def footer(d):
    d.line((70, 660, 1210, 660), fill=(69, 91, 106), width=2)
    d.text((70, 672), "vrx3d.uk", font=font(18, True), fill=WHITE)
    d.text((988, 672), "FREE & OPEN SOURCE", font=font(16, True), fill=CYAN)


def text_block(d, lines, body=None, x=70, y=174, size=53):
    for i, line in enumerate(lines):
        d.text((x, y+i*(size+12)), line, font=font(size, True), fill=WHITE, stroke_width=0)
    if body:
        by = y + len(lines)*(size+12) + 32
        for i, line in enumerate(body):
            d.text((x, by+i*32), line, font=font(24), fill=MUTED)


def scene(i):
    im = base()
    d = ImageDraw.Draw(im)
    sections = ["THE SHIFT", "THE SCREEN", "EASY MODE", "THE TECH", "THE ROOM", "MORE TO WATCH", "HOW YOU PLAY", "GET VRX3D"]
    label(d, sections[i], i+1)
    if i == 0:
        photo(im, "helldivers.jpg", (570, 107, 632, 498))
        text_block(d, ["FLAT GAMES.", "REAL DEPTH."], ["Your PC games, reimagined", "on a huge screen in VR."], size=52)
    elif i == 1:
        photo(im, "cyberpunk.jpg", (570, 107, 632, 498))
        text_block(d, ["A CINEMA", "IN YOUR", "HEADSET."], ["Stereo 3D, made from", "the game you already play."], size=51)
    elif i == 2:
        text_block(d, ["START YOUR GAME.", "PUT ON YOUR HEADSET."], ["Easy mode notices the game in front", "and attaches automatically."], y=118, size=45)
        photo(im, "easy-mode.png", (222, 383, 836, 242))
    elif i == 3:
        text_block(d, ["ONE FRAME.", "TWO PERSPECTIVES."], ["VRX3D estimates depth on your GPU", "and builds a view for each eye."], y=105, size=45)
        photo(im, "cyberpunk.jpg", (89, 368, 535, 260))
        photo(im, "cyberpunk-depth.png", (656, 368, 535, 260), border=(197, 119, 255))
        d.text((91, 636), "GAME", font=font(15, True), fill=CYAN)
        d.text((658, 636), "ESTIMATED DEPTH", font=font(15, True), fill=(197, 119, 255))
    elif i == 4:
        photo(im, "system-shock.jpg", (570, 107, 632, 498))
        text_block(d, ["THE ROOM", "COMES ALIVE."], ["Curved screen. Ambilight.", "Glass walls and reflections."], size=51)
    elif i == 5:
        photo(im, "elite.jpg", (570, 107, 632, 498))
        text_block(d, ["GAMES.", "AND VIDEO."], ["Bring a browser window to", "the same 3D VR screen."], size=53)
    elif i == 6:
        photo(im, "skyrim.jpg", (570, 107, 632, 498))
        text_block(d, ["PLAY YOUR", "OWN WAY."], ["Keep your keyboard and mouse.", "No motion controllers needed."], size=52)
    else:
        logo = Image.open(ASSETS / "logo.png").convert("RGBA")
        logo.thumbnail((160, 160))
        im.paste(logo, (560, 99), logo)
        d.text((W//2, 273), "VRX3D", font=font(106, True), fill=WHITE, anchor="mt")
        d.text((W//2, 416), "PLAY FLAT. EXPERIENCE DEPTH.", font=font(40, True), fill=CYAN, anchor="mt")
        d.rounded_rectangle((438, 520, 842, 591), radius=20, fill=CYAN)
        d.text((W//2, 540), "DOWNLOAD FREE", font=font(29, True), fill=BG, anchor="mt")
    footer(d)
    return cv2.cvtColor(np.asarray(im), cv2.COLOR_RGB2BGR)


def main():
    frames = [scene(i) for i in range(8)]
    out = str(ROOT / "promo_silent.mp4")
    writer = cv2.VideoWriter(out, cv2.VideoWriter_fourcc(*"mp4v"), FPS, (W, H))
    if not writer.isOpened():
        raise RuntimeError("Could not open video writer")
    per_scene = round(FPS * SCENE_SECONDS)
    for idx, still in enumerate(frames):
        for t in range(per_scene):
            progress = t / max(1, per_scene-1)
            scale = 1.0 + 0.018 * progress
            crop_w = int(W / scale)
            crop_h = int(H / scale)
            # A slow, deliberate push gives the stills subtle motion without simulating VR depth.
            x = int((W-crop_w) * (0.25 + 0.5*progress))
            y = int((H-crop_h) * 0.5)
            current = cv2.resize(still[y:y+crop_h, x:x+crop_w], (W,H), interpolation=cv2.INTER_LINEAR)
            if idx and t < FADE_FRAMES:
                alpha = t / FADE_FRAMES
                current = cv2.addWeighted(frames[idx-1], 1-alpha, current, alpha, 0)
            writer.write(current)
    writer.release()
    print(out)


if __name__ == "__main__":
    main()
