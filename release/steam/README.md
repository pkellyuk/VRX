# VRX3D on Steam

Uploading a VRX release to Steam with SteamPipe. VRX does not link the Steamworks API
(`steam_api64.dll`): Steam only installs and launches it, so the GPL-3.0 licence and code
signing are unaffected. The only Steamworks piece used is `steamcmd`, the uploader in the
Steamworks SDK (`sdk\tools\ContentBuilder\builder\steamcmd.exe`), which is not kept here.

The depot is the portable release payload, the `VRX-<version>-win-x64` folder in the
release ZIP, exactly as GitHub Actions built it. Steam installs that folder; there is no
installer on Steam.

## One-time setup in Steamworks

1. **App and depot.** In the app's SteamPipe > Depots, create one depot for Windows 64-bit,
   all languages. Note the App ID and Depot ID.
2. **Launch option.** In Installation > General: executable `VRX.Desktop.exe`, operating
   system Windows, 64-bit. It needs no arguments.
3. **Steam Cloud (optional).** In Steam Cloud > Auto-Cloud, sync VRX's per-game settings with
   no code: root **WinAppDataLocal** (Windows `%LOCALAPPDATA%`), subdirectory `VRX\profiles`,
   pattern `*.json`, and root **WinAppDataLocal**, subdirectory `VRX`, pattern
   `base-settings.json`. Leave `app-settings.json` (window positions, per PC) and the
   `sessions` logs out. Profiles are keyed by the game's full path, so they match between
   PCs only when the game is installed at the same path.
4. **Source code (GPL-3.0).** The payload already includes `LICENSE`, `THIRD-PARTY-NOTICES.txt`,
   the `licenses` folder and, in `README.txt` and `BUILD.txt`, the link to the exact source
   of the build. Also link https://github.com/pkellyuk/VRX from the store page.
5. **A build account (recommended).** A separate Steam account with only the "Edit App
   Metadata" and "Publish App Changes To Steam" permissions for this app, used just for
   uploads.

## Uploading a release

Download the release ZIP from GitHub (it is checked file by file against its own
`FILES.sha256.txt` before upload), then:

```powershell
./release/steam/upload-steam.ps1 -AppId <app id> -DepotId <depot id> -Account <build account> `
    -Release C:\path\to\VRX-1.7.8-win-x64.zip -Preview
```

`-Preview` builds and checks the depot without uploading. Run it again without `-Preview`
to upload. `steamcmd` asks for the account's password and Steam Guard code itself. The filled-in build
scripts and SteamPipe's logs go to `release\out\steam\<time>\`.

The upload never sets a build live. In Steamworks, SteamPipe > Builds, set it live on a
beta branch first, try it from Steam, then set it live on the default branch.

`steamcmd` is found from `-SteamCmd`, the `STEAMCMD` environment variable, or the newest
`steamworks_sdk_*` folder in Downloads.
