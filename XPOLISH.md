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

## Twitch: a new frame after a quiet gap went flat

**Cause.** This was most of the twitch on a still desktop. The stale-depth guard
(`UsableDepth`) compares the times of the frame shown and of the frame its depth came
from, and allows 0.25 s. On a still desktop frames come only now and then (a blinking
text cursor: 2 to 7 a second), each more than 0.25 s after the last, so every one was
shown flat for the one to three frames the worker needs to publish its depth, even
when that depth is the previous one carried forward. One session's log had 315
`flat viewing` / `stereo depth available` pairs, one about every half second. With
more happening on screen the frames are closer together and it never showed.

**Fix** (`playback_policy.h`: `UsableDepthAfterQuiet`). Depth that was up to date when
a newer frame arrived stays usable for 0.25 s from then, while the worker catches up.
A worker that never catches up still goes flat when that time ends, and a failed one
at once. After a quiet gap a completely different picture is drawn with the old depth
for those one to three frames instead of flat.

## Edges: the same in both eyes

**Cause.** The warp moves every pixel sideways by its depth, in opposite directions
for the two eyes. Wherever the depth at the picture's left or right edge was not
exactly the screen's, one eye got a strip of mirrored fill there and the other lost
the same strip: the edges had different widths in the two eyes, which the eyes cannot
fuse. A full-screen browser shows it most (its scrollbar is about as wide as the
shift).

**Fix** (`xr_common.h`: `EdgeWeight`, `EDGE_FADE`; the same sums in `kWarpHlsl`). The
shift fades smoothly to zero over a band at each side (4% of the width suits), so the
picture's edges sit on the screen plane and are identical in both eyes. Anything in
that band is drawn nearer the screen plane than its depth says. The width is the
**Flatten the left and right edges** slider in the Depth section (0 off to 15% of the
width each side, off by default, per game, applies live; `EdgeFlatten` in the profile,
the last value of the v12 control snapshot). It was 4% by default at first; it is off
by default now because it is useful on the desktop but flattens whatever a game has at
its edges. Not in head-locked
mode, where zero shift is infinity and not the screen. `--selftest` compares the GPU
with the CPU reference with the fade on, and checks that the outer columns are the
picture's own in both eyes.

## Edges across the picture: a step, not a shear

**Cause.** Desktop icons just above a window had their lower halves slanted at about
45 degrees, right in one eye and left in the other. The warp moves pixels sideways
only, so where the depth changes from row to row every row is moved a little more
than the one above: a shear. A dump of that desktop showed the model's own edge to be
quite sharp (far to near over about 3 depth rows, centred on the window's top border),
but `DilateNear` then widened the near side by 3 depth rows (about 8 of the picture's)
in every direction, which carried the whole ramp up onto the icons: about 17 px of
shift over about 10 rows. Without the widening the ramp sits on the object's own top
rows instead, which lean the other way (why it was added).

**Fix** (`depth_edges.h`, used by `PublishDepth`; `NearAt` in `kWarpHlsl`).

- Down each column of the depth map, a short ramp (up to 6 rows, each changing by at
  least 0.03 the same way, 0.12 or more in all) becomes a step between two rows: at the
  picture's own edge when the luma changes by 6 of 255 levels or more within 2 rows of
  the ramp's middle, else at the middle. The rows above take the ramp's upper value,
  the rows below its lower one. A longer ramp is a real slope and is left alone.
- The near side is still widened sideways, but not up or down across a step.
- The warp no longer blends between two depth rows that differ by more than 0.25 when
  it scales the depth up to the picture (that was a ramp again, 3 picture rows tall);
  each picture row takes the nearer depth row.

The picture is then cut at the edge instead of sheared across it. On the same desktop,
before and after (`--dump`, with `--edge-snap` for after): the icons stand upright
with at most their last 2 rows cut, and the wallpaper just under a window's bottom
edge no longer bends. 0.3% of the picture's pixels changed, nearly all of them along
such edges. The picture's own edges need the picture on the depth grid, which exists
while steady depth or fusion is on; without them the steps sit at the ramps' middles.
The worker's 2-second summary reports the number of column steps per depth map.

**In games** the steps can leave fragments: where a step lands slightly off the real
edge, a piece of the background moves with the object, like a blob hovering over a
character's helmet. So both ways are kept, as three settings in the Depth section (per
game, live, the last three values of the v13 control snapshot):

- **Cut cleanly at depth edges across the picture (off: slant)** (`EdgeSteps`, off by
  default): the steps above, or the ramps as before. Off by default because in the
  headset the fragments around objects were not clearly better than the slant; it is
  there for desktop use, where it keeps icons next to a window upright.
- **Cut only edges at least this deep** (`EdgeStepMin`, 5 to 60% of the depth range,
  12% by default): the least change of depth that is made a step; shallower edges keep
  their ramps. A window over the desktop is about 60%.
- **Widen near objects up and down** (`WidenNear`, 0 to 300% of the model's own rows,
  100% by default): how far `DilateNear` carries a near object's depth above and below
  it, which is where a ramp's slant lands. Nothing is carried across a step.

Not measured: whether a step's row changes from pass to pass on moving pictures, and
which defaults suit games best.

## A hair across text in one eye

**Cause.** Very slight diagonal lines in one eye, like a hair fallen on text. Along
each contour of a smoothly changing depth the shift changes by one pixel. In the eye
that stretches the surface there, two pixels that are neighbours in the picture land
two apart, leaving a one-pixel gap. The warp filled every gap as a hole behind
something nearer: a copied, mirrored pixel. The gaps line up along the contour, so the
copies made a thin line, broken letters where it crossed text ("n" split, "t" cut),
and only in that eye (the other eye squeezes the same place and has no gap). The
sub-pixel warp fixed the banding between contours but left these.

**Fix** (`STRETCH_GAP` in `xr_common.h`, the same in `kWarpHlsl`). With the sub-pixel
warp on, a gap of up to 2 pixels between two pixels at most 2 apart in the picture is
a stretch, not an opening: it is drawn as the picture between its two neighbours, at
the place that lands on it. Wider gaps are filled as before. On a dump of the desktop
before and after, the changed pixels are dotted diagonal lines across the text, and
the broken letters along them are whole. `--selftest` compares the GPU with the CPU
reference as before. Whole-pixel warp (sub-pixel off) is unchanged.

## Thin parts leaning

Detected and fixed, as an experimental setting that is off by default. Checked on one
captured scene and in a short live run; not yet seen in the headset.

**Cause**, from a capture of a castle's spires against mountains. Each spire curves
away from its roof, left in the left eye and right in the right eye. The depth model
does not see the spires at all: in the 686x392 depth map the roofs are rounded shapes
and the spires have the mountain's depth. `DilateNear` widens each roof's depth a
little past its tip, so the foot of a spire is dragged along with the roof and the
rest stays with the mountain. That transition is the curve.

**Detector** (`thin_parts.h`: `FindThinParts`). Both eyes are made from one picture
and one depth map, so the lean of a thin part is known without comparing the eyes: it
is how much its shift changes along it. The depth cannot say where the thin part is,
so that comes from the picture, at its own resolution:

- on each row, a **bar**: two neighbouring edges of opposite sign (12 of 255 levels
  across 2 px) at most 0.7% of the width apart (13 px at 1920), with much the same
  picture either side;
- bars on successive rows that line up (within 2 px, gaps of up to 2 rows) are one
  **part**, if it runs for 1.2% of the height or more (13 rows at 1080). That is short
  because the spires are: 15 to 40 rows. Text strokes, scrollbars and window borders
  are parts too (about 290 parts in the castle scene);
- along each part, its depth is compared with the depth 6 depth pixels either side of
  it. The **lean** is how much that difference changes along the part, in pixels of
  shift per eye; 2 px or more is leaning. Comparing with what is beside it matters: a
  scrollbar on a window that the depth tilts changes with the window and does not
  lean.

**Fix** (`CarryDepthAlongParts`). A part is given the depth of what it is attached
to, along its whole length and the few rows to its foot, when all of these hold:

- it leans (2 px or more per eye);
- within 8 depth rows past one of its ends there is something at least 0.08 nearer
  than what is beside the part (the roof under a spire, the ground under a post);
- it is nowhere more than 0.05 farther than what is beside it (that is a gap between
  two things, not a thing);
- it stands out most at the attached end.

Depth is only ever brought nearer. A text stroke or a scrollbar sits on one depth and
is left alone.

**On the castle capture** (`--dump` writes `xrapp5_thin.ppm`, the parts marked, and
`xrapp5_eye*_thinfix.ppm`, both eyes by the real warp with the fix, next to
`xrapp5_eye*_mirror.ppm` without): of about 290 parts, 15 lean and 3 are given new
depth - the two spires and a pole in front of trees.

- The single spire stands upright in both eyes, from a lean of 11.6 px per eye.
- The pair of spires side by side is only half fixed: one of the two is found, so in
  one eye they still splay.
- Moving a spire leaves a gap in the mountain behind it, and the hole fill there is
  visibly speckled for a few pixels beside the spire's top.
- At half the picture's resolution (960x540) the thinner spire is no longer found, so
  the detector needs the picture at its presented size.

**Live** (`SubmitFine`, `TraceThin` and `PublishDepth` in `xrapp5.cpp`), behind the
setting **Keep thin parts upright (experimental)** in the Depth section (`ThinParts`,
off by default, per game, live; the last value of the v14 control snapshot; renderer
flag `--thin-parts`).

- With each depth pass the picture is also read back at its presented size (at most
  1920 wide, 8 MB) by the same shader that makes the depth grid's picture, and its
  parts are traced on the worker thread, the rows in parallel (`TraceThinParts`).
- Each time depth is published, the depth along the parts is measured and the fix
  applied, after `DilateNear` (`MeasureThinParts`, `CarryDepthAlongParts`). A barely
  changed frame that keeps the last depth keeps the last parts too.
- Measured in a 40 s run on a 4K desktop with the RTX 5060 Ti: finding the parts took
  9.5 ms a pass (including the wait for the picture), about 200 parts in the picture,
  the same 3 given new depth on every pass. That run made about 30 depth passes a
  second; an earlier session without it made about 44, on different content, so the
  drop is indicative only. The worker's 2-second summary reports the three figures.
- The lean is judged in pixels at a nominal view (86.9 degrees, 71 mm) scaled by the
  3D strength, because the worker does not have the headset's real figures.

**Not done.**

- Not seen in the headset. One scene, checked by `--dump`; no game, and no moving
  picture: a part found on one pass and missed on the next would jump between leaning
  and upright, and nothing holds a part over from the pass before.
- With another GPU as the depth GPU it was not tried.
- Parts that are thin across the picture rather than upright are not looked for.

## Depth edges on the picture's edges

An experimental setting, off by default: **Sharpen depth edges with the picture
(experimental)** in the Depth section (`SharpenEdges`, per game, live; the last value
of the v15 control snapshot; renderer flag `--refine`). Checked by tests, `--selftest`
and `--dump` on one captured scene; not yet seen in the headset.

**Cause.** The slants, halos and dragged background around objects all come from one
thing: depth is a 686x392 map whose edges are soft (far to near over about 3 depth
pixels), scaled up to the picture by plain blending. An object's edge in the depth is
a ramp about 8 picture pixels wide that seldom sits on the object's edge in the
picture, and everything on the ramp is moved by an in-between amount. `DilateNear`,
the clean cut and the thin-part fix each patch one symptom of it.

**Fix** (`depth_refine.h`: `RefineAt`, `MarkFlat`, `WidenFine`; `kRefineHlsl`). Each
picture pixel takes its depth from the 7 x 7 depth samples around it, weighted by

- how close the sample is (falling off over 1.5 depth pixels);
- how alike the picture is: the pixel's colour against the picture averaged over the
  sample's own patch (the grid image). A patch that straddles an edge is a mix of both
  sides and is like neither;
- how flat the depth is around the sample (its 3 x 3 range against 0.04): a sample on
  a ramp is the model's in-between value, and a pixel is better served by a sample of
  its own colour a little further off, where the depth has settled.

A pixel with nothing alike around it (busy texture) keeps the plain blend. The near
side is then widened by 2 picture pixels: the picture's own outlines are 2 or 3 pixels
of in-between colour that would otherwise fall to either side one by one, a dotted
fringe beside every edge (seen in the first dumps).

**Live.**

- The worker publishes the depth as estimated (no `DilateNear`, no clean cut; the
  thin-part fix still runs) together with that frame's own picture on the depth grid
  and the flatness (`DepthSlot::guideUp`). It asks for the grid image even with steady
  depth and fusion off.
- The render thread sharpens every frame against the picture it is about to show
  (`RecordRefine`), into a depth buffer at the picture's size that the warp reads with
  `DW x DH = CW x CH`; the warp shader is unchanged. Because the guide is the depth
  frame's picture and the pixel is the shown frame's, depth that is a frame or two
  late still lands on where the object is now, within the 3 depth pixels it looks.
- Cost: 0.49 ms a frame for a 1920x1080 picture on the RTX 5060 Ti with nothing else
  running (`--selftest` reports it). Not measured with a game sharing the GPU.

**Checked.**

- `TestDepthRefine` (`playback_test`): a soft depth edge 8 picture pixels off a
  picture edge steps at the picture's edge; busy texture on smooth depth keeps the
  plain blend to 0.01; a 6 px bar the model half-sees gets one depth across it and
  what is beside it is not dragged along.
- `--selftest`: the GPU pass against the CPU reference, worst difference 0.00000.
- `--dump` on the castle capture (`xrapp5_eye*_refine.ppm` beside `_mirror` as now and
  `_plain` with neither; `xrapp5_near_refined.ppm`): in the depth, roofs come to a
  point and edges sit on the picture's. In the eyes the outlines are clean without
  the 8-pixel widening; against what is shown now the difference on a still picture
  is small, against the plain blend it is large. A few specks remain beside a chimney
  and one roof.

**Not done.**

- Not seen in the headset; no game frame; no moving picture. Edges may flicker with
  picture noise or compression blocks, and the gain for late depth is untested.
- The spires are unchanged: the model did not see them, so there is no depth of theirs
  to place. The thin-part fix is still the answer there.
- Only the main picture; the curved screen and room read the same warped eyes, so
  they follow, but that was not looked at.

## Behind near objects: a background layer

An experimental setting, off by default: **Fill behind objects from a background layer
(experimental)** in the Depth section (`BackLayer`, per game, live; the last value of
the v17 control snapshot; renderer flag `--fill=layer`). Checked by tests, `--selftest`
and `--dump` on one captured scene; not yet seen in the headset.

**Cause.** When a near object is moved sideways for an eye, a strip of what was behind
it comes into view beside it, in that eye only. The picture has nothing there, so the
warp fills it row by row with the background next to it, mirrored (`FillHole`). Each
row is filled on its own, and the strip is sharp picture that one eye sees and the
other does not.

**Fix** (`back_layer.h`: `BuildBackLayer`; `FILL_LAYER` in `xr_common.h`, the same in
`kWarpHlsl`).

- The worker makes one background layer per depth map, on the depth grid, for both
  eyes. A sample is a near object's when it is more than 0.08 nearer than the farthest
  thing within 14 depth pixels along its row (along the row, so a floor that comes
  nearer down the picture is not one). Those samples, and the mixed ones next to them,
  are replaced by the background beside them: the samples within that reach on their
  own and the 2 rows above and below that are no nearer than the farthest thing there,
  the closer ones counting more.
- In the warp, each pixel of an uncovered strip shows the place the background that
  lands there comes from: its own position moved back by the farther side's shift.
  Where the depth at that place is the background's, it shows the sharp picture
  there; where it is a near object's, the layer.
- One or two pixels that land alone inside a strip (bits of background that the depth
  put in between) are treated as uncovered too. Without that they were dots of sharp
  picture on the soft layer.
- The layer is soft (it is on the depth grid, and behind an object it is an average).
  In the headset a soft strip beside every near object read as a frosted-glass border.
  So the layer carries the fine detail of the background beside it: the mirrored pixel
  the old fill would have shown, less the layer at that pixel, is added to it. The
  strip keeps the layer's colours and the picture's texture.

`--selftest` compares the GPU with the CPU reference for the new fill (whole-pixel,
sub-pixel, with the edge fade, and without a layer, which is the mirror fill).
`TestBackLayer` covers the layer: an object in front of two backgrounds is replaced by
them, each side taking after its own, with none of the object's colour.

**On the castle capture** (`--dump`: `xrapp5_eye*_refine_layer.ppm` beside
`xrapp5_eye*_refine.ppm`, `xrapp5_eye*_layer.ppm` beside `_mirror`, and the layers
themselves, `xrapp5_back*.ppm`):

- The strips are one smooth patch each, the same from row to row, in the colours of
  the mountain beside each roof. The speckle that the mirror fill left there is gone.
- They are visibly softer than the textured mountain around them, with a clear
  outline. In a still of one eye the mirrored fill looks more natural; what the layer
  is for (a soft patch in one eye giving way to the other eye's sharp picture, and
  both eyes agreeing on what is hidden) only the headset can show.
- Two faults found on the way and fixed: the first fill (colours spread inward from
  all sides) pulled in the inside of wide roofs, which count as "background" only
  because nothing farther is within reach, and the strips came out dark; and using the
  layer's own low-resolution mask to choose between picture and layer left dotted
  patches, now decided by the depth the warp is using.

**Live.** With the setting on, the worker asks for the picture on the depth grid (as
sharpened edges does), makes the layer with each new estimate and publishes it with
the depth (`DepthSlot::backUp`); a barely changed frame keeps the last one. Measured
on the castle image: 8 ms a depth pass on the worker's CPU threads. The warp's own
cost was not measured separately.

**Not done.**

- Not seen in the headset; no game frame; no moving picture. The layer is as old as
  the depth, so behind a fast-moving object it trails by a frame or two.
- The layer is at the depth map's resolution (2.8 times coarser than the picture).
- Nothing is remembered from earlier frames: what was really behind an object a
  moment ago is not used (stage 3).
