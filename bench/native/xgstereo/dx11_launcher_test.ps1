#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path $PSScriptRoot ('out/dx11-launcher-test-' + [Guid]::NewGuid().ToString('N'))
$gameRoot = Join-Path $testRoot 'game'
$sourceRoot = Join-Path $testRoot 'dxvk'
$null = New-Item -ItemType Directory -Path $gameRoot, $sourceRoot
$application = Join-Path $gameRoot 'probe.exe'
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/cmd.exe') -Destination $application
foreach ($dll in @('d3d11.dll','dxgi.dll')) { 'test DLL content; never loaded by cmd' | Set-Content -LiteralPath (Join-Path $sourceRoot $dll) }
$launch = Join-Path $PSScriptRoot 'launch-dx11.ps1'
function Invoke-Probe([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Command pwsh).Source)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($arg in @('-NoProfile', '-File', $launch, '-Executable', $application, '-DxvkDirectory', $sourceRoot) + $Arguments) { $start.ArgumentList.Add($arg) }
    $child = [Diagnostics.Process]::Start($start)
    $stdout = $child.StandardOutput.ReadToEndAsync()
    $stderr = $child.StandardError.ReadToEndAsync()
    if (-not $child.WaitForExit(15000)) { $child.Kill(); throw 'DX11 launcher probe timed out.' }
    $code = $child.ExitCode
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    $child.Dispose()
    return @{ Code=$code; Output=$output }
}
$mirrorPreview = Invoke-Probe @('-Preview','-Stereo','mirror')
if ($mirrorPreview.Code -ne 0 -or $mirrorPreview.Output -notmatch 'mirror') { throw 'Mirror preview must expose the selected backend.' }
$invalidTransport = Invoke-Probe @('-Preview','-Stereo','mirror','-Transport','cpu')
if ($invalidTransport.Code -eq 0 -or $invalidTransport.Output -notmatch 'requires GPU') { throw 'Mirror mode must reject CPU transport before installing files.' }
$preview = Invoke-Probe @('-Preview')
if ($preview.Code -ne 0) { throw $preview.Output }
foreach ($dll in @('d3d11.dll','dxgi.dll')) { if (Test-Path -LiteralPath (Join-Path $gameRoot $dll)) { throw 'Preview installed a DLL.' } }
# Use a real child executable that exits without loading graphics libraries.
# Its default command line is invalid for cmd, so it returns a nonzero exit.
$run = Invoke-Probe @()
if ($run.Output -notmatch 'Geometry game PID') { throw ('Child was not launched: ' + $run.Output) }
foreach ($dll in @('d3d11.dll','dxgi.dll')) { if (Test-Path -LiteralPath (Join-Path $gameRoot $dll)) { throw 'Launcher failed to clean its temporary DLL.' } }
$existing = Join-Path $gameRoot 'dxgi.dll'
'existing game file' | Set-Content -LiteralPath $existing
$originalHash = (Get-FileHash -LiteralPath $existing).Hash
$refused = Invoke-Probe @()
if ($refused.Code -eq 0 -or $refused.Output -notmatch 'Refusing to replace' -or (Get-FileHash -LiteralPath $existing).Hash -ne $originalHash) {
    throw 'Launcher must preserve existing game DLLs and refuse to start.'
}
if (Test-Path -LiteralPath (Join-Path $gameRoot 'd3d11.dll')) { throw 'Refusal must happen before installing any file.' }
Write-Output 'PASS: DX11 launcher preview, child exit cleanup and preservation of existing game DLLs'
