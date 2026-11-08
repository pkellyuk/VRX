# Polishing (xpolishing branch)

From play, after 1.7.9: some twitching is still there, most of all on the desktop, and
thin parts of objects (the spiky top of a rocket) lean left or right.

## Twitch: unchanged parts hold their depth

**Cause.** A fresh depth estimate of an unchanged picture is never exactly the same,
and the model looks at the whole picture, so a change in one place (a video in one
window, typing, scrolling, a game's HUD) moves the depth everywhere a little. 1.7.9
keeps the last depth when less than 0.5% of the picture has changed, but anything more
gets a whole new estimate. Steady depth then blended in only half of the previous
depth (`STEADY_ALPHA` 0.5), which leaves 58% of the estimate's noise.

**Fix** (`depth_fusion.h`: `Stillness`, `Steady` with a stillness map). Each steadied
pass compares the picture on the depth grid with the previous pass's, in place, with no
motion:

- a pixel is **still** when the largest luma change within 2 depth pixels of it is at
  most 3 of 255 levels, and **changed** from 8 levels; a single changed pixel counts in
  full (it is never averaged away by unchanged neighbours, so a mouse pointer or a
  blinking cursor gets fresh depth). Only the still side of the boundary is softened;
- still pixels keep the previous output with weight `STILL_HOLD` 0.9, unmoved;
- changed pixels blend the previous output moved by the motion estimator, where the
  motion is verified, as before (0.5 x trust);
- partly still pixels mix the two.

On a still picture this leaves **0.23** of the raw estimate's noise instead of 0.58
(0.40 of what steady depth left before, measured by `playback_test`). A real change of
depth in a still part settles in about 10 depth passes. Moving content is unchanged.
The worker's 2-second summary reports the share of the picture held as still
(`steadied N (P% still)`).

It needs steady depth (on by default). Nothing changes with it off.

## Thin parts leaning

Not started. Likely cause: the warp scales the 686x392 depth up to the picture by
plain bilinear interpolation (`NearAt` in `kWarpHlsl`), so a spike a few pixels wide
gets a mix of its object's depth and the sky's, fading toward the tip; the tip moves
less than the base and leans the opposite way in each eye. Candidate fix: colour-guided
upsampling. A `--dump` of such a scene would show whether the model saw the spike at
all.
