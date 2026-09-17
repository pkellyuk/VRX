$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr.h')
Write-Output '=== depth/stencil formats ==='
([regex]'XR_FORMAT_(D[0-9]+[A-Z_]*|X[0-9]+_D[0-9]+[A-Z_]*) = 0x[0-9A-Fa-f]+,').Matches($h) | ForEach-Object { $_.Value } | ForEach-Object { Write-Output $_ }
