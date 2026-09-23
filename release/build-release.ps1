# Builds a VRX release. With no -Stage it does everything on this PC, unsigned, as before.
# The GitHub Actions build (.github/workflows/release.yml) runs it in stages around
# SignPath's code signing:
#   Payload    build everything into <out>\VRX-<version>-win-x64, plus <out>\to-sign with
#              VRX's own programs and Inno Setup's uninstaller, for the first signing request
#   Package    put the signed files (-SignedFiles) back, then build the installer and ZIP
#   Checksums  SHA256SUMS.txt for the installer and ZIP (again after the installer is signed)
param(
    [string]$OutputRoot = '',
    [ValidateSet('All', 'Payload', 'Package', 'Checksums')] [string]$Stage = 'All',
    [string]$SignedFiles = ''
)
$ErrorActionPreference = 'Stop'
$version = '1.7.7'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$OutputRoot)
{
    if ($Stage -ne 'All' -and $Stage -ne 'Payload') { throw "-Stage $Stage needs the -OutputRoot of the Payload stage" }
    $OutputRoot = Join-Path $PSScriptRoot ("out\v$version-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$payload = Join-Path $OutputRoot "VRX-$version-win-x64"
$nativeOutput = Join-Path $OutputRoot 'native'
$toSign = Join-Path $OutputRoot 'to-sign'
# VRX's own programs: the only unsigned files in the payload (the rest are signed by Microsoft or Valve).
$ownPrograms = 'VRX.Desktop.exe', 'VRX.Desktop.dll', 'engine\xrplayer.exe'
Write-Output "build-release: $Stage, version $version, output $OutputRoot"

function Find-InnoCompiler
{
    foreach ($candidate in @($env:INNO_SETUP_COMPILER, (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')))
    {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'Inno Setup 6 (ISCC.exe) not found'
}

function Build-Payload
{
    if (Test-Path -LiteralPath $payload) { throw 'Choose a fresh output folder; existing packages are never overwritten.' }
    New-Item -ItemType Directory -Path $payload -Force | Out-Null
    Push-Location $projectRoot
    $oldNativeOutput = $env:VRX_NATIVE_OUT
    try {
        # Pinned inputs: the native NuGet packages and the two depth models (built from source).
        & dotnet restore release/NativePackages.csproj
        if ($LASTEXITCODE) { throw 'Native package restore failed' }
        & (Join-Path $PSScriptRoot 'build-models.ps1')
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
        # d3dcompiler_47.dll: the Windows SDK redistributable (copied by build.bat), loaded
        # app-local so that every PC compiles with the compiler the shader cache was built by.
        Copy-Item "$nativeOutput\xrplayer.exe","$nativeOutput\openxr_loader.dll","$nativeOutput\d3dcompiler_47.dll" $engine
        # The OpenXR loader must be the pinned, Valve-signed one (release\vendor\README.txt).
        $loader = Join-Path $engine 'openxr_loader.dll'
        if ((Get-FileHash -LiteralPath $loader -Algorithm SHA256).Hash -ne '9DAE7F85DCFF14352DF31D699153CEA72100D39BA3F3BA86C236291EF9265BAF') { throw 'openxr_loader.dll is not the pinned release\vendor copy' }
        if ((Get-AuthenticodeSignature -LiteralPath $loader).Status -ne 'Valid') { throw 'openxr_loader.dll signature is not valid' }
        # So must the shader compiler: the shipped shader cache is compiled with it.
        $shaderCompiler = Join-Path $engine 'd3dcompiler_47.dll'
        if ((Get-FileHash -LiteralPath $shaderCompiler -Algorithm SHA256).Hash -ne 'A05F99734F7C4822FEFC12B367AF21FD0976ED6608752FB1E1E80B6ECE7ECBBB') { throw 'd3dcompiler_47.dll is not the pinned release\vendor copy' }
        if ((Get-AuthenticodeSignature -LiteralPath $shaderCompiler).Status -ne 'Valid') { throw 'd3dcompiler_47.dll signature is not valid' }
        Copy-Item "$ortPackage\runtimes\win-x64\native\*.dll" $engine
        Copy-Item "$dmlPackage\bin\x64-win\DirectML.dll" $engine
        # Both built from their upstream sources and hash-checked by build-models.ps1.
        Copy-Item bench/models/model_fixed_686x392.onnx, bench/models/zipdepth_faithful_fp16_672x384.onnx $models
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1
        if (!$crt) { throw 'Visual C++ redistributable directory missing' }
        Copy-Item "$crt\*.dll" $engine
        Copy-Item "$crt\*.dll" $payload
        # Pre-compiled shaders, so the first start after an install does not spend 10-60 s in
        # D3DCompile with the VR session open. No headset, OpenXR runtime or GPU is needed.
        # The cache is keyed by d3dcompiler_47.dll's version too; the pre-warm runs the payload's
        # own xrplayer.exe, so it loads the app-local SDK compiler that ships with it.
        $warmLog = Join-Path $OutputRoot 'shader-cache-warm.log'
        & "$engine\xrplayer.exe" "--warm-shader-cache=$engine\shader-cache" *> $warmLog
        if ($LASTEXITCODE) { Get-Content $warmLog -Tail 20; throw 'Shader cache pre-warm failed' }
        if (!(Select-String -LiteralPath $warmLog -SimpleMatch "$engine\d3dcompiler_47.dll" -Quiet)) { Get-Content $warmLog -Tail 20; throw 'Shader cache pre-warm did not use the app-local d3dcompiler_47.dll' }
        Get-Content $warmLog -Tail 1
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
        $builtBy = if ($env:GITHUB_ACTIONS -eq 'true') { "GitHub Actions run $env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" } else { 'local build' }
        @("VRX $version", "Source commit: $commit", "Working tree dirty: $([bool]$dirty)", "Built by: $builtBy", "Built UTC: $([DateTime]::UtcNow.ToString('O'))") | Set-Content "$payload\BUILD.txt"
        foreach ($library in 'onnxruntime.dll','DirectML.dll','openxr_loader.dll','d3dcompiler_47.dll') {
            $libraryVersion = (Get-Item -LiteralPath (Join-Path $engine $library)).VersionInfo.FileVersion
            "$library : $libraryVersion" | Add-Content "$payload\BUILD.txt"
        }
    } finally { $env:VRX_NATIVE_OUT = $oldNativeOutput; Pop-Location }
}

# The files for the first signing request: VRX's own programs, and Inno Setup's uninstaller,
# which the compiler writes out (and stops) when the script asks for a signed uninstaller.
function Write-SigningInput
{
    if (Test-Path -LiteralPath $toSign) { throw "$toSign already exists" }
    foreach ($program in $ownPrograms)
    {
        $target = Join-Path $toSign $program
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $payload $program) -Destination $target
    }
    $uninstallers = New-Item -ItemType Directory -Path (Join-Path $toSign 'uninstaller') -Force
    $compiler = Find-InnoCompiler
    & $compiler /Qp "/DPayload=$payload" "/DArtifacts=$OutputRoot" "/DSignedUninstallerDir=$uninstallers" (Join-Path $PSScriptRoot 'VRX.iss') *> $null
    $global:LASTEXITCODE = 0                       # the compile stops on purpose after writing it
    $uninstaller = @(Get-ChildItem -LiteralPath $uninstallers -Filter 'uninst-*.e32')
    if ($uninstaller.Count -ne 1) { throw 'Inno Setup did not write its uninstaller for signing' }
    # It is a Windows program; SignPath signs it under an .exe name. Package renames it back.
    Rename-Item -LiteralPath $uninstaller[0].FullName -NewName ($uninstaller[0].Name + '.exe')
    Get-ChildItem -LiteralPath $toSign -Recurse -File | ForEach-Object { Write-Output "to sign: $([IO.Path]::GetRelativePath($toSign, $_.FullName))" }
}

function Build-Package
{
    if (!(Test-Path -LiteralPath $payload)) { throw "No payload at $($payload) - run the Payload stage first" }
    $innoDefines = @()
    if ($SignedFiles)
    {
        # Put the signed programs back, refusing anything that is not validly signed.
        foreach ($program in $ownPrograms)
        {
            $signed = Join-Path $SignedFiles $program
            if ((Get-AuthenticodeSignature -LiteralPath $signed).Status -ne 'Valid') { throw "$program is not validly signed" }
            Copy-Item -LiteralPath $signed -Destination (Join-Path $payload $program) -Force
            Write-Output "signed: $program"
        }
        $uninstaller = @(Get-ChildItem -LiteralPath (Join-Path $SignedFiles 'uninstaller') -Filter 'uninst-*.e32.exe')
        if ($uninstaller.Count -ne 1 -or (Get-AuthenticodeSignature -LiteralPath $uninstaller[0].FullName).Status -ne 'Valid') { throw 'The uninstaller is not validly signed' }
        $uninstallers = New-Item -ItemType Directory -Path (Join-Path $OutputRoot 'signed-uninstaller') -Force
        Copy-Item -LiteralPath $uninstaller[0].FullName -Destination (Join-Path $uninstallers ($uninstaller[0].Name -replace '\.exe$', ''))
        Write-Output "signed: uninstaller $($uninstaller[0].Name)"
        $innoDefines += "/DSignedUninstallerDir=$uninstallers"
    }
    Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object Name -ne 'FILES.sha256.txt' | Sort-Object FullName | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($payload, $_.FullName)
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    } | Set-Content "$payload\FILES.sha256.txt"
    $compiler = Find-InnoCompiler
    & $compiler /Qp "/DPayload=$payload" "/DArtifacts=$OutputRoot" @innoDefines (Join-Path $PSScriptRoot 'VRX.iss')
    if ($LASTEXITCODE) { throw 'Installer build failed' }
    Compress-Archive -LiteralPath $payload -DestinationPath "$OutputRoot\VRX-$version-win-x64.zip" -CompressionLevel Optimal
}

function Write-Checksums
{
    Get-ChildItem -LiteralPath $OutputRoot -File | Where-Object Extension -in '.exe','.zip' | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
    } | Set-Content "$OutputRoot\SHA256SUMS.txt"
    Get-Content "$OutputRoot\SHA256SUMS.txt"
}

switch ($Stage)
{
    'All'       { Build-Payload; Build-Package; Write-Checksums }
    'Payload'   { Build-Payload; Write-SigningInput }
    'Package'   { Build-Package; Write-Checksums }
    'Checksums' { Write-Checksums }
}
Write-Output "Release artifacts: $OutputRoot"
