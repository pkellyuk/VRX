$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$s = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr-spec.html')
Write-Output '=== 1.1 SPEC: CompositionLayerDepth function names ==='
([regex]'xr[A-Za-z]*CompositionLayerDepth[A-Za-z]*').Matches($s) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
Write-Output '=== 1.1 SPEC: context around XrCompositionLayerDepthInfoKHR struct ==='
$rx = [regex]'struct XrCompositionLayerDepthInfoKHR'
$m = $rx.Matches($s) | Select-Object -First 1
if ($m) {
    $start = [Math]::Max(0, $m.Index - 200)
    $len = [Math]::Min(2200, $s.Length - $start)
    Write-Output (($s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '))
} else { Write-Output '(not found)' }
Write-Output '=== 1.1 SPEC: context around XR_FORMAT_D32_SFLOAT ==='
$rx2 = [regex]'XR_FORMAT_D32_SFLOAT'
$m2 = $rx2.Matches($s) | Select-Object -First 1
if ($m2) {
    $start = [Math]::Max(0, $m2.Index - 400)
    $len = [Math]::Min(1800, $s.Length - $start)
    Write-Output (($s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '))
} else { Write-Output '(not found)' }
