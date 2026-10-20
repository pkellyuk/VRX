#requires -Version 7.0
param([Parameter(Mandatory)][string]$Engine, [Parameter(Mandatory)][string]$Producer, [switch]$Gpu, [switch]$SameFrame, [switch]$Mirror)
$ErrorActionPreference = 'Stop'
if ($Mirror) { $Gpu = $true; $SameFrame = $true; }
$session = 'check-' + [Guid]::NewGuid().ToString('N')
$producerPath = (Resolve-Path -LiteralPath $Producer).Path
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$start = [Diagnostics.ProcessStartInfo]::new($producerPath)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Environment['VK_LAYER_PATH'] = [IO.Path]::GetDirectoryName($producerPath)
$start.ArgumentList.Add($(if ($Mirror) { '--serve-mirror-gpu=' } elseif ($Gpu) { '--serve-gpu=' } else { '--serve=' }) + $session)
$child = [Diagnostics.Process]::new()
$child.StartInfo = $start
try {
    if (-not $child.Start()) { throw 'Geometry test producer did not start.' }
    $producerOut = $child.StandardOutput.ReadToEndAsync()
    $producerErr = $child.StandardError.ReadToEndAsync()
    # Wait for the named mapping, not an arbitrary shader-startup delay.
    $deadline = [Environment]::TickCount64 + 10000
    $ready = $false
    do {
        try {
            $map = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\VRX.xgstereo.' + $session + $(if ($Gpu) { '.gpu.frames' } else { '.frames' }))
            $map.Dispose(); $ready = $true
        } catch [IO.FileNotFoundException] { Start-Sleep -Milliseconds 10 }
        if ($child.HasExited) { throw ('Producer failed: ' + $producerErr.GetAwaiter().GetResult()) }
    } while (-not $ready -and [Environment]::TickCount64 -lt $deadline)
    if (-not $ready) { throw 'Producer mapping unavailable.' }
    foreach ($mode in @('stereo', 'mono', 'wrong-owner')) {
        $check = [Diagnostics.ProcessStartInfo]::new($enginePath)
        $check.UseShellExecute = $false
        $check.CreateNoWindow = $true
        $check.RedirectStandardOutput = $true
        $check.RedirectStandardError = $true
        foreach ($argument in @(('--geometry-session=' + $session), '--check-geometry', '--debug', '--normal-gpu-priority')) {
            $check.ArgumentList.Add($argument)
        }
        $check.ArgumentList.Add('--geometry-pid=' + $(if ($mode -eq 'wrong-owner') { '4294967295' } else { $child.Id }))
        if ($mode -eq 'mono') { $check.ArgumentList.Add('--no-warp') }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $check
        try {
            if (-not $process.Start()) { throw 'Engine check did not start.' }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(15000)) { $process.Kill(); $process.WaitForExit(); throw 'Engine check timed out.' }
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
        }
        finally { $process.Dispose() }
    }
    Write-Output 'PASS: producer -> Windows pair transport -> VRX D3D12 eye textures; stereo, mono and wrong-owner rejection'
}
finally {
    if ($child.Id -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
    $child.Dispose()
}
