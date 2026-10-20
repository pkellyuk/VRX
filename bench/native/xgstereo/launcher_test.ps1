#requires -Version 7.0
param([Parameter(Mandatory)][string]$LayerDirectory)
$ErrorActionPreference = 'Stop'
$LayerDirectory = (Resolve-Path -LiteralPath $LayerDirectory).Path
$names = @('VRX_XG_EYE', 'VRX_XG_SHIFT', 'VRX_XG_SESSION', 'VK_INSTANCE_LAYERS', 'VK_ADD_LAYER_PATH')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$output = Join-Path $LayerDirectory 'launcher-probe.json'
try {
    $env:VRX_XG_EYE = 'parent-eye'
    $env:VRX_XG_SHIFT = 'parent-shift'
    $env:VRX_XG_SESSION = 'parent-session'
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_test_existing'
    $env:VK_ADD_LAYER_PATH = 'C:\existing layer path'
    $literal = 'literal $() `text` "quoted" ; & spaces'
    $gameArguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'launcher_probe.ps1'),
        '-OutputPath', $output, '-First', 'argument with spaces', '-Second', $literal)
    & (Join-Path $PSScriptRoot 'launch.ps1') -Executable (Join-Path $PSHOME 'pwsh.exe') `
        -LayerDirectory $LayerDirectory -Eye right -Shift 0.125 -GameArguments $gameArguments
    if ($LASTEXITCODE -ne 7) { throw 'Child exit code was not preserved.' }
    $result = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    if ($result.Eye -ne 'right' -or $result.Shift -ne '0.125') { throw 'Child eye configuration differs.' }
    if ($result.Session) { throw 'Fixed-eye launch must not inherit an unrelated geometry session.' }
    if ($result.First -cne 'argument with spaces' -or $result.Second -cne $literal) { throw 'Arguments were changed or evaluated.' }
    if ($result.Layers -ne 'VK_LAYER_test_existing;VK_LAYER_VRX_xgstereo') { throw 'Existing layers not retained.' }
    if ($result.Paths -ne "$LayerDirectory;C:\existing layer path") { throw 'Existing layer paths not retained.' }
    if ($env:VRX_XG_EYE -ne 'parent-eye' -or $env:VRX_XG_SHIFT -ne 'parent-shift' -or
        $env:VK_INSTANCE_LAYERS -ne 'VK_LAYER_test_existing' -or $env:VK_ADD_LAYER_PATH -ne 'C:\existing layer path') {
        throw 'Launcher changed the parent environment.'
    }
    & (Join-Path $PSScriptRoot 'launch.ps1') -Executable (Join-Path $PSHOME 'pwsh.exe') `
        -LayerDirectory $LayerDirectory -Eye alternate -Session test-game -Shift 3.2 -GameArguments $gameArguments
    if ($LASTEXITCODE -ne 7) { throw 'Alternate launcher child exit code differs.' }
    $result = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    if ($result.Session -ne 'test-game' -or $result.Eye -ne 'alternate' -or $result.Shift -ne '3.2' -or $env:VRX_XG_SESSION -ne 'parent-session') {
        throw 'Alternate session must be passed only to the child.'
    }
    Write-Output 'PASS: child-only layer settings, literal arguments, inherited layers, child exit code'
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
}
