$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'

# 1.0 spec check
try {
    Invoke-WebRequest -Uri 'https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html' -OutFile (Join-Path $refs 'openxr-spec-1.0.html') -UseBasicParsing
    $s0 = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr-spec-1.0.html')
    $rx0 = [regex]'XR_(KHR|MSFT|EXT|ML|META|FB|VARJO|AMD|INTEL|NVIDIA|GOOGLE)_[A-Za-z0-9]*[Dd]epth[A-Za-z0-9]*'
    Write-Output '=== 1.0 SPEC: depth extension names ==='
    $rx0.Matches($s0) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
} catch { Write-Output ('1.0 spec FAIL: ' + $_.Exception.Message) }

$s = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr-spec.html')

Write-Output ''
Write-Output '=== 1.1 SPEC: MND_ identifiers ==='
$rx = [regex]'XR_MND_[A-Za-z0-9_]*'
$rx.Matches($s) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output ''
Write-Output '=== 1.1 SPEC: context around composition_layer_depth ext name ==='
$rx2 = [regex]'XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME'
foreach ($m in $rx2.Matches($s)) {
    $start = [Math]::Max(0, $m.Index - 100)
    $len = [Math]::Min(2500, $s.Length - $start)
    $chunk = $s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '
    Write-Output $chunk
    break
}

Write-Output ''
Write-Output '=== 1.1 SPEC: context around xrSetEnvironmentDepthEstimationVARJO ==='
$rx3 = [regex]'xrSetEnvironmentDepthEstimationVARJO'
foreach ($m in $rx3.Matches($s)) {
    $start = [Math]::Max(0, $m.Index - 200)
    $len = [Math]::Min(2500, $s.Length - $start)
    $chunk = $s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '
    Write-Output $chunk
    break
}
