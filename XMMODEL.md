# Two-model depth fusion (xmmodel branch)

Question: can VRX run ZipDepth and Depth Anything V2 together and fuse them, to get
DA-V2's depth quality at ZipDepth's speed?

Short answer: **yes, when DA-V2 is fast enough (a second GPU, or an idle fast GPU),
and only if the slower model's result is moved forward to the current frame using
the GPU's hardware motion estimator.** Averaging the two models, or fitting one to the
other without motion compensation, gives much less. Everything below was measured
offline on recorded footage; nothing has been tried in the headset yet.

## The method that works

Each displayed frame *i*:

1. **ZipDepth** runs on frame *i*, as today (2 ms on the 3090).
2. **DA-V2** runs in the background on the depth GPU (25.5 ms per pass on the RTX
   3060, measured in v1.2). Its newest result is for some older frame *k*.
3. The GPU's **hardware motion estimator** (D3D12 video motion estimation, on the
   video engine, not the shader cores) matches frame *i* against frame *k* in 8x8
   blocks. The DA-V2 map for *k* is moved to where that content is in frame *i*.
4. **Motion trust:** the picture of frame *k* is moved with the same vectors and
   compared with frame *i*. Where they match (within ~10 grey levels, fading to zero
   trust at ~30), the move is trusted. Where they don't (occlusions, blocks the
   estimator got wrong), ZipDepth is used there instead, mapped onto DA-V2's scale with
   one global fit. If less than half the frame is trusted (a whip-pan or scene cut),
   that frame is plain ZipDepth.
5. **Fit:** ZipDepth for frame *i* is fitted to that target with a smooth
   per-region scale and shift (a guided filter with ZipDepth as the guide, Gaussian
   window sigma 16 px on the 686x392 grid). Fine detail and edge positions come from the
   current ZipDepth frame; the larger-scale layout and relative depths come from DA-V2.

## Results

Four clips (three from a Star Citizen capture with a third-person character, one
real-camera clip from ZipDepth's repository), 1,064 frames in total. Scores are means
over the clips, and each percentage is the change against ZipDepth alone.
**"Ready after"** is how long after its frame a DA-V2 result becomes usable. The v1.2
3060 measurement puts the second GPU at about 27 ms.

| DA-V2 ready after | Method | Structure error | Near-object error | Flicker | Edge alignment |
|---|---|---:|---:|---:|---:|
| - | ZipDepth alone (today) | 0.0647 | 0.0516 | 0.0360 | 0.7874 |
| 27 ms | DA-V2 alone, lagged as it would be live | -48% | -49% | -14% | **-21%** |
| 27 ms | Naive average of both | -35% | -29% | -6% | -5% |
| 27 ms | **Fused, hardware vectors + trust (16 px)** | **-59%** | **-52%** | **-16%** | **-3%** |
| 60 ms | **Fused, hardware vectors + trust (16 px)** | -48% | -45% | -10% | -4% |
| 120 ms | **Fused, hardware vectors + trust (16 px)** | -34% | -35% | -14% | -4% |
| - | DA-V2 every frame, no lag (unachievable target) | -100% | -100% | -21% | -3% |

What the scores mean:

- **Structure error:** the mean difference from DA-V2 run on every frame with no lag,
  after the best overall scale and shift. It measures how DA-V2-like the depth is.
- **Near-object error:** the same, only where DA-V2 says something is near, such as
  characters and weapons.
- **Flicker:** frame-to-frame change after following the motion with optical flow,
  divided by the frame's depth spread. It measures shimmer.
- **Edge alignment:** the share of the strongest depth edges within 2 px of an edge
  in the *current* picture. Lagged depth scores low.

Per clip at 27 ms, the fused result beat ZipDepth alone on structure and flicker on
all four clips, and lost 1.4% to 4.4% on edge alignment. At 27 ms the motion
estimator's matches were trusted for 93-99% of pixels on average.

Pictures and videos: `bench/xmmodel/out/<clip>/sheet.png` and `compare.mp4` (27 ms).
Each shows the game frame, ZipDepth alone, DA-V2 lagged, the fused result, the
motion-trust map and the DA-V2 target. These files are not committed.

### Steadying against the previous frame (no second model)

The same motion estimator and trust check can steady ZipDepth against **its own**
previous output:

1. Move the previous output to the current frame with frame-to-frame vectors
   (0.43 ms per estimate on the 3060).
2. Blend it 50/50 with the current ZipDepth where the motion is verified.
3. Use ZipDepth alone elsewhere.

Across the four clips:

| Method | Structure error | Flicker | Edge alignment |
|---|---:|---:|---:|
| ZipDepth steadied (no DA-V2) | -1% | **-30%** | -1% |
| Fused (27 ms, 16 px, verified), then steadied | -55% | **-36%** | -3% |

Steadying cut flicker on every clip, from -14% (`zd_clip`) to -42% (`sc_hall`). It
needs no second model or GPU, so it would help every user. Caveat: the flicker score
follows motion the same way the steadying does, so it may flatter it. Any trailing or
smearing on moving edges has to be judged in the headset.

### Side-by-side 3D videos

`bench/xmmodel/xm_sbs.py` renders each clip through VRX's own stereo warp
(`bench/native/xmmodel/sbs_render.cpp`, a CPU port of `kWarpHlsl`) with:

- the default screen, 5.7 m wide at 3 m
- 63 mm IPD
- ZipDepth's near-field dilation

The renders are 1920x1080 per eye, full side-by-side. There are four variants,
labelled in both eyes:

- A: ZipDepth alone
- B: ZipDepth steadied
- C: fused
- D: fused + steadied

Each variant is written separately, plus a `<clip>_ALL_SBS_LR.mp4` with all four back
to back. The files go to `bench/xmmodel/out/sbs/` and are not committed.

### What did not work

- **Naive average** (both models, globally matched): it roughly halves the benefit,
  and the lagged DA-V2 edges blur the result.
- **Fitting ZipDepth to the lagged DA-V2 without motion compensation:** the fit stays
  where objects *were*. Structure improves 44%, but flicker gets no better.
- **A global tone curve** ("ZipDepth value x means DA-V2 value y"): structure
  improves only ~6%, and flicker gets worse (+11%) as the curve shifts. So the
  difference between the models is spatial, not a global remapping.
- **Flattening depth where the models disagree:** it costs edge alignment (-10%) for
  no gain.
- **Motion compensation without the trust check:** during fast pans the 8x8 vectors
  break down into blocky noise. The fused result then copies a wrong layout (visible
  in `sc_walk/sheet.png`), sometimes worse than ZipDepth alone. The trust check fixes
  this, and it gains the most at longer lags.

## Hardware motion estimator: measured

`bench/native/xmmodel/me_probe.exe` (D3D12 `ID3D12VideoDevice1::CreateVideoMotionEstimator`,
`ID3D12VideoEncodeCommandList::EstimateMotion` / `ResolveMotionVectorHeap`):

- **Support:** both the RTX 3090 and the RTX 3060 support 8x8 and 16x16 blocks at
  quarter-pel precision, for NV12 input from 32x32 to 4096x4096. So the 686x392 depth
  grid works directly.
- **Synthetic check:** a known 6,3 px shift gave exactly (-24, -12) quarter pels on
  100% of blocks. The vector points from the current block to where its content was
  in the reference frame, which is exactly the lookup needed to move the old map.
- **Real frames:** 4,224 real frame pairs from the clips took 0.45 ms p50 and
  0.55 ms p95 per estimate on the 3060, measured from submit to done.
- **Caveat from Microsoft's docs:** encoder motion estimators "may be optimized for
  improving compression" rather than true motion. On these clips they did slightly
  better than a block-averaged Farneback optical-flow stand-in: structure -54% against
  -50% at 27 ms with 16 px regions, before adding the trust check.

## Caveats

- Everything is offline. The real test is the headset, especially Helldivers 2, which
  is not in these clips.
- The clips run at ~28 fps and the engine captures at up to 60 fps. That means less
  motion per frame, which should help motion estimation, but this is untested.
- **Structure scores closeness to DA-V2, not to real depth.** They are only good news
  if DA-V2's layout is better. The sheets suggest it is more detailed, for example the
  character's layers and the seats, but that is a judgement call for your eyes.
- The flicker score rewards frozen depth. DA-V2 held for 120 ms looks "steady"
  because it barely changes.
- DA-V2 run on every frame itself scores lower on edge alignment than ZipDepth. So a
  few percent of edge loss partly reflects DA-V2, not the fusion.
- Both models ran on the CPU for the cache. ZipDepth used the same FP16 export the
  engine ships.

## Engine implementation (xmmodel branch)

There are two independent per-game tickboxes in the desktop app, and both apply live.
After the first headset test (fused + steadied looked clearly better), steadying is on
by default, including for existing profiles, and fusion is off by default.

- **Steady depth — motion vectors:** works on any GPU that has a hardware motion
  estimator.
- **Fuse with Depth Anything V2 (experimental):** needs ZipDepth as the main model,
  and works best with a second depth GPU.

The engine flags are `--steady` and `--fuse`. The control snapshot is now v5, which
adds the two flags after the fast-model flag. v1–v4 launchers leave both off.

What the engine does per depth pass when either option is on (`xrapp5.cpp`,
`PostProcessDepth`):

1. **Grid image** (`kGridHlsl`): the frame is box-filtered onto the 686x392 depth grid
   as RGBA8 and read back (1 MB). It is queued on the ml queue right after the model's
   input, so it is made while the model runs.
2. **Motion** (`motion_estimator.h`): the frame's luma is uploaded to an NV12 slot, and
   the hardware estimator runs on its own `VIDEO_ENCODE` queue. It needs up to two
   estimates, against the anchor frame and against the previous frame. Both go in one
   submission and are read back as 86x49 vectors each.
3. **Fusion** (`depth_fusion.h`, a port of `xm_fuse.py`): the grid image goes to the
   anchor thread. The anchor is Depth Anything V2 on its own DIRECT queue on the depth
   GPU, at normal priority, with a CPU input tensor. Without a second GPU it is capped
   at 10 passes per second. With the newest anchor result, the engine:
   - fits it globally onto the fast model's map of the same frame
   - moves it to the current frame
   - applies the trust check, then the guided fit (sigma 16)

   Anchors older than 0.3 s only supply the global scale. That keeps the depth in
   the anchor's range instead of jumping between the two models' ranges.
4. **Steadying:** the previous output is moved to this frame and blended 50/50 where
   the motion is verified.
5. Then come `DilateNear` and publish, as before.

The ONNX Runtime sessions no longer spin their CPU threads after each pass
(`session.intra_op.allow_spinning=0`). DirectML runs the models on the GPU, and the
spinning took cores from the post-processing.

Measured with `xrplayer.exe --selftest --steady --fuse`. That is 6 s of real passes on
the captured desktop, one RTX 3090, with another GPU workload using the machine:

| Measure | Result |
|---|---|
| Depth passes | 183, all finite and in range |
| Passes steadied | 182 |
| Passes fused | 155 |
| Anchor passes | 48 (capped at 10 per second) |
| Post-processing | p50 11.9 ms, p95 14.9 ms |

Per pass, on average:

| Stage | ms |
|---|---:|
| Grid image + luma | 0.5 |
| Luma upload | 0.5 |
| Anchor hand-over | 0.8 |
| Both motion estimates | 1.1 |
| Fusion maths (CPU) | 5.0 |
| Steadying maths (CPU) | 3.8 |

The CPU maths is the cost that matters: it adds to each depth pass. If the headset
shows depth updates slowing, the next step is to move the maths into compute shaders.
All inputs except the anchor map are already on the GPU.

Tests:

- **`fusion_golden.exe`:** the C++ maths against `xm_fuse.py` on real sc_walk frames,
  including a partial-trust frame and a whip-pan frame where trust is cut. Mean
  differences are at most 0.000016 for trust and 0.000003 for fused and steadied
  depth.
- **`playback_test`:** properties of the maths (identity motion, whole-frame cut,
  fallback and affine reproduction, steady blend), and parsing of the v5 settings.
- **`--selftest`:**
  - the grid shader against the CPU (0 of 268,912 pixels differ)
  - hardware motion on a known 6,3 px shift (3,948 of 3,948 blocks exact)
  - a steady blend
  - with `--steady`/`--fuse`, the pipeline run above
- **Desktop smoke test:** checkbox mapping, fusion disabled without ZipDepth, old
  profiles load with both off, and the v5 control snapshot.

Still to check in the headset:

- whether depth updates per second drop noticeably with the options on (the
  worker's 2-second log line shows passes, post-processing ms, and steadied/fused
  counts)
- smearing on moving edges
- whether ShadowPlay or OBS recording competes for the video encoder

## Reproducing

From `bench/` (CPU only for the models; the probe uses the chosen GPU's video engine):

```
.venv-dml/Scripts/python.exe xmmodel/xm_cache.py xmmodel/out/sc_ship "<capture.mp4>" --start=8 --seconds=8
.venv-dml/Scripts/python.exe xmmodel/xm_fuse.py xmmodel/out/sc_ship --export-mv=27
native/xmmodel/build.bat
native/xmmodel/out/me_probe.exe --pairs=xmmodel/out/sc_ship/me_27 --adapter=1
.venv-dml/Scripts/python.exe xmmodel/xm_fuse.py xmmodel/out/sc_ship --lat=15,27,60,120 --sigma=16,24,48 --show-lat=27 --show-sigma=16
.venv-dml/Scripts/python.exe xmmodel/xm_report.py xmmodel/out/sc_ship xmmodel/out/sc_walk ...
.venv-dml/Scripts/python.exe xmmodel/xm_fuse.py xmmodel/out/sc_ship --export-prev=1
native/xmmodel/out/me_probe.exe --pairs=xmmodel/out/sc_ship/me_prev --adapter=1
.venv-dml/Scripts/python.exe xmmodel/xm_sbs.py xmmodel/out/sc_ship --lat=27 --sigma=16
```

`xm_sbs.py` needs `imageio-ffmpeg` in the venv, for its bundled H.264 encoder.

`me_probe.exe` with no arguments runs the capability, synthetic-accuracy and timing
check on every GPU. `xm_golden.py <clip_dir> <out_dir> --frame=N` exports one frame for
`fusion_golden.exe <out_dir>`.

Clips used: a Star Citizen capture at 8 s, 123 s and 284 s (8 s each), and
`bench/models/zipdepth/assets/examples/clip.mp4` (12.6 s).
