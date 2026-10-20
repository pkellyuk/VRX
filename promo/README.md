# VRX promo video

A 40-second promo for VRX, built from the product's own published assets.

| Deliverable | What it is |
|---|---|
| [`VRX-promo-1080p.mp4`](VRX-promo-1080p.mp4) | The master: 1920x1080, H.264, AAC stereo. |
| [`VRX-promo-vertical-1080x1920.mp4`](VRX-promo-vertical-1080x1920.mp4) | The social cut: 1080x1920, same score, recomposed rather than letterboxed. |
| [`VRX-promo-poster.png`](VRX-promo-poster.png) | A still from the master, for a thumbnail. `-alt*.png` are alternates. |
| [`SHOTLIST.md`](SHOTLIST.md) | Every shot, its timings, its assets and the source of its claims. Generated, so it cannot drift from the film. |

## Rebuilding it

```sh
python promo/film/render_film.py            # both masters and the poster
python promo/film/render_film.py wide       # just the 16:9 master
python promo/film/render_film.py tall       # just the 9:16 cut
python promo/film/render_film.py poster     # just the stills
python promo/tools/verify_promo.py          # check every deliverable
```

`render_film.py` needs `numpy`, `opencv-python`, `Pillow` and `scipy`, and an
ffmpeg with libx264. `tools/fetch_ffmpeg.py` puts a modern essentials build in
`tools/`; the film prefers that over whatever is on `PATH`, because the ffmpeg
that ships with some Python distributions is a 2013 build with a much older
x264. Rendering both masters takes about fifteen minutes on one machine.

## What is real, and what is generated

**Real, taken unaltered from the repository:**

- The gameplay captures in `assets/` - Cyberpunk 2077, Helldivers 2, System
  Shock - and the app's own depth maps for each of them.
- `docs/vrx-headset.png`, the capture the site captions "Helldivers 2 on a
  curved screen in the room, as seen in the headset".
- `assets/easy-mode.png`, the real Easy-mode window.
- `docs/vrx-logo.png`, the real logo.

**Generated for the film:**

- The depth parallax. Each frame of the three Window shots is the real capture
  repositioned by the app's real depth map for that capture, using a proper
  camera model - see `film/vrx_dibr.py` and the note in `SHOTLIST.md`. Nothing
  is drawn in: the picture is the picture, moved according to the depth the
  product computed. Where a camera move reveals a sliver that was never
  captured - only ever a few pixels wide behind a near object - it is filled by
  inpainting, and nothing else in the frame is invented.
- The room around a screen, where one appears. The glow is the real border
  colours of the real capture, blurred outward, which is what the app's own
  ambilight does.
- The score (`film/vrx_score.py`). Every part is synthesised - oscillators,
  envelopes, filters, a convolution reverb built from decaying noise. There are
  no samples, so it carries no third-party rights.
- All the type and layout.

**Claims.** Every statement on screen is one the repository already makes; the
Source column of `SHOTLIST.md` names where. The one number used - "ZipDepth -
2.0 ms per estimate on an RTX 3090" - is from `README.md`, which in turn cites
the project's own measurements.

## How it is put together

The film is a software render, not an edit. `film/vrx_film.py` composes every
frame from the assets at both aspect ratios: `vertical=True` moves the picture
into a panel with the captions above and below it, so the social cut is a
recomposition rather than a crop.

```
film/vrx_shotlist.py   the six shots, as data, with the source of every claim
film/vrx_film.py       the film: layout, shots, transitions, both aspect ratios
film/vrx_dibr.py       depth-image-based rendering: the parallax
film/vrx_score.py      the soundtrack
film/vrx_draw.py       text, panels, glows, masks
film/vrx_brand.py      the product's colours and typefaces
film/vrxmedia.py       ffmpeg, frame piping, WAV, mux, probe
film/render_film.py    the entry point
```

The tools under `tools/` are the checks. Each one exists because it caught
something:

| Tool | What it does |
|---|---|
| `verify_promo.py` | Checks the deliverables: size, codecs, duration, a clean decode, that every frame is really there and readably described, that the audio is not silence, that the vertical cut is not letterboxed, and that the film still renders identically to the recorded golden frames. |
| `verify_promo.py --colour` | Compares frames decoded from the delivered files against freshly rendered ones, at both aspect ratios. Worth its own mention: the first cut of this film shipped with red and blue swapped through the whole video, and every structural check passed it. It reports what the colour-swapped comparison scores too, so it proves it is sensitive. |
| `check_delivered.py` | Reads every shipped file back and reports anything wrong with it. This is how a truncated 16:9 encode was caught - 950 decodable frames instead of 1200, with a header that still looked plausible. |
| `analyse_depth.py` | Measures the depth convention and the image/depth alignment instead of assuming them. |
| `test_dibr.py` | Proves the parallax renderer is an exact identity at zero camera movement, and reports how far the picture moves at each depth. |
| `analyse_score.py` | Prints the score's level envelope and spectrum so a hit that lands as a dip, or a section that is accidentally quiet, is visible. |
| `profile_stems.py` | Per-stem levels, which is how a 16 dB bass-over-pad imbalance got found. |
| `test_film.py` | Renders sample frames from both aspect ratios as contact sheets, and audits every headline's fitted type size. |
| `debug_encode.py` | Reproduces the encode path in isolation, to tell a fault in this code apart from a fault in the machine. |
| `ocr_artwork.py` | Reads the wording out of the real logo and UI captures, so captions match the product. |
| `extract_brand.py` | Takes the accent colours from the logo and the published captures. |

## Output is checked, not assumed

Two faults in this build produced files that were valid enough to look right and
were wrong: red and blue swapped throughout, and a video stream that stopped
after 950 of 1200 frames while the audio ran the full length. Neither raised an
error anywhere. So the build no longer trusts its own output:

- `vrxmedia.encode_verified()` writes to a temporary file, reads it back with
  `verify_encoded()` - dimensions, pixel format, H.264 profile, decoded frame
  count, and a clean decode - retries once, and only then replaces the target.
- `vrxmedia.mux()` does the same with its own temporary file.
- `tools/verify_promo.py` re-checks the finished deliverables independently.

## Credits and rights

The film is built from VRX's own assets. The gameplay captures show Cyberpunk
2077, Helldivers 2 and System Shock, and are the captures already published in
this repository and on vrx3d.uk; they are used here to show what the product
does with them. VRX itself is free and open source.
