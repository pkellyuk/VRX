$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr.h')
Write-Output '=== HEADER: compositionlayerdepth (case-insensitive) distinct ==='
([regex]'[A-Za-z0-9_]*compositionlayerdepth[A-Za-z0-9_]*').Matches($h) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
Write-Output '=== HEADER: context around XrCompositionLayerDepthInfoKHR struct def ==='
$rx = [regex]'typedef struct XrCompositionLayerDepthInfoKHR'
$m = $rx.Matches($h) | Select-Object -First 1
if ($m) {
    $len = [Math]::Min(1200, $h.Length - $m.Index)
    Write-Output ($h.Substring($m.Index, $len))
} else { Write-Output '(not found)' }
Write-Output '=== HEADER: context around xrLocateCompositionLayerDepthInfoKHR ==='
$rx2 = [regex]'xrLocateCompositionLayerDepthInfoKHR'
$m2 = $rx2.Matches($h) | Select-Object -First 1
if ($m2) {
    $start = [Math]::Max(0, $m2.Index - 800)
    $len = [Math]::Min(1500, $h.Length - $start)
    Write-Output ($h.Substring($start, $len))
} else { Write-Output '(not found)' }
Write-Output '=== HEADER: XrDepthInfo / depth info struct names ==='
([regex]'[A-Za-z0-9_]*DepthInfo[A-Za-z0-9_]*').Matches($h) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
