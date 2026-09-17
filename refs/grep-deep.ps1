$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Join-Path $refs 'openxr.h'
$s = Join-Path $refs 'openxr-spec.html'

Write-Output '=== HEADER: xr* function names containing Depth ==='
$hc = Get-Content -Raw -LiteralPath $h
$rx = [regex]'xr[A-Za-z0-9]*[Dd]epth[A-Za-z0-9]*'
$rx.Matches($hc) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== HEADER: xr* function names containing Stencil ==='
$rx2 = [regex]'xr[A-Za-z0-9]*[Ss]tencil[A-Za-z0-9]*'
$rx2.Matches($hc) | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== SPEC: context around depth_stencil (200 chars each) ==='
$sc = Get-Content -Raw -LiteralPath $s
$rx3 = [regex]'depth_stencil'
$i = 0
foreach ($m in $rx3.Matches($sc)) {
    $start = [Math]::Max(0, $m.Index - 120)
    $len = [Math]::Min(280, $sc.Length - $start)
    $chunk = $sc.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '
    Write-Output ('--- match ' + $i + ': ' + $chunk)
    $i++
}
