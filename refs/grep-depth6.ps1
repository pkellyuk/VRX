$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'
$h = Get-Content -Raw -LiteralPath (Join-Path $refs 'openxr.h')
Write-Output '=== swapchain usage flags ==='
([regex]'XR_SWAPCHAIN_USAGE_[A-Z_]+ = 0x[0-9A-Fa-f]+;').Matches($h) | ForEach-Object { $_.Value } | ForEach-Object { Write-Output $_ }
Write-Output '=== depth formats ==='
([regex]'#define XR_FORMAT_[A-Z0-9_]*D[0-9]+[A-Z0-9_]* 0x[0-9A-Fa-f]+').Matches($h) | ForEach-Object { $_.Value } | ForEach-Object { Write-Output $_ }
Write-Output '=== XrSwapchainCreateInfo (usageFlags context) ==='
$rx = [regex]'usageFlags'
$m = $rx.Matches($h) | Select-Object -First 1
if ($m) { $start = [Math]::Max(0, $m.Index - 600); $len = [Math]::Min(1400, $h.Length - $start); Write-Output ($h.Substring($start, $len)) }
Write-Output '=== XR_KHR_composition_layer_depth extension block ==='
$rx2 = [regex]'XR_KHR_composition_layer_depth is a preprocessor guard'
$m2 = $rx2.Matches($h) | Select-Object -First 1
if ($m2) { $len = [Math]::Min(1600, $h.Length - $m2.Index); Write-Output ($h.Substring($m2.Index, $len)) } else { Write-Output '(not found)' }
