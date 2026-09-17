$ErrorActionPreference = 'SilentlyContinue'
Write-Host '--- compilers/tools ---'
foreach ($c in 'cl','cmake','dotnet','g++') {
  $cmd = Get-Command $c -ErrorAction SilentlyContinue
  if ($cmd) { Write-Host ("{0} -> {1}" -f $cmd.Name, $cmd.Source) }
}
$vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vs) { Write-Host ("vswhere: " + (& $vs -latest -property installationPath)) } else { Write-Host 'vswhere: not found' }
Write-Host '--- OpenXR runtimes (registry) ---'
$hklm = Get-ChildItem 'HKLM:\SOFTWARE\Khronos\OpenXR'
if ($hklm) { $hklm | ForEach-Object { Write-Host ("HKLM: " + $_.PSChildName); Get-ItemProperty $_.PSPath | ForEach-Object { Write-Host ("  Path=" + $_.Path) } } } else { Write-Host 'HKLM: none' }
$hkcu = Get-ChildItem 'HKCU:\SOFTWARE\Khronos\OpenXR'
if ($hkcu) { $hkcu | ForEach-Object { Write-Host ("HKCU: " + $_.PSChildName) } } else { Write-Host 'HKCU: none' }
Write-Host '--- SteamVR ---'
Write-Host ("SteamVR install dir: " + (Test-Path 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'))
$vr = Get-Process | Where-Object { $_.ProcessName -match 'vrmonitor|vrserver' }
if ($vr) { $vr | Select-Object -ExpandProperty ProcessName } else { Write-Host 'no SteamVR processes running' }
Write-Host '--- dotnet SDKs ---'
dotnet --list-sdks
