#requires -Version 7.0
<#
.SYNOPSIS
Launch one Vulkan application with the experimental VRX geometry layer.
.DESCRIPTION
Only the child process receives the layer settings. Nothing is registered or
copied into the game. Alternate mode exports pairs to the named VRX session.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][ValidateSet('left', 'right', 'alternate', 'mirror')][string]$Eye,
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Session = 'game',
    [ValidateRange(0, 64)][float]$Shift = 0.032,
    [ValidateSet('gpu', 'cpu')][string]$Transport = 'gpu',
    [string]$LayerDirectory = (Join-Path $PSScriptRoot 'out/Release'),
    [string[]]$GameArguments = @(),
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
if ($Eye -eq 'mirror' -and $Transport -ne 'gpu') { throw 'Same-frame mirroring requires GPU texture sharing.' }
if ($Eye -in @('alternate', 'mirror') -and $Shift -le 0) { throw 'Alternate mode requires a positive shift; turn off stereo in VRX for mono viewing.' }
$application = (Resolve-Path -LiteralPath $Executable).Path
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) { throw 'Executable must be a file.' }
$layerRoot = (Resolve-Path -LiteralPath $LayerDirectory).Path
foreach ($name in @('VkLayer_vrx_xgstereo.json', 'vrx_xgstereo.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $layerRoot $name) -PathType Leaf)) {
        throw "Build the xgstereo prototype first; missing $name in $layerRoot"
    }
}
$start = [System.Diagnostics.ProcessStartInfo]::new($application)
$start.UseShellExecute = $false
$start.WorkingDirectory = [System.IO.Path]::GetDirectoryName($application)
foreach ($argument in $GameArguments) { $start.ArgumentList.Add($argument) }
$extraPaths = $start.Environment['VK_ADD_LAYER_PATH']
$start.Environment['VK_ADD_LAYER_PATH'] = if ($extraPaths) { "$layerRoot;$extraPaths" } else { $layerRoot }
$layers = @($start.Environment['VK_INSTANCE_LAYERS'] -split ';' | Where-Object { $_ })
if ($layers -notcontains 'VK_LAYER_VRX_xgstereo') { $layers += 'VK_LAYER_VRX_xgstereo' }
$start.Environment['VK_INSTANCE_LAYERS'] = $layers -join ';'
$start.Environment['VRX_XG_EYE'] = $Eye
$start.Environment['VRX_XG_SHIFT'] = $Shift.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
if ($Eye -in @('alternate', 'mirror')) { $start.Environment['VRX_XG_SESSION'] = $Session; $start.Environment['VRX_XG_TRANSPORT'] = $Transport }
else { $null = $start.Environment.Remove('VRX_XG_SESSION'); $null = $start.Environment.Remove('VRX_XG_TRANSPORT') }
if ($Preview) {
    # Show only settings introduced here, never the inherited environment.
    [pscustomobject]@{
        Executable = $start.FileName
        Arguments = @($start.ArgumentList)
        LayerDirectory = $layerRoot
        Eye = $Eye
        Shift = $start.Environment['VRX_XG_SHIFT']
        Session = if ($Eye -in @('alternate', 'mirror')) { $Session } else { $null }
    }
    return
}
$child = [System.Diagnostics.Process]::new()
$child.StartInfo = $start
try {
    if (-not $child.Start()) { throw 'Application did not start.' }
    $child.WaitForExit()
    $result = $child.ExitCode
}
finally { $child.Dispose() }
exit $result
