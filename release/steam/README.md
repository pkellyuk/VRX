# VRX3D on Steam

Uploading a VRX release to Steam with SteamPipe. VRX does not link the Steamworks API
(`steam_api64.dll`): Steam only installs and launches it, so the GPL-3.0 licence and code
signing are unaffected. The only Steamworks piece used is `steamcmd`, the uploader in the
Steamworks SDK (`sdk\tools\ContentBuilder\builder\steamcmd.exe`), which is not kept here.

The depot is the portable release payload, the `VRX-<version>-win-x64` folder in the
release ZIP, exactly as GitHub Actions built it. Steam installs that folder; there is no
installer on Steam.

## One-time setup in Steamworks

1. **App and depot (done 2026-10-16).** VRX3D is App ID **5458290**, a Software Application.
   Steam created its one depot, **5458291** ("VRX3D Content", all languages and
   architectures), and the store package 1883489.
2. **Launch option (done 2026-10-16).** In Installation > General: executable
   `VRX.Desktop.exe`, launch type "Launch (Default)", Windows, 64-bit only, no arguments.
   VRX3D opens its desktop window and starts SteamVR itself when you press Start VR.
3. **Steam Cloud (optional).** In Steam Cloud > Auto-Cloud, sync VRX's per-game settings with
   no code: root **WinAppDataLocal** (Windows `%LOCALAPPDATA%`), subdirectory `VRX\profiles`,
   pattern `*.json`, and root **WinAppDataLocal**, subdirectory `VRX`, pattern
   `base-settings.json`. Leave `app-settings.json` (window positions, per PC) and the
   `sessions` logs out. Profiles are keyed by the game's full path, so they match between
   PCs only when the game is installed at the same path.
4. **Source code (GPL-3.0).** The payload already includes `LICENSE`, `THIRD-PARTY-NOTICES.txt`,
   the `licenses` folder and, in `README.txt` and `BUILD.txt`, the link to the exact source
   of the build. Steam's store review rejects descriptions that link to other websites, so
   do not put the GitHub link in the description: set the store page's website to
   https://vrx3d.uk (which links to the source) and say in words that the source is on GitHub.
5. **A build account (recommended).** A separate Steam account with only the "Edit App
   Metadata" and "Publish App Changes To Steam" permissions for this app, used just for
   uploads.

## Uploading a release

Download the release ZIP from GitHub (it is checked file by file against its own
`FILES.sha256.txt` before upload), then:

```powershell
./release/steam/upload-steam.ps1 -AppId 5458290 -DepotId 5458291 -Account <build account> `
    -Release C:\path\to\VRX-1.7.8-win-x64.zip -Preview
```

`-Preview` builds and checks the depot without uploading. Run it again without `-Preview`
to upload. `steamcmd` asks for the account's password and Steam Guard code itself. The filled-in build
scripts and SteamPipe's logs go to `release\out\steam\<time>\`.

The upload never sets a build live. In Steamworks, SteamPipe > Builds, set it live on a
beta branch first, try it from Steam, then set it live on the default branch.

`steamcmd` is found from `-SteamCmd`, the `STEAMCMD` environment variable, or the newest
`steamworks_sdk_*` folder in Downloads.
