# Depth banding: the "ploughed field" (xbands branch)

Reported from play: scenes with grass showed evenly spaced ridges across the ground,
as if the field had been ploughed, strongest when the ground was around a quarter of
the way into the scene's depth.

## Cause

VRX's warp shifted each source pixel by a WHOLE number of pixels
(`xrapp5.cpp` kWarpHlsl: `int r = lround(scaleFocal * eye * invZ)`). With the default
screen (5.7 m wide at 3 m, 63 mm IPD, 1920 px colour width) the focal length is
1011 px, so the shift spans only **+15.9 px (1.2 m) to -8.0 px (12 m)**: about
**25 distinct shifts for the entire depth range**. The near value has to change by
~0.042 (of 0..1) before the picture moves at all.

A smoothly receding surface therefore becomes flat stripes with a 1 px step between
them. On a synthetic field (`bench/xmmodel/xm_bands.py`):

- 24 ridges, evenly spaced **13-14 px apart**, all within one band of the image - the
  middle distance, which matches the report;
- along a column through the field, **94% of rows got identical depth**, with 24
  one-pixel cliffs between them.

The ridges are invisible in one eye: a 1 px shift step only exists as depth once both
eyes are fused.

## Fix: sub-pixel warp

The warp already computes the exact fractional shift and then rounds it. Sub-pixel
keeps it: the scatter stores where each source pixel really lands, and the output
samples the colour at the fractional source position that lands on this destination,
blended from its two neighbours. Filled (disoccluded) pixels have no continuous
mapping and stay whole-pixel.

On the same field, the shift each row receives becomes:

| | Distinct values | Biggest jump | Rows with no change |
|---|---:|---:|---:|
| Whole pixels (before) | 25 | 1.000 px | 94% |
| Sub-pixel | 330 | 0.075 px | 15% |

Cost: one extra value per destination pixel (the warp scratch buffer grows from 2 to
3 values per pixel) and a two-texel blend instead of a direct read. The blend softens
the picture very slightly, which is why the setting can be turned off.

Setting: **Smooth depth steps (sub-pixel warp)**, per game, default on, applies live
(v7 control snapshot). Engine flag: `--whole-pixel` restores the old warp.

## Verification

- `SelfTestWarp` compares the GPU warp with the CPU reference (`xr_common.h`
  WarpEyeFill) pixel for pixel, in both hole-fill modes and at 4x exaggerated
  disparity. Whole-pixel stays bit-exact; sub-pixel agrees **within one last bit** per
  channel, because the GPU's UNORM store rounds ties to even where `lroundf` rounds
  away from zero. Depth output is unchanged.
- The self-test caught a real bug during development: the CPU reference looked up the
  fractional position by source index instead of destination index (300k mismatched
  pixels).
- `bench/xmmodel/xm_bands.py` measures the banding, renders both warps, and with
  `--sbs` writes side-by-side 3D videos (still scenes) for judging in a headset.
  `sbs_render.exe --subpixel` does the same warp offline.
