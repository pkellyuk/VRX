param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$OutputRoot) { $OutputRoot = Join-Path $PSScriptRoot ('out\v1.6.0-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$payload = Join-Path $OutputRoot 'VRX-1.6.0-win-x64'
$nativeOutput = Join-Path $OutputRoot 'native'
if (Test-Path -LiteralPath $payload) { throw 'Choose a fresh output folder; existing packages are never overwritten.' }
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Push-Location $projectRoot
$oldNativeOutput = $env:VRX_NATIVE_OUT
try {
    $env:VRX_NATIVE_OUT = $nativeOutput
    & cmd /c 'bench\native\openxr\build.bat --desktop'
    if ($LASTEXITCODE) { throw 'Native build failed' }
    $env:LIB = ''
    & dotnet publish desktop/VRX.Desktop/VRX.Desktop.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -o $payload
    if ($LASTEXITCODE) { throw 'Desktop publish failed' }
    Get-ChildItem -LiteralPath $payload -Filter '*.pdb' | Remove-Item
    $engine = New-Item -ItemType Directory -Path (Join-Path $payload 'engine')
    $models = New-Item -ItemType Directory -Path (Join-Path $engine 'models')
    $licenses = New-Item -ItemType Directory -Path (Join-Path $payload 'licenses')
    $packages = Join-Path $env:USERPROFILE '.nuget\packages'
    $ortPackage = Join-Path $packages 'microsoft.ml.onnxruntime.directml\1.24.4'
    $dmlPackage = Join-Path $packages 'microsoft.ai.directml\1.15.4'
    Copy-Item "$nativeOutput\xrplayer.exe","$nativeOutput\openxr_loader.dll" $engine
    Copy-Item "$ortPackage\runtimes\win-x64\native\*.dll" $engine
    Copy-Item "$dmlPackage\bin\x64-win\DirectML.dll" $engine
    Copy-Item bench/models/model_fixed_686x392.onnx $models
    # Default depth model (Depth Anything V2 above is the per-game alternative); generated, not downloaded.
    $zipDepth = 'bench/models/zipdepth_faithful_fp16_672x384.onnx'
    if (!(Test-Path $zipDepth)) { throw "$zipDepth missing: see the setup steps at the top of bench/zipdepth_export.py" }
    Copy-Item $zipDepth $models
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1
    if (!$crt) { throw 'Visual C++ redistributable directory missing' }
    Copy-Item "$crt\*.dll" $engine
    Copy-Item "$crt\*.dll" $payload
    Copy-Item release/licenses/*.txt $licenses
    Copy-Item "$ortPackage\LICENSE" "$licenses\ONNXRuntime-LICENSE.txt"
    Copy-Item "$ortPackage\ThirdPartyNotices.txt" "$licenses\ONNXRuntime-ThirdPartyNotices.txt"
    Copy-Item "$dmlPackage\LICENSE.txt" "$licenses\DirectML-LICENSE.txt"
    Copy-Item "$dmlPackage\ThirdPartyNotices.txt" "$licenses\DirectML-ThirdPartyNotices.txt"
    $runtimeConfig = Get-Content "$payload\VRX.Desktop.runtimeconfig.json" -Raw | ConvertFrom-Json
    foreach ($framework in $runtimeConfig.runtimeOptions.includedFrameworks) {
        $runtimePackage = Join-Path $packages ($framework.name.ToLowerInvariant() + '.runtime.win-x64\' + $framework.version)
        $runtimeLicense = Get-ChildItem -LiteralPath $runtimePackage -File | Where-Object Name -match '^LICENSE(\.TXT)?$' | Select-Object -First 1
        if (!$runtimeLicense) { throw "Runtime license missing: $runtimePackage" }
        Copy-Item -LiteralPath $runtimeLicense.FullName -Destination "$licenses\$($framework.name)-LICENSE.txt"
        Get-ChildItem -LiteralPath $runtimePackage -File | Where-Object Name -match '^THIRD-PARTY-NOTICES\.TXT$' | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination "$licenses\$($framework.name)-ThirdPartyNotices.txt"
        }
    }
    Copy-Item release/README.txt,release/THIRD-PARTY-NOTICES.txt $payload
    Copy-Item LICENSE $payload                          # VRX's own licence (GPL-3.0)
    $commit = & git rev-parse HEAD
    $dirty = & git status --porcelain
    @("VRX 1.6.0", "Source commit: $commit", "Working tree dirty: $([bool]$dirty)", "Built UTC: $([DateTime]::UtcNow.ToString('O'))") | Set-Content "$payload\BUILD.txt"
    foreach ($library in 'onnxruntime.dll','DirectML.dll','openxr_loader.dll') {
        $version = (Get-Item -LiteralPath (Join-Path $engine $library)).VersionInfo.FileVersion
        "$library : $version" | Add-Content "$payload\BUILD.txt"
    }
    Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object Name -ne 'FILES.sha256.txt' | Sort-Object FullName | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($payload, $_.FullName)
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    } | Set-Content "$payload\FILES.sha256.txt"
    $compiler = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
    & $compiler /Qp "/DPayload=$payload" "/DArtifacts=$OutputRoot" release/VRX.iss
    if ($LASTEXITCODE) { throw 'Installer build failed' }
    Compress-Archive -LiteralPath $payload -DestinationPath "$OutputRoot\VRX-1.6.0-win-x64.zip" -CompressionLevel Optimal
    Get-ChildItem -LiteralPath $OutputRoot -File | Where-Object Extension -in '.exe','.zip' | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
    } | Set-Content "$OutputRoot\SHA256SUMS.txt"
    Write-Output "Release artifacts: $OutputRoot"
} finally { $env:VRX_NATIVE_OUT = $oldNativeOutput; Pop-Location }
