"""The shot list for the VRX promo, as data.

Every shot names the real VRX asset it is built from and, where it makes a claim,
the place in the repository that makes that claim true. tools/write_shotlist.py
renders SHOTLIST.md from this file so the document cannot drift from the film.

Sources of truth used below:
    README.md            "Play flat PC games on a stereo screen in VR using your
                         keyboard and mouse... captures the selected game window,
                         estimates depth, and creates a stereo view."
    README.md            ZipDepth "about 6x less GPU time per estimate than
                         Depth Anything V2 Small (2.0 vs 13.1 ms on an RTX 3090)"
    README.md            "start your game and bring it to the front; VRX counts
                         down and attaches by itself"
    README.md            Screen curve / Ambilight / Room (XCURVE.md, XROOM.md)
    vrx3d.uk             "Play flat PC games on a big stereo 3D screen in VR."
    docs/vrx-headset.png real headset capture, captioned on the site as
                         "Helldivers 2 on a curved screen in the room, as seen in
                         the headset"
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

FPS = 30
TOTAL_SECONDS = 40.0


@dataclass(frozen=True)
class Shot:
    key: str
    title: str
    seconds: float
    assets: tuple[str, ...]
    shows: str          # the VRX feature this shot demonstrates
    caption: tuple[str, ...]
    source: str         # where the claim/asset comes from
    notes: str = ""


SHOTS: tuple[Shot, ...] = (
    Shot(
        key="flat",
        title="Flat",
        seconds=3.4,
        assets=("promo/assets/cyberpunk.jpg", "promo/assets/cyberpunk-depth.png"),
        shows="The problem: a flat game capture, no depth at all.",
        caption=("Your games are flat.",),
        source="promo/assets/cyberpunk.jpg is a real game capture published in the repo",
        notes="Opens on a monitor-style switch-on, then holds still. The held "
              "stillness is the point: nothing moves until depth arrives.",
    ),
    Shot(
        key="depth",
        title="Depth",
        seconds=3.8,
        assets=("promo/assets/cyberpunk.jpg", "promo/assets/cyberpunk-depth.png"),
        shows="Depth estimation: VRX works out a depth map for the frame.",
        caption=("VRX estimates depth", "for every frame."),
        source="README.md: captures the window, estimates depth, creates a "
               "stereo view; ZipDepth 2.0 ms per estimate on an RTX 3090",
        notes="A vertical wipe reveals the app's real depth map for the same "
              "frame, tinted in the brand ramp, with the model name as a label.",
    ),
    Shot(
        key="window",
        title="Window",
        seconds=10.4,
        assets=("promo/assets/cyberpunk.jpg", "promo/assets/cyberpunk-depth.png",
                "promo/assets/helldivers.jpg", "promo/assets/helldivers-depth.png",
                "promo/assets/system-shock.jpg", "promo/assets/system-shock-depth.png"),
        shows="The payoff: the real depth maps drive real parallax, so the "
              "capture gains a stereo sense of depth.",
        caption=("Your screen becomes a window.",),
        source="Each frame is the real capture warped by the app's real depth "
               "map for that capture (see tools/analyse_depth.py for the "
               "alignment and convention check)",
        notes="Three games, ~3.47 s each: Cyberpunk 2077, Helldivers 2, System "
              "Shock. The camera drifts and dollies; the near field moves much "
              "further than the far field. The last beat pulls back to a screen "
              "in the room, which becomes the next shot.",
    ),
    Shot(
        key="room",
        title="Room",
        seconds=7.9,
        assets=("docs/vrx-headset.png",),
        shows="What it looks like in the headset: a curved screen in a dark "
              "room, lit by the picture.",
        caption=("A curved screen,", "in a dark room,", "lit by the picture."),
        source="docs/vrx-headset.png, the real capture the site captions "
               "'Helldivers 2 on a curved screen in the room, as seen in the "
               "headset'; XCURVE.md and XROOM.md for curve, ambilight and room",
        notes="Feature chips name what is on screen. The capture is shown "
              "whole, never stretched.",
    ),
    Shot(
        key="easy",
        title="Easy",
        seconds=6.6,
        assets=("promo/assets/easy-mode.png",),
        shows="Starting a game: Easy mode attaches by itself.",
        caption=("Start your game.", "VRX does the rest."),
        source="promo/assets/easy-mode.png is the real Easy-mode window; README.md "
               "'start your game and bring it to the front; VRX counts down and "
               "attaches by itself'",
        notes="Three-step line: start the game, VRX attaches, play. The window "
              "is the real capture, unaltered.",
    ),
    Shot(
        key="end",
        title="End card",
        seconds=7.9,
        assets=("docs/vrx-logo.png",),
        shows="Free and open source, where to get it.",
        caption=("Play flat PC games", "on a big stereo 3D screen in VR."),
        source="README.md states VRX is free and open source; vrx3d.uk is the "
               "product site; requirements from README.md",
        notes="Real logo, site tagline, download call to action.",
    ),
)


def schedule() -> list[tuple[str, float, float]]:
    """(key, start, end) for each shot, in order."""
    out, t = [], 0.0
    for s in SHOTS:
        out.append((s.key, t, t + s.seconds))
        t += s.seconds
    return out


def total() -> float:
    return sum(s.seconds for s in SHOTS)


if __name__ == "__main__":
    if abs(total() - TOTAL_SECONDS) > 1e-6:
        raise SystemExit(f"shot list is {total():.2f}s, expected {TOTAL_SECONDS:.2f}s")
    for key, a, b in schedule():
        shot = next(s for s in SHOTS if s.key == key)
        print(f"{key:8s} {a:6.2f} -> {b:6.2f}  ({shot.seconds:4.2f}s)  {shot.title}")
    print(f"total {total():.2f}s = {int(total() * FPS)} frames at {FPS} fps")
