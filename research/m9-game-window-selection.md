# M9 — capture the game, not the terminal

Date: 2026-09-17

The user launched the suggested title-based command and saw the command prompt
in the headset. The old selector stopped at the first visible window containing
`HELLDIVERS` in its title. A terminal displaying the launch command could match.

## Change

- `--exe=helldivers2.exe` matches the owning executable filename exactly,
  case-insensitively, using limited process-information access. No injection or
  reading of game memory is involved. The title may be empty or change.
- Title selection excludes console/Windows Terminal classes, shell executables,
  and VRX's own process. Combining executable and title requires both to match.
- Selection must yield exactly one visible window with a nonempty client area.
  Missing or ambiguous matches fail explicitly; there is no fallback to desktop
  capture. Multiple game windows can be narrowed with an additional title filter.
- The selected HWND, PID, executable, and UTF-8 title are logged. This also fixes
  the previous blank title log for `HELLDIVERS™ 2`.
- `--check-source` validates the source without starting OpenXR, capture, or AI.
- `Play-Helldivers2.cmd` sets the working directory and launches a 30-minute
  session with exact executable selection. Start SteamVR and the game first.

## Verification

The full native build and standalone playback suite pass. Selection regression
checks cover a command-line title containing the game's name, Windows Terminal,
unavailable process identity for a console, exact/case-insensitive executable
matching, own-process exclusion, empty game titles, and combined filters.

Desktop source validation found the actual game: PID 50204,
`helldivers2.exe`, title `HELLDIVERS™ 2`, HWND `00000000000A0CA4`.
The sandbox cannot enumerate the user's desktop, so this check was run outside
the sandbox. It did not start headset playback. Capture presentation with this
new selector remains to be confirmed by the user.
