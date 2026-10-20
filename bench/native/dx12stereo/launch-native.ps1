#requires -Version 7.0
<#
.SYNOPSIS
Launch an x64 native DX12 app with the opt-in local bridge and owned cleanup.
.DESCRIPTION
Keeps the launcher alive until the app exits. Existing game DLLs are preserved.
ObserveOnly collects native API counts without creating right-eye resources.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Session='native-game',
    [string]$Proxy=(Join-Path $PSScriptRoot 'out/proxy/Release/d3d12.dll'),
    [ValidatePattern('^[0-9]+$')][string]$SteamAppId,
    [string[]]$GameArguments=@('-dx12','-windowed','-ResX=1280','-ResY=720'),
    [ValidateRange(0,64)][float]$Shift=0.032,
    [ValidateRange(0,0.25)][float]$Convergence=0,
    [switch]$ObserveOnly,
    [switch]$RecordOnly,
    [switch]$CloneOnly,
    [switch]$SkipCommandRecording,
    [switch]$Preview
)
$ErrorActionPreference='Stop'
if([int]$ObserveOnly.IsPresent+[int]$RecordOnly.IsPresent+[int]$CloneOnly.IsPresent -gt 1){throw 'Choose only one native diagnostic mode'}
if($SkipCommandRecording -and -not $RecordOnly){throw 'Skipping shadow command recording requires -RecordOnly'}
$application=(Resolve-Path -LiteralPath $Executable).Path
$bridge=(Resolve-Path -LiteralPath $Proxy).Path
$gameRoot=[IO.Path]::GetDirectoryName($application)
$target=Join-Path $gameRoot 'd3d12.dll'
function RequireX64([string]$Path) {
    $file=[IO.File]::OpenRead($Path)
    try {
        $reader=[IO.BinaryReader]::new($file)
        if($reader.ReadUInt16() -ne 0x5a4d){throw "Not a PE image: $Path"}
        $file.Position=0x3c;$pe=$reader.ReadUInt32()
        if($pe -gt $file.Length-6){throw "Invalid PE header: $Path"}
        $file.Position=$pe
        if($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664){throw "Native DX12 requires an x64 image: $Path"}
    } finally {$file.Dispose()}
}
RequireX64 $application;RequireX64 $bridge
$key='game-'+[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($application.ToUpperInvariant()))).Substring(0,12).ToLowerInvariant()
$gate=[Threading.Mutex]::new($false,'Local\VRX.xgstereo.files.'+$key)
$held=$false;$child=$null;$installed=$false;$temporary=$null;$temporaryOwned=$false;$runRoot=$null;$started=$false
try {
    try {$held=$gate.WaitOne(5000)}catch [Threading.AbandonedMutexException]{$held=$true}
    if(-not $held){throw 'Another geometry launcher is changing files for this game'}
    foreach($name in @('d3d12.dll','d3d11.dll','dxgi.dll')) {
        if(Test-Path -LiteralPath (Join-Path $gameRoot $name)){throw "Existing game file prevents a scoped native launch: $name"}
    }
    foreach($running in @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($application)) -ErrorAction SilentlyContinue)) {
        if([string]::Equals($running.Path,$application,[StringComparison]::OrdinalIgnoreCase)){throw 'The selected game is already running'}
    }
    if($Preview){[pscustomobject]@{Executable=$application;Proxy=$bridge;Session=$Session;Arguments=$GameArguments;ObserveOnly=$ObserveOnly.IsPresent;RecordOnly=$RecordOnly.IsPresent;CloneOnly=$CloneOnly.IsPresent;SkipCommandRecording=$SkipCommandRecording.IsPresent};return}
    $runRoot=Join-Path $PSScriptRoot ('out/native-'+[Guid]::NewGuid().ToString('N'));$null=New-Item -ItemType Directory -Path $runRoot
    $runRecord=[pscustomobject]@{Pid=0;StartTicks=0L;Executable=$application;Session=$Session;Backend='dx12';ObserveOnly=$ObserveOnly.IsPresent;RecordOnly=$RecordOnly.IsPresent;CloneOnly=$CloneOnly.IsPresent;SkipCommandRecording=$SkipCommandRecording.IsPresent;Arguments=$GameArguments}
    $runRecord | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'run.json')
    $hash=(Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash
    $record=[pscustomobject]@{Path=$target;SHA256=$hash}
    $record | ConvertTo-Json -AsArray | Set-Content -LiteralPath (Join-Path $runRoot 'installed.json')
    # Stage the complete file before its atomic, non-overwriting installation.
    $temporary=Join-Path $gameRoot ('vrx-d3d12-'+[Guid]::NewGuid().ToString('N')+'.tmp')
    $inputFile=[IO.File]::OpenRead($bridge)
    try {
        $outputFile=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $temporaryOwned=$true
        try {$inputFile.CopyTo($outputFile);$outputFile.Flush($true)}finally{$outputFile.Dispose()}
    }finally{$inputFile.Dispose()}
    if((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $hash){throw 'Staged bridge hash differs from its source'}
    foreach($path in @($temporary,$target)){if(-not [string]::Equals([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)),$gameRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Native bridge installation escaped the selected game directory'}}
    [IO.File]::Move($temporary,$target,$false);$temporary=$null;$temporaryOwned=$false;$installed=$true
    $start=[Diagnostics.ProcessStartInfo]::new($application);$start.UseShellExecute=$false;$start.WorkingDirectory=$gameRoot
    foreach($argument in $GameArguments){$start.ArgumentList.Add($argument)}
    $start.Environment['VRX_DX12_INTERCEPT']='1';$start.Environment['VRX_DX12_MIRROR']=$(if($ObserveOnly){'0'}else{'1'})
    $start.Environment['VRX_DX12_RECORD_ONLY']=$(if($RecordOnly -or $CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_CLONE_ONLY']=$(if($CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_SKIP_COMMANDS']=$(if($SkipCommandRecording){'1'}else{'0'})
    $start.Environment['VRX_XG_SESSION']=$Session
    $start.Environment['VRX_DX12_SHIFT']=$Shift.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    $start.Environment['VRX_DX12_CONVERGENCE']=$Convergence.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    # Test-only fixture options must never leak into a selected ordinary game.
    $start.Environment.Remove('VRX_DX12_TEST_BGRA') > $null;$start.Environment.Remove('VRX_DX12_TEST_MODERN') > $null
    $start.Environment.Remove('VRX_DX12_TEST_TEN') > $null
    if($SteamAppId){$start.Environment['SteamAppId']=$SteamAppId;$start.Environment['SteamGameId']=$SteamAppId}
    $child=[Diagnostics.Process]::Start($start);if(-not $child){throw 'Native DX12 app did not start'};$started=$true
    $runRecord.Pid=$child.Id;$runRecord.StartTicks=$child.StartTime.ToUniversalTime().Ticks
    $runRecord | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'run.json')
    Write-Output "Native DX12 PID $($child.Id); session $Session; run $runRoot"
    $gate.ReleaseMutex();$held=$false
    $child.WaitForExit();$result=$child.ExitCode
    $result | Set-Content -LiteralPath (Join-Path $runRoot 'exit-code.txt')
}finally{
    if($temporaryOwned -and $temporary -and (Test-Path -LiteralPath $temporary)){Remove-Item -LiteralPath $temporary}
    if($installed -and (-not $started -or $child.HasExited)) {
        if(-not $held){try {$held=$gate.WaitOne(5000)}catch [Threading.AbandonedMutexException]{$held=$true}}
        # A later launch can already have this same bridge loaded. File ownership
        # alone is insufficient to remove it while another instance is alive.
        $otherRunning=$false
        if($held){foreach($process in @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($application)) -ErrorAction SilentlyContinue)) {
            try {if([string]::Equals($process.Path,$application,[StringComparison]::OrdinalIgnoreCase)){$otherRunning=$true}}
            finally {$process.Dispose()}
        }}
        if($held -and -not $otherRunning -and (Test-Path -LiteralPath $target)) {
            if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $hash){Remove-Item -LiteralPath $target}
            else {Write-Warning "Leaving changed file in place: $target"}
        }
        if($held -and -not $otherRunning -and -not (Test-Path -LiteralPath $target)){'[]' | Set-Content -LiteralPath (Join-Path $runRoot 'installed.json')}
    }elseif($installed){Write-Warning "App is still running; installed.json records the owned bridge: $runRoot"}
    if($child){$child.Dispose()};if($held){$gate.ReleaseMutex()};$gate.Dispose()
}
if($null -ne $result){exit $result}
