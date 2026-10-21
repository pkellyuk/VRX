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
$version = '1.8.0'
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
# VRX's own programs: the files VRX signs. Everything else is signed by Microsoft or Valve,
# except the geometry runtime's DXVK and vkd3d-proton DLLs, which ship exactly as their
# projects release them (unsigned) and are pinned by hash instead (release\vendor).
$ownPrograms = 'VRX.Desktop.exe', 'VRX.Desktop.dll', 'vrx-steam-launch.exe', 'engine\xrplayer.exe', 'geometry\layer\vrx_xgstereo.dll'
# The geometry runtime's third-party DLLs: the official release archives, checked by these
# pinned SHA-256s, and each extracted DLL by release\vendor\geometry-runtime.sha256.
$geometryArchives = @(
    @{ Name = 'DXVK 2.7.1'; Folder = 'dxvk'; File = 'dxvk-2.7.1.tar.gz'
       Url = 'https://github.com/doitsujin/dxvk/releases/download/v2.7.1/dxvk-2.7.1.tar.gz'
       Sha256 = 'D85CE7C79F57ECD765AAA1B9E7007CB875E6FDE9F6D331DF799BCE73D513CE87'
       Members = @{ 'dxvk-2.7.1/x64/d3d11.dll' = 'd3d11.dll'; 'dxvk-2.7.1/x64/dxgi.dll' = 'dxgi.dll' } },
    @{ Name = 'vkd3d-proton 3.0.1'; Folder = 'vkd3d'; File = 'vkd3d-proton-3.0.1.tar.zst'
       Url = 'https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.0.1/vkd3d-proton-3.0.1.tar.zst'
       Sha256 = '3CF2315522AF5E43605EF6D3C41DAD91387040BF97199934F3F7AB76CAAA2F0C'
       Members = @{ 'vkd3d-proton-3.0.1/x64/d3d12.dll' = 'd3d12.dll'; 'vkd3d-proton-3.0.1/x64/d3d12core.dll' = 'd3d12core.dll' } }
)
$geometryPins = Join-Path $PSScriptRoot 'vendor\geometry-runtime.sha256'
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

# release\vendor\geometry-runtime.sha256 as a map of payload path -> SHA-256 (upper case).
function Read-GeometryPins
{
    Write-Output "geometry pins: reading $geometryPins" | Out-Host
    if (!(Test-Path -LiteralPath $geometryPins)) { throw "Missing $geometryPins" }
    $pins = @{}
    foreach ($line in Get-Content -LiteralPath $geometryPins)
    {
        if (!$line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})  (geometry\\[^\s]+)$') { throw "Malformed line in ${geometryPins}: $line" }
        $pins[$Matches[2]] = $Matches[1].ToUpperInvariant()
    }
    if ($pins.Count -ne 4) { throw "Expected 4 pinned geometry DLLs in $geometryPins, found $($pins.Count)" }
    Write-Output "geometry pins: $($pins.Count) files" | Out-Host
    return $pins
}

# The DXVK and vkd3d-proton DLLs: each official release archive is downloaded once into
# release\out\vendor-cache (kept between builds), checked against its pinned SHA-256, and
# only the x64 DLLs are extracted into the payload and checked against their own pins.
function Add-GeometryRuntime([string]$geometry)
{
    Write-Output "geometry runtime: enter, into $geometry"
    if (!$geometry) { throw 'Add-GeometryRuntime needs the payload geometry folder' }
    $pins = Read-GeometryPins
    $cache = Join-Path $PSScriptRoot 'out\vendor-cache'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    # Windows' own bsdtar (libarchive) reads .tar.gz and .tar.zst; Git's GNU tar may be first on PATH.
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (!(Test-Path -LiteralPath $tar)) { throw "Windows tar not found at $tar" }
    foreach ($archive in $geometryArchives)
    {
        $file = Join-Path $cache $archive.File
        if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $archive.Sha256)
        {
            Write-Output "geometry runtime: cached $($archive.File) does not match its pin - downloading again"
            Remove-Item -LiteralPath $file
        }
        if (!(Test-Path -LiteralPath $file))
        {
            Write-Output "geometry runtime: downloading $($archive.Url)"
            $partial = "$file.partial"
            Invoke-WebRequest -Uri $archive.Url -OutFile $partial -UseBasicParsing
            Move-Item -LiteralPath $partial -Destination $file -Force
        }
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        if ($hash -ne $archive.Sha256) { throw "$($archive.File) SHA-256 $hash is not the pinned $($archive.Sha256)" }
        Write-Output "geometry runtime: $($archive.Name) archive verified ($hash)"
        $staging = Join-Path $OutputRoot ('vendor-extract\' + $archive.Folder)
        if (Test-Path -LiteralPath $staging) { throw "$staging already exists" }
        New-Item -ItemType Directory -Path $staging -Force | Out-Null
        & $tar -xf $file -C $staging @($archive.Members.Keys)
        if ($LASTEXITCODE) { throw "Extracting $($archive.File) failed (this needs Windows' tar with zstd: Windows 11 23H2+ or Server 2025)" }
        $target = New-Item -ItemType Directory -Path (Join-Path $geometry $archive.Folder) -Force
        foreach ($member in $archive.Members.Keys)
        {
            $name = $archive.Members[$member]
            $relative = "geometry\$($archive.Folder)\$name"
            if (!$pins.ContainsKey($relative)) { throw "$relative has no pin in $geometryPins" }
            $extracted = Join-Path $staging ($member -replace '/', '\')
            $dllHash = (Get-FileHash -LiteralPath $extracted -Algorithm SHA256).Hash
            if ($dllHash -ne $pins[$relative]) { throw "$relative SHA-256 $dllHash is not the pinned $($pins[$relative])" }
            Copy-Item -LiteralPath $extracted -Destination (Join-Path $target $name)
            Write-Output "geometry runtime: $relative verified ($dllHash)"
        }
    }
    Copy-Item -LiteralPath $geometryPins -Destination (Join-Path $geometry 'geometry-runtime.sha256')
    Write-Output 'geometry runtime: exit ok'
}

# VRX's own Vulkan layer (bench\native\xgstereo), Release configuration, the layer target
# only: no test programs, no SPIR-V validation, and not the native DX12 proxy
# (bench\native\dx12stereo), which is never shipped. Its Vulkan and SPIR-V headers are
# fetched by CMake at the commits pinned in its CMakeLists.txt.
function Build-GeometryLayer([string]$layer)
{
    Write-Output "geometry layer: enter, into $layer"
    if (!$layer) { throw 'Build-GeometryLayer needs the payload layer folder' }
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (!$cmake) { throw 'CMake (3.24 or later) not found' }
    $build = Join-Path $OutputRoot 'xgstereo-build'
    & $cmake.Source -S (Join-Path $projectRoot 'bench\native\xgstereo') -B $build -A x64 -DVRX_XG_VALIDATE_SPIRV=OFF
    if ($LASTEXITCODE) { throw 'Geometry layer configure failed' }
    & $cmake.Source --build $build --config Release --target vrx_xgstereo
    if ($LASTEXITCODE) { throw 'Geometry layer build failed' }
    New-Item -ItemType Directory -Path $layer -Force | Out-Null
    foreach ($name in 'vrx_xgstereo.dll', 'VkLayer_vrx_xgstereo.json')
    {
        $built = Join-Path $build "Release\$name"
        if (!(Test-Path -LiteralPath $built)) { throw "Geometry layer build did not produce $name" }
        Copy-Item -LiteralPath $built -Destination $layer
    }
    $manifest = Get-Content -LiteralPath (Join-Path $layer 'VkLayer_vrx_xgstereo.json') -Raw | ConvertFrom-Json
    if ($manifest.layer.library_path -ne '.\vrx_xgstereo.dll') { throw "Unexpected layer library path $($manifest.layer.library_path)" }
    Write-Output "geometry layer: exit ok ($($manifest.layer.name), API $($manifest.layer.api_version))"
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
        # --no-diagnostics: the engine's test-only switches (--check-geometry...) are not compiled in.
        & cmd /c 'bench\native\openxr\build.bat --desktop --no-diagnostics'
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
        # The fixed Steam Launch Options entry point (statically linked; VRX copies it to
        # %LOCALAPPDATA%\VRX\launcher at start-up, see desktop\VRX.Desktop\SteamLauncher.cs).
        Copy-Item "$nativeOutput\vrx-steam-launch.exe" $payload
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
        # A release engine must not carry the test-only geometry switches.
        $diagnostics = & "$engine\xrplayer.exe" '--geometry-session=release-check' '--check-geometry' 2>&1 | Out-String
        if (!$LASTEXITCODE -or $diagnostics -notmatch 'unknown option --check-geometry') { throw "engine\xrplayer.exe accepts --check-geometry: built with diagnostics" }
        # Geometry stereo: VRX's Vulkan layer and the pinned DXVK and vkd3d-proton DLLs.
        $geometry = Join-Path $payload 'geometry'
        Build-GeometryLayer (Join-Path $geometry 'layer')
        Add-GeometryRuntime $geometry
        # Only vkd3d-proton's d3d12.dll ships: never the native DX12 proxy (bench\native\dx12stereo).
        $pins = Read-GeometryPins
        foreach ($d3d12 in Get-ChildItem -LiteralPath $payload -Recurse -File -Filter 'd3d12.dll')
        {
            $relative = [IO.Path]::GetRelativePath($payload, $d3d12.FullName)
            if ($relative -ne 'geometry\vkd3d\d3d12.dll' -or (Get-FileHash -LiteralPath $d3d12.FullName -Algorithm SHA256).Hash -ne $pins[$relative]) { throw "Unexpected $relative in the payload" }
        }
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
        foreach ($archive in $geometryArchives) {
            "geometry\$($archive.Folder) : $($archive.Name) ($($archive.Url), SHA-256 $($archive.Sha256.ToLowerInvariant()))" | Add-Content "$payload\BUILD.txt"
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
