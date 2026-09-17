$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Join-Path $refs 'openxr.h'
$s = Join-Path $refs 'openxr-spec.html'

Write-Output '=== HEADER: depth-related identifiers ==='
$matches = Select-String -Path $h -Pattern 'XR_[A-Za-z0-9_]*[Dd]epth[A-Za-z0-9_]*' -AllMatches
$matches.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== HEADER: stencil identifiers ==='
$matches2 = Select-String -Path $h -Pattern '[A-Za-z0-9_]*[Ss]tencil[A-Za-z0-9_]*' -AllMatches
$matches2.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== SPEC: depth_stencil mentions ==='
$matches3 = Select-String -Path $s -Pattern 'depth_stencil' -AllMatches
Write-Output ('count: ' + $matches3.Count)
$matches3.Matches.Value | Sort-Object -Unique | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
