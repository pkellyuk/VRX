$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$spec = Join-Path $refs 'openxr-spec.html'

Write-Output '=== EXTENSION NAMES mentioning depth ==='
$matches = Select-String -Path $spec -Pattern 'XR_[A-Z0-9_]*[Dd]epth[A-Z0-9_]*' -AllMatches
$matches.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== EXTENSION NAMES mentioning passthrough ==='
$matches2 = Select-String -Path $spec -Pattern 'XR_[A-Z0-9_]*[Pp]assthrough[A-Z0-9_]*' -AllMatches
$matches2.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }

Write-Output '=== EXTENSION NAMES mentioning composition_layer ==='
$matches3 = Select-String -Path $spec -Pattern 'XR_[A-Z0-9_]*composition_layer[A-Z0-9_]*' -AllMatches
$matches3.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
