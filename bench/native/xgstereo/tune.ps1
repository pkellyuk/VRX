#requires -Version 7.0
<#
.SYNOPSIS
Adjust a running experimental DX11 geometry session without restarting the game.
.DESCRIPTION
Convergence is the per-eye normalized screen offset. With ordinary perspective
projection, the zero-parallax clip W is Shift / Convergence. Use -Mono to set
both values to zero. Values apply together at a new left/right pair boundary.
#>
[CmdletBinding(DefaultParameterSetName='Stereo')]
param(
    [Parameter(Mandatory, ParameterSetName='Stereo')][ValidateRange(0.000001, 64)][float]$Shift,
    [Parameter(ParameterSetName='Stereo')][ValidateRange(0, 0.25)][float]$Convergence = 0,
    [Parameter(Mandatory, ParameterSetName='Mono')][switch]$Mono,
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Session = 'game',
    [string]$RunDirectory
)
$ErrorActionPreference = 'Stop'
$candidates = if ($RunDirectory) { @(Get-Item -LiteralPath $RunDirectory) } else {
    @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'out') -Directory -Filter 'dx11-*')
}
$live = @($candidates | Where-Object {
    $metadata = Join-Path $_.FullName 'run.json'
    if (-not (Test-Path -LiteralPath $metadata -PathType Leaf)) { return $false }
    $run = Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if ($run.Session -ne $Session) { return $false }
    $game = Get-Process -Id $run.Pid -ErrorAction SilentlyContinue
    return $game -and $game.StartTime.ToUniversalTime().Ticks -eq $run.StartTicks -and
        $game.Path -eq $run.Executable -and (Test-Path -LiteralPath (Join-Path $_.FullName 'stereo.txt'))
})
if ($live.Count -ne 1) { throw "Expected one live DX11 session '$Session'; found $($live.Count). Specify -RunDirectory when necessary." }
$metadata = Get-Content -LiteralPath (Join-Path $live[0].FullName 'run.json') -Raw | ConvertFrom-Json
if ($metadata.Stereo -eq 'mirror') { throw 'The mirror backend currently reads tuning at launch. Restart it with -Shift/-Convergence, or disable stereo in VRX for identical eyes.' }
$path = Join-Path $live[0].FullName 'stereo.txt'
if ($Mono) { $Shift = 0; $Convergence = 0 }
$invariant = [Globalization.CultureInfo]::InvariantCulture
$text = $Shift.ToString('R', $invariant) + ' ' + $Convergence.ToString('R', $invariant) + "`n"
$temporary = $path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
try {
    [IO.File]::WriteAllText($temporary, $text)
    [IO.File]::Move($temporary, $path, $true)
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
Write-Output "Session ${Session}: separation $Shift, convergence $Convergence; applies within 200 ms while the game is rendering."
