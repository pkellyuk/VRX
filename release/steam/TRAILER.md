# VRX trailer plan

Working notes for the Steam store trailer. Nothing here is final.

## The reframe that matters

VRX is not a game, so it does not need a cinematic trailer. It needs a
**demonstration video** — the same job the GitHub README does with words and
stills, done in motion. Steam's own trailer doc lists "Gameplay" as a category:
*"When a trailer is mostly comprised of gameplay, showing the user what it's like
to play the game and from the perspective that the player will be playing."* For
a tool, that means showing the thing working.

This is a good fit for a solo dev. There is no voiceover, no actor, no script to
perform: it is screen capture, a few captions, and a music bed.

## The one hard problem: depth does not survive on a 2D screen

A trailer plays back flat. Stereo separation — the entire product — is invisible
in it. So we cannot sell "stereo" directly. What we *can* sell, and what does
survive a flat screen:

1. **Motion parallax.** As the viewpoint moves, near and far layers slide at
   different rates. This reads as depth even in 2D, and it is the strongest cue
   available. Deliberate, slow head movement is therefore the single most
   important thing in the capture.
2. **The room.** A real screen floating in a lit room, with walls catching the
   picture — that is inherently a 3D scene, and depth is obvious in it.
3. **The split view.** Game frame next to the estimated depth map, then the
   result. Explains the product in three seconds.
4. **Honesty.** No fake "wobble" superimposed on flat footage. If it needs a
   label to be believed, label it plainly.

## Capture setup

- Record the **SteamVR display mirror**, one eye, fullscreen (SteamVR settings →
  Display → Mirror). Not the headset's own capture, and not both eyes stacked.
- **OBS**, 1080p60. Prefer 60 fps: Steam accepts 60/59.94 and a VR trailer at 30
  looks wrong. Capture at native panel resolution and scale to 1920x1080 at
  export for a crisp result.
- Capture the **desktop app separately** for the Easy-mode and settings shots.
  Do not rely on one take covering both.
- Drive the head **slowly and deliberately** for parallax shots. Real head
  tracking is jittery and reads as handheld-camcorder footage. Slow drifts and
  gentle arcs, held steady for a beat at each extreme.
- Watch the depth GPU setting: recording adds load, and a capture that steals GPU
  time changes the very depth quality being demonstrated.

## Shot list (~60 s, cut ruthlessly)

| # | ~time | Shot | Why |
|---|-------|------|-----|
| 1 | 0-4 s | Flat game playing on a monitor. Ordinary, unremarkable. | States the problem. |
| 2 | 4-12 s | Same game, in the headset: huge curved screen in the lit room. Slow head drift left to right. | The payoff, and parallax does the depth work. |
| 3 | 12-20 s | Easy mode: pick the window, countdown, attached. | Ease of use — the main objection to VR tools. |
| 4 | 20-28 s | The room: ambilight, glass walls, reflections. | The most visually distinctive thing VRX has. |
| 5 | 28-36 s | Depth map beside the result, for a beat. | Explains *how*, once curiosity exists. |
| 6 | 36-44 s | YouTube in a browser window, now in 3D. | The non-gaming use case, a real differentiator. |
| 7 | 44-52 s | Screen curve and placement on the desktop app. | Shows the depth of configuration. |
| 8 | 52-60 s | End card: VRX, vrx3d.uk, free and open source. | Where to get it. |

Front-load shots 1-2. Steam shows the first two valid trailers ahead of every
screenshot, and most viewers decide inside ten seconds.

## Steam requirements

From the Steamworks "Trailers" documentation:

- Upload in **Store Page Admin → Trailers**. Direct file upload; YouTube is not
  required.
- Highest resolution available, **up to 1920x1080**, **30/29.97 or 60/59.94 fps**,
  high bitrate (**5,000+ Kbps**), `.mov` / `.wmv` / `.mp4`. **H.264 + AAC** preferred.
  **16:9** preferred.
- Audio is transcoded down to **stereo** — do not rely on surround.
- Use a **common resolution and a 44/48 kHz sample rate**; unusual resolution and
  odd sample rates are the two most common causes of a failed conversion.
- Steam auto-generates a **600x380 poster** and a **232x130 thumbnail**. A custom
  poster is allowed but **must be a frame from the video itself**, 1920x1080
  `.jpg`/`.png`, and should avoid text.
- Tagging and country/language restriction live under **Advanced Settings**.

Two operational warnings worth planning around:

- **You cannot release while a trailer is converting.** Conversion runs in a FIFO
  queue and can take hours. Note also that deleting and re-submitting moves you
  to the **back of the queue** — get it right first time.
- **Microtrailers are automatic and cannot be customised.** Steam builds a
  6-second loop from **six 1-second clips taken from various points** in the first
  video. Consequence: there is no safe "boring middle". Any dead second anywhere
  can be sampled into a hub or sale page, so keep the whole 60 seconds alive.

## Music

Suno is usable for this, but only within its terms, and there are three traps.
Checked against Suno's Terms of Service (last revised 10 August 2026, effective
3 September 2026):

- **You must be on Pro or Premier when the track is generated.** Pro/Premier:
  *"Suno hereby assigns to you all of its right, title and interest in and to any
  Output owned by Suno and generated from Submissions made by you."* Free/Basic:
  *"you will only use such Outputs for your lawful, personal and non-commercial
  purposes."* A store page is commercial. So: no free-tier tracks, ever.
- **You must download the track through Suno's own download button.** The
  permitted commercial use is conditional on *"a permitted download of that
  Output in accordance with the download allocations for your applicable service
  tier."* Explicitly prohibited: *"Obtaining a copy of an Output by any means other
  than a download channel made available by Suno is prohibited (for example,
  recording or stream ripping are prohibited)."* So do not record the browser tab
  or rip the stream — use Download.
- **Do not use Remix.** Remixes are non-commercial *"regardless of whether you are
  a free Service tier user or a subscriber to a paid Service tier."* Generating
  from your own prompt is fine; remixing, covering or extending someone else's
  Suno track is not.

The reassuring part: once you have a permitted download on a paid tier, those
rights are *"perpetual and are not affected by your exhaustion of your Download
allotment, by any later change to allotments or pricing, or by the expiry,
cancellation, downgrade or suspension of your subscription."* Cancelling later
does not revoke the trailer's licence.

Two caveats to keep in mind rather than be alarmed by. Suno adds: *"Due to the
nature of machine learning, Suno makes no representation or warranty to you that
any copyright will vest in any Output"* — so you may hold a contractual right to
use it without holding a copyright you could enforce. And these terms bind you and
Suno; they are not a shield against a third party claiming an output resembles
their work.

Housekeeping: keep the downloaded file, the track's Suno ID, the generation date
and proof of the paid tier. That file is the evidence if anyone ever asks.

Alternative if any of that is uncomfortable: a royalty-free library track whose
licence explicitly covers commercial use in promotional video, or the game's own
audio. A commercial track used because it "fits" is the one option that can lose
a store page, so it is not an option.

## Through-the-lens (TTL) footage

TTL is the right instinct and the wrong backbone. VR players trust it precisely
*because* it is not a render — it is proof that the thing is real. But it is
technically awkward, so use it as 2-4 second accents between clean SteamVR mirror
shots, not as the body of the trailer.

Where TTL earns its place:

- **The opening push.** Headset exterior, slow push into one lens, resolving into
  full-frame footage. Strong opening, or a strong transition between sections.
- **One authenticity beat** a third of the way in, so nobody suspects the rest is
  a mockup.

Capture notes for PSVR2 through a phone:

- **Kill the shutter/refresh beat.** The panels run at 90/120 Hz and a rolling
  shutter will band against them. Use a shutter speed that averages whole refresh
  cycles — at 90 Hz (11.11 ms per refresh) **1/30 s is exactly three cycles** and
  bands cleanly; 1/45 s is two. Test before committing.
- **Dark room, and block the gap** between phone and lens with a cloth. Ambient
  light reflecting off the lens surface is the most common way this footage is
  ruined.
- **Manual focus, exposure and white balance.** Autofocus will hunt on a virtual
  image. OLED white balance drifts.
- **Turn image stabilisation off.** It fights deliberate camera moves.
- **Align to the eye box.** The sweet spot is small. A rigid arm or clamp holding
  the phone is essential; handheld will not hold the position.
- **Shoot 16:9 landscape.** Steam prefers 16:9, and cropping vertical phone
  footage into it wastes resolution.
- **Expect vignetting and a circular frame.** Compose for it, or crop in.
- **You cannot see the framing while wearing the headset**, so shoot many short
  takes and review on the phone between them.
- **Record the zoom at high resolution** — 4K or above — so the push can be done
  in the edit with position/scale keyframes, which is far more controllable and
  repeatable than a physical move.

Complementary b-roll that is much easier to film and sells the same point: the
monitor with the game running, and the player at the desk wearing the headset with
hands on keyboard and mouse. VRX deliberately needs no motion controllers, and
that is a selling point worth a shot.

Audio: TTL footage carries no game sound, so it needs a bed or a sync pass.

## Open questions

- Which game to feature. Helldivers 2 is the validated one (README: tested with
  HD2, RTX 3090, PSVR2, SteamVR) and already has stills in `docs/`, but check its
  own publisher's stance on promotional footage before featuring it.
- Whether the trailer is 30 s or 60 s. Shorter is generally better; the shot list
  above can be cut to ~35 s by dropping shots 5 and 7.
- Whether to include an anaglyph (red/cyan) section for viewers with glasses. Only
  if VRX can produce that output honestly.
