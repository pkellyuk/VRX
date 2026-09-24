# Uploads a VRX release to Steam with SteamPipe (steamcmd from the Steamworks SDK's
# ContentBuilder). It takes the release ZIP (or an extracted payload folder), checks it,
# fills in the build scripts beside this file, and runs steamcmd. steamcmd asks for your
# password and Steam Guard code itself; nothing here stores them.
#
#   ./release/steam/upload-steam.ps1 -AppId 1234560 -DepotId 1234561 -Account mysteambuilder `
#       -Release release/out/v1.7.8/VRX-1.7.8-win-x64.zip [-Preview]
#
# -Preview builds and checks the depot without uploading. After a real upload, set the
# build live on a branch in Steamworks (SteamPipe > Builds); this script never does.
param(
    [Parameter(Mandatory)] [ValidateRange(1, [uint32]::MaxValue)] [uint32]$AppId,
    [Parameter(Mandatory)] [ValidateRange(1, [uint32]::MaxValue)] [uint32]$DepotId,
    [Parameter(Mandatory)] [string]$Account,
    [Parameter(Mandatory)] [string]$Release,
    [string]$SteamCmd = '',
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
Write-Output "upload-steam: enter, app $AppId, depot $DepotId, account $Account, release $Release, preview $([bool]$Preview)"
if ([string]::IsNullOrWhiteSpace($Account)) { throw 'An account name is required' }
if (!(Test-Path -LiteralPath $Release)) { throw "No release at $Release" }
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path $projectRoot ("release\out\steam\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

# The payload: the release ZIP's single top folder (VRX-<version>-win-x64), or a folder.
$payload = (Resolve-Path -LiteralPath $Release).Path
if ((Get-Item -LiteralPath $payload) -is [IO.FileInfo])
{
    $extracted = Join-Path $work 'payload'
    Expand-Archive -LiteralPath $payload -DestinationPath $extracted
    $top = @(Get-ChildItem -LiteralPath $extracted -Directory)
    if ($top.Count -ne 1) { throw "Expected one folder in $Release, found $($top.Count)" }
    $payload = $top[0].FullName
}
foreach ($required in 'VRX.Desktop.exe', 'engine\xrplayer.exe', 'BUILD.txt', 'LICENSE', 'README.txt', 'THIRD-PARTY-NOTICES.txt', 'FILES.sha256.txt')
{
    if (!(Test-Path -LiteralPath (Join-Path $payload $required))) { throw "The payload has no $required - is $Release a VRX release?" }
}
# Every file must match the release's own list (FILES.sha256.txt), so exactly the tested
# build is uploaded.
$listed = 0
foreach ($line in Get-Content -LiteralPath (Join-Path $payload 'FILES.sha256.txt'))
{
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { continue }
    $file = Join-Path $payload $Matches[2]
    if (!(Test-Path -LiteralPath $file)) { throw "Missing from the payload: $($Matches[2])" }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Matches[1]) { throw "Changed since the build: $($Matches[2])" }
    $listed++
}
$build = Get-Content -LiteralPath (Join-Path $payload 'BUILD.txt')
$version = ($build | Select-Object -First 1) -replace '^VRX\s+', ''
$commit = (($build | Where-Object { $_ -like 'Source commit:*' }) -replace '^Source commit:\s*', '').Trim()
Write-Output "upload-steam: payload $payload - VRX $version, commit $commit, $listed files checked"

# steamcmd: -SteamCmd, $env:STEAMCMD, or the Steamworks SDK in Downloads.
if (!$SteamCmd) { $SteamCmd = $env:STEAMCMD }
if (!$SteamCmd)
{
    $sdk = Get-ChildItem -Path (Join-Path $env:USERPROFILE 'Downloads') -Directory -Filter 'steamworks_sdk_*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($sdk) { $SteamCmd = Join-Path $sdk.FullName 'sdk\tools\ContentBuilder\builder\steamcmd.exe' }
}
if (!$SteamCmd -or !(Test-Path -LiteralPath $SteamCmd)) { throw 'steamcmd.exe not found: pass -SteamCmd or set STEAMCMD (Steamworks SDK: sdk\tools\ContentBuilder\builder)' }

# The build scripts, filled in. Values with quotes or backslashes are written as VDF needs.
function Vdf([string]$value) { return $value.Replace('\', '\\').Replace('"', '\"') }
$output = New-Item -ItemType Directory -Path (Join-Path $work 'output') -Force
$app = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'app_build.vdf')).
    Replace('<APPID>', "$AppId").Replace('<DEPOTID>', "$DepotId").
    Replace('<DESCRIPTION>', (Vdf "VRX $version ($($commit.Substring(0, [Math]::Min(12, $commit.Length))))")).
    Replace('<PREVIEW>', $(if ($Preview) { '1' } else { '0' })).
    Replace('<CONTENTROOT>', (Vdf $payload)).Replace('<BUILDOUTPUT>', (Vdf $output.FullName))
$depot = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'depot_build.vdf')).Replace('<DEPOTID>', "$DepotId")
$appFile = Join-Path $work 'app_build.vdf'
Set-Content -LiteralPath $appFile -Value $app -Encoding utf8
Set-Content -LiteralPath (Join-Path $work 'depot_build.vdf') -Value $depot -Encoding utf8
Write-Output "upload-steam: build scripts in $work"

Write-Output "upload-steam: running steamcmd as $Account (it asks for the password and Steam Guard code)"
& $SteamCmd +login $Account +run_app_build $appFile +quit
if ($LASTEXITCODE) { throw "steamcmd failed with exit code $LASTEXITCODE - see $($output.FullName)" }
Write-Output "upload-steam: exit ok$(if ($Preview) { ' (preview: nothing was uploaded)' } else { ' - now set the build live on a branch in Steamworks' })"
