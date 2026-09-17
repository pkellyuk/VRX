$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Join-Path $refs 'openxr.h'
$s = Join-Path $refs 'openxr-spec.html'
$hc = Get-Content -Raw -LiteralPath $h
$sc = Get-Content -Raw -LiteralPath $s

Write-Output '=== HEADER: context around DEPTH_STENCIL_ATTACHMENT flag ==='
$rx = [regex]'XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT'
foreach ($m in $rx.Matches($hc)) {
    $start = [Math]::Max(0, $m.Index - 2500)
    $len = [Math]::Min(3200, $hc.Length - $start)
    $chunk = $hc.Substring($start, $len) -replace '\s+', ' '
    Write-Output $chunk
    break
}

Write-Output ''
Write-Output '=== SPEC: context around DEPTH_STENCIL_ATTACHMENT (1500 chars) ==='
$rx2 = [regex]'XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT'
foreach ($m in $rx2.Matches($sc)) {
    $start = [Math]::Max(0, $m.Index - 200)
    $len = [Math]::Min(1700, $sc.Length - $start)
    $chunk = $sc.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '
    Write-Output $chunk
    break
}

Write-Output ''
Write-Output '=== SPEC: extension page titles containing depth ==='
$rx3 = [regex]'<title>([^<]*[Dd]epth[^<]*)</title>'
$rx3.Matches($sc) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
