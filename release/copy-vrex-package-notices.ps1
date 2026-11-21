param(
    [Parameter(Mandatory)] [string]$Payload,
    [Parameter(Mandatory)] [string]$Packages
)

$ErrorActionPreference = 'Stop'
$destination = Join-Path $Payload 'licenses/vrex'
New-Item -Path $destination -ItemType Directory -Force | Out-Null
$worker = Join-Path $Payload 'vrex'
$packageIds = foreach ($deps in @('VRX.VrexHost.deps.json', 'voice/personabot-voice.deps.json')) {
    $manifest = Get-Content -LiteralPath (Join-Path $worker $deps) -Raw | ConvertFrom-Json
    $manifest.libraries.PSObject.Properties | Where-Object { $_.Value.type -eq 'package' } |
        Select-Object -ExpandProperty Name
}
$index = foreach ($id in ($packageIds | Sort-Object -Unique)) {
    $path = Join-Path $Packages $id.ToLowerInvariant()
    $notices = Get-ChildItem -LiteralPath $path -File | Where-Object {
        $_.Name -match '^(licen[cs]e|copying|notice|third[- ]?party[- ]?notices)(\.[a-z]+)?$'
    }
    foreach ($notice in $notices) {
        Copy-Item -LiteralPath $notice.FullName -Destination (Join-Path $destination "$($id.Replace('/', '-'))-$($notice.Name)")
    }
    $id
}
$index | Set-Content -LiteralPath (Join-Path $destination 'package-versions.txt')
