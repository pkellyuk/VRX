$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$f10 = Join-Path $refs 'openxr-spec-1.0.html'
Write-Output ('1.0 spec size: ' + (Get-Item $f10 -ErrorAction SilentlyContinue).Length)
$s0 = Get-Content -Raw -LiteralPath $f10
Write-Output '=== 1.0 SPEC: stencil (case-insensitive) distinct ==='
([regex]'stencil').Matches($s0) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
Write-Output '=== 1.0 SPEC: DepthStencilSwapchain distinct ==='
([regex]'[A-Za-z]*DepthStencilSwapchain[A-Za-z]*').Matches($s0) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

$s = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr-spec.html')

Write-Output ''
Write-Output '=== 1.1 SPEC: COMPOSITION_LAYER_DEPTH (case-insensitive) distinct ==='
([regex]'[A-Za-z0-9_]*COMPOSITION_LAYER_DEPTH[A-Za-z0-9_]*').Matches($s) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output ''
Write-Output '=== 1.1 SPEC: context around META environment depth ext name ==='
$rx = [regex]'XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME'
$m = $rx.Matches($s) | Select-Object -First 1
if ($m) {
    $start = [Math]::Max(0, $m.Index - 100)
    $len = [Math]::Min(2500, $s.Length - $start)
    Write-Output (($s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '))
} else { Write-Output '(not found)' }

Write-Output ''
Write-Output '=== 1.1 SPEC: context around PASSTHROUGH_LAYER_DEPTH_BIT_FB ==='
$rx2 = [regex]'XR_PASSTHROUGH_LAYER_DEPTH_BIT_FB'
$m2 = $rx2.Matches($s) | Select-Object -First 1
if ($m2) {
    $start = [Math]::Max(0, $m2.Index - 300)
    $len = [Math]::Min(2200, $s.Length - $start)
    Write-Output (($s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '))
} else { Write-Output '(not found)' }

Write-Output ''
Write-Output '=== 1.1 SPEC: context around REPROJECTION_MODE_DEPTH_MSFT ==='
$rx3 = [regex]'XR_REPROJECTION_MODE_DEPTH_MSFT'
$m3 = $rx3.Matches($s) | Select-Object -First 1
if ($m3) {
    $start = [Math]::Max(0, $m3.Index - 300)
    $len = [Math]::Min(2200, $s.Length - $start)
    Write-Output (($s.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '))
} else { Write-Output '(not found)' }
