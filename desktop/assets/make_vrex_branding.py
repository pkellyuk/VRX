"""Regenerate the badge/speech data in vrex_branding.h. Requires Pillow on Windows.

Run from any directory. Model and bubble shader functions in the header are preserved.
The English phrase IDs are stable; see docs/INTERNATIONALIZATION.md for locale rollout.
"""
import json
import re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[2]
header = root / "bench/native/openxr/vrex_branding.h"
image = Image.open(root / "docs/vrx-logo.png").convert("RGBA")
background = Image.new("RGBA", image.size, (8, 12, 20, 255))
background.alpha_composite(image)
indexed = background.convert("RGB").resize((32, 32), Image.Resampling.LANCZOS).quantize(colors=16)
palette = indexed.getpalette()
pixels = list(indexed.get_flattened_data())
logo = [sum(pixels[i+j] << (j*4) for j in range(8)) for i in range(0, 1024, 8)]
phrases = json.loads((Path(__file__).with_name("vrex-lines.en.json")).read_text(encoding="utf-8"))
assert len(phrases) == 8, "Speech phrase IDs must remain stable"
font = ImageFont.truetype("C:/Windows/Fonts/seguisb.ttf", 12)
speech = []
for phrase in phrases:
    canvas = Image.new("L", (128, 20))
    draw = ImageDraw.Draw(canvas)
    box = draw.textbbox((0, 0), phrase, font=font)
    width = box[2] - box[0]
    assert width <= 124, f"Shorten the phrase to fit the speech atlas: {phrase}"
    draw.text(((128-width)/2, 1), phrase, font=font, fill=255)
    for y in range(20):
        for x in range(0, 128, 32):
            speech.append(sum((canvas.getpixel((x+j, y)) > 96) << j for j in range(32)))

source = header.read_text(encoding="utf-8")
def replace_array(name, declaration):
    global source
    source, count = re.subn(r"static const (?:uint|float3) " + name + r"\[\d+\] = \{[^}]+\};", declaration, source)
    assert count == 1, f"Missing shader data array: {name}"

replace_array("VREX_LOGO", "static const uint VREX_LOGO[128] = {" + ",".join(f"0x{word:08x}u" for word in logo) + "};")
colours = ["float3(" + ",".join(f"{palette[i*3+c]/255:.6f}" for c in range(3)) + ")" for i in range(16)]
replace_array("VREX_COLOURS", "static const float3 VREX_COLOURS[16] = {" + ",".join(colours) + "};")
replace_array("VREX_SPEECH", "static const uint VREX_SPEECH[640] = {" + ",".join(f"0x{word:08x}u" for word in speech) + "};")
menu = []
menu_font = ImageFont.truetype("C:/Windows/Fonts/seguisb.ttf", 10)
menu_phrases = json.loads(Path(__file__).with_name("vrex-menu.en.json").read_text(encoding="utf-8"))
assert len(menu_phrases) == 2, "Interaction option IDs must remain stable"
for phrase in menu_phrases:
    canvas = Image.new("L", (128,20)); draw = ImageDraw.Draw(canvas)
    box = draw.textbbox((0,0),phrase,font=menu_font); width = box[2]-box[0]
    assert width <= 124, "Menu text must fit its atlas"
    draw.text(((128-width)/2,2),phrase,font=menu_font,fill=255)
    for y in range(20):
        for x in range(0,128,32):
            menu.append(sum((canvas.getpixel((x+j,y))>96)<<j for j in range(32)))
replace_array("VREX_MENU", "static const uint VREX_MENU[160] = {" + ",".join(f"0x{word:08x}u" for word in menu) + "};")
header.write_text(source, encoding="utf-8", newline="\n")
print("Regenerated Vrex badge and eight speech phrases")
