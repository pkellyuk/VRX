#requires -Version 7.0
<#
.SYNOPSIS
Run a DirectX 11 game through DXVK and VRX's experimental geometry layer.
.DESCRIPTION
Supply the x64 directory from an official DXVK release. Temporarily installs
d3d11.dll and dxgi.dll beside the executable, refusing to replace existing files.
Keeps the launcher running and removes its unchanged DLLs when the game exits.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$DxvkDirectory,
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Session = 'game',
    [ValidateRange(0.000001, 64)][float]$Shift = 0.032,
    [ValidateRange(0, 0.25)][float]$Convergence = 0,
    [ValidateSet('gpu', 'cpu')][string]$Transport = 'gpu',
    [ValidateSet('alternate', 'mirror')][string]$Stereo = 'alternate',
    [ValidatePattern('^[0-9]+$')][string]$SteamAppId,
    [string]$LayerDirectory = (Join-Path $PSScriptRoot 'out/Release'),
    [string[]]$GameArguments = @('-dx11', '-windowed', '-ResX=2560', '-ResY=1440'),
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
if ($Stereo -eq 'mirror' -and $Transport -ne 'gpu') { throw 'Same-frame mirroring requires GPU texture sharing.' }
$application = (Resolve-Path -LiteralPath $Executable).Path
$gameRoot = [IO.Path]::GetDirectoryName($application)
$layerRoot = (Resolve-Path -LiteralPath $LayerDirectory).Path
$dxvkRoot = (Resolve-Path -LiteralPath $DxvkDirectory).Path
foreach ($file in @('VkLayer_vrx_xgstereo.json', 'vrx_xgstereo.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $layerRoot $file) -PathType Leaf)) { throw "Missing layer file: $file" }
}
$installed = @()
foreach ($dll in @('d3d11.dll', 'dxgi.dll')) {
    $source = Join-Path $dxvkRoot $dll
    $target = Join-Path $gameRoot $dll
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing DXVK x64 DLL: $source" }
    if (Test-Path -LiteralPath $target) { throw "Refusing to replace an existing game file: $target" }
}
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($application)) -ErrorAction SilentlyContinue) {
    throw 'Close the game before launching geometry stereo.'
}
if ($Preview) {
    [pscustomobject]@{ Executable=$application; DxvkDirectory=$dxvkRoot; Session=$Session; Shift=$Shift; Transport=$Transport; Stereo=$Stereo; Arguments=$GameArguments }
    return
}
$runRoot = Join-Path $PSScriptRoot ('out/dx11-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runRoot
$configPath = Join-Path $runRoot 'dxvk.conf'
$tuningPath = Join-Path $runRoot 'stereo.txt'
$invariant = [Globalization.CultureInfo]::InvariantCulture
($Shift.ToString('R', $invariant) + ' ' + $Convergence.ToString('R', $invariant)) | Set-Content -LiteralPath $tuningPath
# Inline shader modules remain supported; disabling pipeline libraries limits
# background compilation/cache paths while validating this experimental bridge.
@('dxvk.enableGraphicsPipelineLibrary = False', 'dxvk.enableDescriptorBuffer = False', 'dxvk.disableNvLowLatency2 = True') | Set-Content -LiteralPath $configPath
$result = 1
try {
    foreach ($dll in @('d3d11.dll', 'dxgi.dll')) {
        $source = Join-Path $dxvkRoot $dll
        $target = Join-Path $gameRoot $dll
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $inputFile = [IO.File]::OpenRead($source)
        try {
            # CreateNew prevents a concurrent installer from being overwritten.
            $outputFile = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            $record = [pscustomobject]@{ Path=$target; SHA256=$hash }
            $installed += $record
            $installed | ConvertTo-Json -AsArray | Set-Content -LiteralPath (Join-Path $runRoot 'installed.json')
            try { $inputFile.CopyTo($outputFile) } finally { $outputFile.Dispose() }
        } finally { $inputFile.Dispose() }
    }
    $start = [Diagnostics.ProcessStartInfo]::new($application)
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $gameRoot
    foreach ($argument in $GameArguments) { $start.ArgumentList.Add($argument) }
    $extraPaths = $start.Environment['VK_ADD_LAYER_PATH']
    $start.Environment['VK_ADD_LAYER_PATH'] = if ($extraPaths) { "$layerRoot;$extraPaths" } else { $layerRoot }
    $layers = @($start.Environment['VK_INSTANCE_LAYERS'] -split ';' | Where-Object { $_ })
    if ($layers -notcontains 'VK_LAYER_VRX_xgstereo') { $layers += 'VK_LAYER_VRX_xgstereo' }
    $start.Environment['VK_INSTANCE_LAYERS'] = $layers -join ';'
    $start.Environment['VRX_XG_EYE'] = $Stereo
    $start.Environment['VRX_XG_TRANSPORT'] = $Transport
    $start.Environment['VRX_XG_SESSION'] = $Session
    $start.Environment['VRX_XG_SHIFT'] = $Shift.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    $start.Environment['VRX_XG_LOG'] = Join-Path $runRoot 'layer.log'
    $start.Environment['VRX_XG_TUNING'] = $tuningPath
    $start.Environment['DXVK_LOG_PATH'] = $runRoot
    $start.Environment['DXVK_CONFIG_FILE'] = $configPath
    $start.Environment['DXVK_STATE_CACHE_PATH'] = $runRoot
    if ($SteamAppId) {
        $start.Environment['SteamAppId'] = $SteamAppId
        $start.Environment['SteamGameId'] = $SteamAppId
    }
    $child = [Diagnostics.Process]::Start($start)
    if (-not $child) { throw 'Game did not start.' }
    Write-Output "Geometry game PID $($child.Id); session $Session; logs $runRoot"
    $child.Id | Set-Content -LiteralPath (Join-Path $runRoot 'game.pid')
    [pscustomobject]@{ Pid=$child.Id; StartTicks=$child.StartTime.ToUniversalTime().Ticks; Session=$Session; Executable=$application; Transport=$Transport; Stereo=$Stereo } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'run.json')
    try { $child.WaitForExit(); $result = $child.ExitCode } finally { $child.Dispose() }
}
finally {
    foreach ($record in $installed) {
        if (Test-Path -LiteralPath $record.Path) {
            if ((Get-FileHash -LiteralPath $record.Path -Algorithm SHA256).Hash -eq $record.SHA256) {
                Remove-Item -LiteralPath $record.Path
            } else { Write-Warning "Leaving a changed file in place: $($record.Path)" }
        }
    }
}
exit $result
