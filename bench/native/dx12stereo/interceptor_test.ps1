#requires -Version 7.0
param([Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$Proxy,[switch]$Passthrough,[switch]$Mirror,[switch]$Warp,[switch]$Unsupported,[switch]$Bindings,[switch]$Compute,[switch]$Present,[switch]$Legacy,[switch]$Bgra,[switch]$Modern,[switch]$Ten,[ValidateSet('write','protected')][string]$ExtendedGuard,[switch]$Vrs,[switch]$DepthBounds,[switch]$NullBlendProbe,[switch]$LatePipeline,[switch]$ZeroShift,[ValidateSet('hit','miss')][string]$FilterProbe,[switch]$ComputeTrace,[switch]$NonIndexed,[switch]$ReplayProbes,[switch]$GpuValidation,[switch]$DebugLayer,[switch]$RecordOnly,[switch]$CloneOnly,[ValidatePattern('^[A-Za-z0-9_,]{1,2048}$')][string]$RecordCommands,[switch]$RecordStateOnly,[switch]$SkipRecordingWork,[switch]$SkipCommandRecording)
$ErrorActionPreference='Stop'
if($ComputeTrace){$ReplayProbes=$true;$Compute=$true}
if($GpuValidation -or $ReplayProbes){$DebugLayer=$true}
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
    $start.Environment['VRX_DX12_REPLAY_PROBES']=$(if($ReplayProbes){'1'}else{'0'})
    $start.Environment['VRX_DX12_GPU_VALIDATION']=$(if($GpuValidation){'1'}else{'0'})
    $start.Environment['VRX_DX12_DEBUG_LAYER']=$(if($DebugLayer){'1'}else{'0'})
    $start.Environment['VRX_DX12_DEBUG_LOG']=$(if($DebugLayer){Join-Path $root 'native-debug.log'}else{''})
    $start.Environment['VRX_DX12_RECORD_ONLY']=$(if($RecordOnly -or $CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_CLONE_ONLY']=$(if($CloneOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_RECORD_COMMANDS']=$RecordCommands
    $start.Environment['VRX_DX12_STATE_ONLY']=$(if($RecordStateOnly){'1'}else{'0'})
    $start.Environment['VRX_DX12_SKIP_WORK']=$(if($SkipRecordingWork){'1'}else{'0'})
    $start.Environment['VRX_DX12_SKIP_COMMANDS']=$(if($SkipCommandRecording){'1'}else{'0'})
    $start.Environment['VRX_DX12_SHIFT']=$(if($ZeroShift){'0'}else{'0.4'}); $start.Environment['VRX_DX12_CONVERGENCE']='0'
    $start.Environment['VRX_DX12_TEST_MODERN']=$(if($Modern){'1'}else{'0'})
    $start.Environment['VRX_DX12_TEST_BGRA']=$(if($Bgra){'1'}else{'0'})
    $start.Environment['VRX_DX12_TEST_TEN']=$(if($Ten){'1'}else{'0'})
    $start.ArgumentList.Add($(if($NonIndexed){'--nonindexed'}elseif($LatePipeline){'--late-pipeline'}elseif($NullBlendProbe){'--null-blend'}elseif($FilterProbe){'--filter-'+$FilterProbe}elseif($DepthBounds){'--depth-bounds'}elseif($ExtendedGuard){'--extended-guard='+$ExtendedGuard}elseif($Mirror){if($Modern){'--modern'}elseif($Present){if($Legacy){'--legacy'}else{'--present'}}elseif($Compute){'--compute'}elseif($Bindings){'--bindings'}elseif($Unsupported){'--unsupported'}else{'--mirror'}}elseif($Passthrough){'--passthrough'}else{'--intercept'}))
    if($Warp) { $start.ArgumentList.Add('--warp') }
    $child=[Diagnostics.Process]::Start($start)
    $stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
    if(-not $child.WaitForExit(30000)) { $child.Kill(); $child.WaitForExit(); throw 'Native interceptor fixture timed out' }
    $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
    Write-Output $output
    if(($Vrs -or $DepthBounds) -and $child.ExitCode -eq 77 -and $output -match '^SKIP: native DX12'){exit 77}
    if($child.ExitCode -ne 0 -or $output -notmatch 'PASS: native DX12') { throw ('Native interceptor fixture failed with exit '+$child.ExitCode) }
    if($DebugLayer){$log=Get-Content -LiteralPath (Join-Path $root 'native-debug.log') -Raw;if($log -notmatch 'debug layer enabled before device creation' -or $log -notmatch 'info-queue callback ready' -or ($NullBlendProbe -and $log -notmatch 'VRX diagnostic application-message probe')){throw 'Native debug logging did not observe the requested callback'}}
    if($DebugLayer -and -not $ReplayProbes -and ($log -notmatch 'VRX diagnostic error after warning flood' -or [regex]::Matches($log,'VRX diagnostic application-message probe').Count -ne 1)){throw 'Warnings hid an error or a device callback was duplicated'}
    if($ReplayProbes -and -not $ComputeTrace -and -not $Unsupported -and $log -notmatch 'VRX replay probe 1: iaV=3/3 iaP=1/1 vs=3/3 clipIn=1/1 clipOut=1/1 ps=([1-9][0-9]*)/\1(?:\r?\n|$)'){throw 'Paired GPU counters did not prove identical native/right geometry'}
    if($ReplayProbes -and -not $ComputeTrace -and -not $Unsupported -and ($log -notmatch 'VRX replay colour 1 target=0 .*hash=([0-9a-f]+)/\1 nonzero=([1-9][0-9]*)/\2(?:\r?\n|$)' -or $log -notmatch 'VRX replay colour 2 target=0 .*hash=([0-9a-f]+)/\1 nonzero=([1-9][0-9]*)/\2(?:\r?\n|$)')){throw ('Colour readback or live probe request did not compare identical populated targets: '+$log)}
    if($ReplayProbes -and $Unsupported -and $log -notmatch 'VRX unpaired replay probe omitted after stereo stop'){throw 'Probe resources were not fenced and retired after native fallback'}
    if($ReplayProbes -and -not $ComputeTrace){$shader=[regex]::Match($log,'VRX replay shader 1 pixel=([0-9a-f]+)');if(-not $shader.Success){throw 'Sampled pixel shader identity missing'};$assembly=Get-Content -LiteralPath (Join-Path $root ('native-debug.log.shader-'+$shader.Groups[1].Value+'.ps.asm')) -Raw;if($assembly -notmatch 'ps_5_1' -or $assembly -notmatch 'dcl_output o0'){throw 'Sampled pixel shader disassembly missing'}}
    if($ReplayProbes -and -not $ComputeTrace -and ($log -notmatch 'VRX replay input 1 table=1 entry=0 .*dimension=1 size=1024x1 .*srvDimension=1' -or $log -notmatch 'VRX replay address 1 root=0 address=([0-9a-f]+)/([0-9a-f]+) heap=1')){throw 'Sampled SRV descriptor relocation or root CBV identity missing'}
    if($ComputeTrace){$shader=[regex]::Match($log,'VRX replay dispatch 1 groups=1,1,1 shader=([0-9a-f]+) listType=0');if(-not $shader.Success -or $log -notmatch 'VRX replay input 1 table=1 entry=0 .*writable=1 stage=cs'){throw ('Compute dispatch or UAV trace missing: '+$log)};$assembly=Get-Content -LiteralPath (Join-Path $root ('native-debug.log.shader-'+$shader.Groups[1].Value+'.cs.asm')) -Raw;if($assembly -notmatch 'cs_5_1' -or $assembly -notmatch 'dcl_uav_structured'){throw 'Compute shader disassembly missing'}}
    if($GpuValidation -and $log -notmatch 'VRX GPU-based validation enabled'){throw 'GPU-based validation was not enabled'}
} finally {
    if($child) { if(-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
    # Resolve and check this exact, freshly created temporary scope before removal.
    $resolved=[IO.Path]::GetFullPath($root)
    if(-not $resolved.StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^VRX-DX12-interception-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the owned fixture directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
