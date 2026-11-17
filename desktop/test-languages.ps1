param(
    [string[]]$Languages = @('en', 'fr', 'de', 'es', 'it', 'pt', 'zh-Hans', 'ja', 'ko')
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$app = Join-Path $PSScriptRoot 'VRX.Desktop\bin\Release\net10.0-windows\VRX.Desktop.exe'
if (!(Test-Path -LiteralPath $app)) { throw 'Build VRX.Desktop in Release first.' }
$output = Join-Path $PSScriptRoot 'out'
foreach ($language in $Languages) {
    if ($language -notin @('en', 'fr', 'de', 'es', 'it', 'pt', 'zh-Hans', 'ja', 'ko')) { throw "Unsupported test language: $language" }
    Write-Host "Testing UI language: $language"
    # Sequential: the existing smoke suite owns a shared, isolated smoke-data store.
    $process = Start-Process -FilePath $app -ArgumentList '--smoke-test', "--language=$language" -WorkingDirectory $repository -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(120000)) { $process.Kill(); throw "$language smoke test timed out" }
    $folder = Join-Path $output "international\$language"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    if ($process.ExitCode -ne 0) {
        Copy-Item -LiteralPath (Join-Path $output 'smoke-error.txt') -Destination $folder
        throw "$language smoke test failed: $(Get-Content -LiteralPath (Join-Path $output 'smoke-error.txt') -Raw)"
    }
    $previousError = Join-Path $folder 'smoke-error.txt'
    if (Test-Path -LiteralPath $previousError) { Remove-Item -LiteralPath $previousError }
    foreach ($file in @('ui-easy-skin.png', 'ui-expert-skin.png', 'ui-expert-all-open.png', 'controller-menu-sections.png', 'controller-menu-screen.png', 'smoke-pass.txt', 'languages-pass.txt')) {
        Copy-Item -LiteralPath (Join-Path $output $file) -Destination $folder
    }
    Write-Host "PASS: $language"
}
