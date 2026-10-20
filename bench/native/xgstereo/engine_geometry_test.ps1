#requires -Version 7.0
param([Parameter(Mandatory)][string]$Engine, [Parameter(Mandatory)][string]$Producer, [switch]$Gpu, [switch]$SameFrame, [switch]$Mirror, [string]$Proxy, [switch]$Resize, [switch]$Mono)
$ErrorActionPreference = 'Stop'
if ($Mirror) { $Gpu = $true; $SameFrame = $true; }
if ($Proxy) {if($Mirror){throw 'Native DX12 proxy and Vulkan mirror modes are mutually exclusive'};$Gpu=$true;$SameFrame=$true}
if($Resize -and -not $Proxy){throw 'Resize handshake requires the native DX12 presentation fixture'}
if($Mono -and -not $Resize){throw 'Explicit mono mode is for the resize handshake; ordinary checks already test stereo and mono'}
$session = 'check-' + [Guid]::NewGuid().ToString('N')
$producerPath = (Resolve-Path -LiteralPath $Producer).Path
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$start = [Diagnostics.ProcessStartInfo]::new($producerPath)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Environment['VK_LAYER_PATH'] = [IO.Path]::GetDirectoryName($producerPath)
$start.ArgumentList.Add($(if($Resize){'--serve-resize-gpu='}elseif ($Mirror) { '--serve-mirror-gpu=' } elseif ($Gpu) { '--serve-gpu=' } else { '--serve=' }) + $session)
$child = [Diagnostics.Process]::new()
$child.StartInfo = $start
$fixtureRoot=$null
$started=$false
try {
    if($Proxy) {
        $fixtureRoot=Join-Path ([IO.Path]::GetTempPath()) ('VRX-DX12-engine-'+[Guid]::NewGuid().ToString('N'))
        $null=New-Item -ItemType Directory -Path $fixtureRoot
        $fixture=Join-Path $fixtureRoot 'native-present.exe'
        Copy-Item -LiteralPath $producerPath -Destination $fixture
        Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Proxy).Path -Destination (Join-Path $fixtureRoot 'd3d12.dll')
        $child.StartInfo.FileName=$fixture
        $child.StartInfo.Environment['VRX_DX12_INTERCEPT']='1';$child.StartInfo.Environment['VRX_DX12_MIRROR']='1'
        $child.StartInfo.Environment['VRX_DX12_SHIFT']='0.4';$child.StartInfo.Environment['VRX_DX12_CONVERGENCE']='0'
    }
    if (-not $child.Start()) { throw 'Geometry test producer did not start.' }
    $started=$true
    $producerOut = $child.StandardOutput.ReadToEndAsync()
    $producerErr = $child.StandardError.ReadToEndAsync()
    # Wait for the named mapping, not an arbitrary shader-startup delay.
    $deadline = [Environment]::TickCount64 + 10000
    $ready = $false
    do {
        try {
            $map = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\VRX.xgstereo.' + $session + $(if ($Gpu) { '.gpu.frames' } else { '.frames' }))
            try {
                $view=$map.CreateViewAccessor(0,$(if($Gpu){176}else{72}),[IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
                try {
                    $ready=$view.ReadUInt32(0) -eq $(if($Gpu){0x47534758}else{0x50534758}) -and
                        $view.ReadUInt32(4) -eq 1 -and $view.ReadUInt32($(if($Gpu){12}else{28})) -eq 1 -and
                        $view.ReadUInt32($(if($Gpu){16}else{64})) -eq $child.Id
                } finally {$view.Dispose()}
            } finally {$map.Dispose()}
        } catch [IO.FileNotFoundException] { }
        if ($child.HasExited) { throw ('Producer failed: ' + $producerErr.GetAwaiter().GetResult()) }
        if(-not $ready){Start-Sleep -Milliseconds 10}
    } while (-not $ready -and [Environment]::TickCount64 -lt $deadline)
    if (-not $ready) { throw 'Producer mapping unavailable.' }
    foreach ($mode in $(if($Resize){@('resize','wrong-owner')}else{@('stereo','mono','wrong-owner')})) {
        $check = [Diagnostics.ProcessStartInfo]::new($enginePath)
        $check.UseShellExecute = $false
        $check.CreateNoWindow = $true
        $check.RedirectStandardOutput = $true
        $check.RedirectStandardError = $true
        foreach ($argument in @(('--geometry-session=' + $session), '--check-geometry', '--debug', '--normal-gpu-priority')) {
            $check.ArgumentList.Add($argument)
        }
        $check.ArgumentList.Add('--geometry-pid=' + $(if ($mode -eq 'wrong-owner') { '4294967295' } else { $child.Id }))
        if ($mode -eq 'mono' -or ($mode -eq 'resize' -and $Mono)) { $check.ArgumentList.Add('--no-warp') }
        if($mode -eq 'resize'){$check.ArgumentList.Add('--check-geometry-resizes=2')}
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $check
        try {
            if (-not $process.Start()) { throw 'Engine check did not start.' }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit($(if($Resize){30000}else{15000}))) { $process.Kill(); $process.WaitForExit(); throw 'Engine check timed out.' }
            $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
            Write-Output $output
            if ($mode -eq 'wrong-owner') {
                if ($process.ExitCode -eq 0 -or $output -notmatch 'geometry session belongs to another process') {
                    throw 'Engine must reject a geometry stream owned by another game process.'
                }
            }
            elseif ($process.ExitCode -ne 0 -or $output -notmatch 'Geometry check: PASS' -or
                $output -match 'InitModel:|WorkerMain:|InitXrInstance:|d3d12\[[01]\]|GPU REMOVED') {
                throw 'Geometry source must preserve the pair on the GPU, without starting AI depth or OpenXR.'
            }
            elseif ($Gpu -and ($output -notmatch 'GPU texture sharing' -or $output -notmatch 'no CPU pixel upload' -or
                ($SameFrame -and $output -notmatch 'same-frame eyes'))) {
                throw 'GPU handoff must explicitly select shared textures and the declared same-frame rendering mode.'
            }
            elseif($mode -eq 'resize' -and $output -notmatch 'Geometry resize check: PASS - 2 automatic reconnects') {throw 'One VRX process must consume and verify both replacement generations'}
        }
        finally { $process.Dispose() }
    }
    Write-Output 'PASS: producer -> Windows pair transport -> VRX D3D12 eye textures; stereo, mono and wrong-owner rejection'
}
finally {
    if ($started -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
    $child.Dispose()
    if($fixtureRoot) {
        $resolved=[IO.Path]::GetFullPath($fixtureRoot);$temporary=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if(-not $resolved.StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^VRX-DX12-engine-[0-9a-f]{32}$') {throw 'Refusing cleanup outside owned native DX12 fixture directory'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
