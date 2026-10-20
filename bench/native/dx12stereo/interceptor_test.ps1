#requires -Version 7.0
param([Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$Proxy,[switch]$Passthrough,[switch]$Mirror,[switch]$Warp,[switch]$Unsupported,[switch]$Bindings,[switch]$Compute,[switch]$Present,[switch]$Legacy,[switch]$Bgra,[switch]$Modern,[switch]$Ten,[ValidateSet('write','protected')][string]$ExtendedGuard,[switch]$Vrs,[switch]$DepthBounds,[ValidateSet('hit','miss')][string]$FilterProbe,[switch]$RecordOnly,[switch]$CloneOnly,[ValidatePattern('^[A-Za-z0-9_,]{1,2048}$')][string]$RecordCommands,[switch]$RecordStateOnly,[switch]$SkipRecordingWork,[switch]$SkipCommandRecording)
$ErrorActionPreference='Stop'
$temporary=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root=Join-Path $temporary ('VRX-DX12-interception-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$child=$null
try {
    $application=Join-Path $root 'native-fixture.exe'
    Copy-Item -LiteralPath $Fixture -Destination $application
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $root 'd3d12.dll')
    $start=[Diagnostics.ProcessStartInfo]::new($application)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Environment['VRX_DX12_INTERCEPT']=$(if($Passthrough){'0'}else{'1'})
    $start.Environment['VRX_DX12_MIRROR']=$(if($Mirror){'1'}else{'0'})
    $start.Environment['VRX_DX12_RECORD_ONLY']=$(if($RecordOnly -or $CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_CLONE_ONLY']=$(if($CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_RECORD_COMMANDS']=$RecordCommands
    $start.Environment['VRX_DX12_STATE_ONLY']=$(if($RecordStateOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_SKIP_WORK']=$(if($SkipRecordingWork){'1'}else{'0'})
    $start.Environment['VRX_DX12_SKIP_COMMANDS']=$(if($SkipCommandRecording){'1'}else{'0'})
    $start.Environment['VRX_DX12_SHIFT']='0.4'; $start.Environment['VRX_DX12_CONVERGENCE']='0'
    $start.Environment['VRX_DX12_TEST_MODERN']=$(if($Modern){'1'}else{'0'})
    $start.Environment['VRX_DX12_TEST_BGRA']=$(if($Bgra){'1'}else{'0'})
    $start.Environment['VRX_DX12_TEST_TEN']=$(if($Ten){'1'}else{'0'})
    $start.ArgumentList.Add($(if($FilterProbe){'--filter-'+$FilterProbe}elseif($DepthBounds){'--depth-bounds'}elseif($ExtendedGuard){'--extended-guard='+$ExtendedGuard}elseif($Mirror){if($Modern){'--modern'}elseif($Present){if($Legacy){'--legacy'}else{'--present'}}elseif($Compute){'--compute'}elseif($Bindings){'--bindings'}elseif($Unsupported){'--unsupported'}else{'--mirror'}}elseif($Passthrough){'--passthrough'}else{'--intercept'}))
    if($Warp) { $start.ArgumentList.Add('--warp') }
    $child=[Diagnostics.Process]::Start($start)
    $stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
    if(-not $child.WaitForExit(30000)) { $child.Kill(); $child.WaitForExit(); throw 'Native interceptor fixture timed out' }
    $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
    Write-Output $output
    if(($Vrs -or $DepthBounds) -and $child.ExitCode -eq 77 -and $output -match '^SKIP: native DX12'){exit 77}
    if($child.ExitCode -ne 0 -or $output -notmatch 'PASS: native DX12') { throw ('Native interceptor fixture failed with exit '+$child.ExitCode) }
} finally {
    if($child) { if(-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
    # Resolve and check this exact, freshly created temporary scope before removal.
    $resolved=[IO.Path]::GetFullPath($root)
    if(-not $resolved.StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^VRX-DX12-interception-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the owned fixture directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
