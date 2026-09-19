# Game frame timing (xsync branch)

On one GPU, depth arrives some time after its game frame. VRX has had two choices:

- **Show the newest game frame.** Motion is smooth and immediate, but moving edges
  are misaligned with the depth.
- **Frame matching.** Each depth result is shown with its own frame: exact, but the
  game image only updates at the depth rate.

This branch adds a third choice: **delayed to depth**.

## Offline comparison (`bench/xmmodel/xm_sync.py`)

This simulates ZipDepth on a busy single GPU: depth arrives `lat` ms after its frame,
back to back. Hardware motion vectors come from the RTX 3060 (`me_<lat>`). The table
is the mean over four clips at 67 ms latency (about 15 depth results per second).

| Timing | Depth mismatch | Edge alignment | Game image age | Game updates/s |
|---|---:|---:|---:|---:|
| A: latest frame (default) | 0.050 | 0.59 | 0 ms | 29 |
| B: matched to depth | 0 | 0.79 | 118 ms | 15 |
| **C: delayed to depth** | **0.016** | **0.72** | 98 ms | **26** |
| D: depth moved to every frame (motion vectors) | 0.045 | 0.68 | 0 ms | 29 |

At 33 ms (about 30 depth results per second):

| Timing | Depth mismatch | Edge alignment | Game image age |
|---|---:|---:|---:|
| C | 0.024 | 0.69 | 36 ms |
| D | 0.034 | 0.72 | 0 ms |

The clips run at 28 fps. What the columns mean:

- **Depth mismatch:** scale/shift-invariant error of the depth shown against the depth
  of the game frame shown.
- **Edge alignment:** the share of the strongest depth edges within 2 px of an edge in
  the game frame shown.

Findings:

- **C keeps most of B's alignment and nearly all of the smoothness.** In a first
  look at the side-by-side videos, C was judged better than A and D, while B was
  still wanted as an option.
- **D, moving depth with motion vectors, mostly fixes edge positions.** ZipDepth's
  estimate changes from frame to frame anyway, so the mismatch barely drops.

Videos (not committed): `bench/xmmodel/out/sbs/<clip>_sync67_ALL_SBS_LR.mp4`, with
A–D labelled in both eyes.

## Engine

The per-game **Game frame timing** list has three settings: Latest frame (default),
Delayed to depth, and Matched to depth. It replaces the "Match game frames to depth"
tickbox. Profiles with frame matching open as Matched to depth. The setting travels
in the v6 control snapshot (`delayed` after `fuse`; matched wins if both are set). The
engine flag is `--delayed`, and the setting applies live.

In delayed mode, the render loop (`frame_timing.h`, unit-tested in `playback_test`)
works like this:

1. It measures the depth delay (capture to publish) of each new depth result,
   smoothed.
2. It keeps the last few captured frames, up to 6. The source ring grew from 8 to 12
   textures for this.
3. It shows the newest kept frame captured at least that long ago, with the latest
   depth. If the depth is newer than that frame, it shows the depth's own frame, as
   matched mode does.

The render log's 2-second line now shows the timing mode, the age of the game frame
shown, and the measured depth delay.
