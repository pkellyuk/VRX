$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$s = Join-Path $refs 'openxr-spec.html'
$sc = Get-Content -Raw -LiteralPath $s

foreach ($pat in @('stencil', 'MSFT_depth', 'KHR_depth', 'EXT_depth', 'ML_depth', 'GOOGLE_depth')) {
    Write-Output ('=== SPEC: ' + $pat + ' ===')
    $rx = [regex]($pat)
    $vals = $rx.Matches($sc) | ForEach-Object { $_.Value } | Sort-Object -Unique
    Write-Output ('distinct values: ' + ($vals -join ', '))
}

Write-Output '=== SPEC: context around each stencil hit (300 chars) ==='
$rx2 = [regex]'stencil'
$i = 0
foreach ($m in $rx2.Matches($sc)) {
    $start = [Math]::Max(0, $m.Index - 150)
    $len = [Math]::Min(320, $sc.Length - $start)
    $chunk = $sc.Substring($start, $len) -replace '<[^>]+>', ' ' -replace '\s+', ' '
    Write-Output ('--- ' + $i + ': ' + $chunk)
    $i++
    if ($i -ge 15) { break }
}
