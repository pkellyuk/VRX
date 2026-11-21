param([string]$PersonaBotRepository = (Join-Path $PSScriptRoot '..\..\PersonaBot'))
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PersonaBotRepository 'artifacts\qemu-shared-gpu'
$target = Join-Path $repository 'integrations\personabot\runtime'
foreach ($file in 'qemu-system-x86_64.exe','qemu-img.exe','personabot-build.json','share\bios-256k.bin') {
    if (!(Test-Path -LiteralPath (Join-Path $source $file))) { throw "Build PersonaBot's bundled QEMU first; missing $file" }
}
New-Item -ItemType Directory -Force -Path $target | Out-Null
foreach ($file in 'qemu-system-x86_64.exe','qemu-img.exe','personabot-build.json') { Copy-Item -LiteralPath (Join-Path $source $file) -Destination $target }
Get-ChildItem -LiteralPath $source -Filter '*.dll' -File | Copy-Item -Destination $target
foreach ($folder in 'share','licenses') {
    if (Test-Path -LiteralPath (Join-Path $source $folder)) { Copy-Item -LiteralPath (Join-Path $source $folder) -Destination $target -Recurse -Force }
}
Write-Host 'Prepared the bundled x86-64 QEMU runtime; no PersonaBot settings, credentials or VM disks were imported.'
