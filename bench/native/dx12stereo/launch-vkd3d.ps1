#requires -Version 7.0
<#
.SYNOPSIS
Launch an x64 D3D12 application through vkd3d-proton, optionally with VRX Vulkan stereo.
.DESCRIPTION
Use locally built vkd3d-proton DLLs and DXVK's x64 dxgi.dll/d3d11.dll. Installation is
temporary and refuses existing files. The launcher must remain running until
the application exits so it can remove its unchanged copies.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$D3d12Dll,
    [Parameter(Mandatory)][string]$D3d12CoreDll,
    [Parameter(Mandatory)][string]$DxgiDll,
    [string]$D3d11Dll,
    [ValidateSet('none','mirror')][string]$Stereo = 'none',
    [ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Session = 'vkd3d',
    [ValidateRange(0,64)][float]$Shift = 0.008,
    [ValidateRange(0,0.25)][float]$Convergence = 0,
    [ValidatePattern('^[0-9]+$')][string]$SteamAppId,
    [string]$LayerDirectory = (Join-Path $PSScriptRoot '../xgstereo/out/Release'),
    [string[]]$GameArguments = @('-dx12','-norhithread','-windowed','-ResX=1280','-ResY=720'),
    [switch]$Preview
)
$ErrorActionPreference = 'Stop'
if ($Stereo -eq 'mirror' -and $Shift -le 0) { throw 'The current Vulkan mirror layer requires a positive stereo shift.' }
$application = (Resolve-Path -LiteralPath $Executable).Path
$gameRoot = [IO.Path]::GetDirectoryName($application)
if (-not $D3d11Dll) { $D3d11Dll = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $DxgiDll).Path) 'd3d11.dll' }
$sources = [ordered]@{
    'd3d12.dll' = (Resolve-Path -LiteralPath $D3d12Dll).Path
    'd3d12core.dll' = (Resolve-Path -LiteralPath $D3d12CoreDll).Path
    'dxgi.dll' = (Resolve-Path -LiteralPath $DxgiDll).Path
    # Games can create auxiliary D3D11 devices for movies or middleware even
    # while their main renderer uses D3D12. Keep those compatible with DXVK DXGI.
    'd3d11.dll' = (Resolve-Path -LiteralPath $D3d11Dll).Path
}
foreach ($name in $sources.Keys) {
    if (-not (Test-Path -LiteralPath $sources[$name] -PathType Leaf)) { throw "Missing DLL: $name" }
    $target = Join-Path $gameRoot $name
    if (Test-Path -LiteralPath $target) { throw "Refusing to replace an existing file: $target" }
}
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($application)) -ErrorAction SilentlyContinue) {
    throw 'Close the application before launching translation.'
}
$layerRoot = $null
if ($Stereo -eq 'mirror') {
    $layerRoot = (Resolve-Path -LiteralPath $LayerDirectory).Path
    foreach ($name in @('VkLayer_vrx_xgstereo.json','vrx_xgstereo.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $layerRoot $name) -PathType Leaf)) { throw "Missing Vulkan layer file: $name" }
    }
}
if ($Preview) {
    [pscustomobject]@{Executable=$application;Sources=$sources;Stereo=$Stereo;Session=$Session;LayerDirectory=$layerRoot;Arguments=$GameArguments}
    return
}
$runRoot = Join-Path $PSScriptRoot ('out/vkd3d-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runRoot
$installed = @()
$result = 1
try {
    foreach ($name in $sources.Keys) {
        $source = $sources[$name]
        $target = Join-Path $gameRoot $name
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $inputFile = [IO.File]::OpenRead($source)
        try {
            $outputFile = [IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
            $installed += [pscustomobject]@{Path=$target;SHA256=$hash}
            $installed | ConvertTo-Json -AsArray | Set-Content -LiteralPath (Join-Path $runRoot 'installed.json')
            try { $inputFile.CopyTo($outputFile) } finally { $outputFile.Dispose() }
        } finally { $inputFile.Dispose() }
    }
    $start = [Diagnostics.ProcessStartInfo]::new($application)
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $gameRoot
    foreach ($argument in $GameArguments) { $start.ArgumentList.Add($argument) }
    # Do not combine the native D3D12 interceptor with the translation test.
    foreach ($key in @($start.Environment.Keys)) {
        if ($key -like 'VRX_DX12_*' -or $key -like 'VRX_XG_*') { $null = $start.Environment.Remove($key) }
    }
    $layers = @($start.Environment['VK_INSTANCE_LAYERS'] -split ';' | Where-Object { $_ -and $_ -ne 'VK_LAYER_VRX_xgstereo' })
    if ($Stereo -eq 'mirror') {
        $extraPaths = $start.Environment['VK_ADD_LAYER_PATH']
        $start.Environment['VK_ADD_LAYER_PATH'] = if ($extraPaths) { "$layerRoot;$extraPaths" } else { $layerRoot }
        $layers += 'VK_LAYER_VRX_xgstereo'
        $start.Environment['VRX_XG_EYE'] = 'mirror'
        $start.Environment['VRX_XG_TRANSPORT'] = 'gpu'
        $start.Environment['VRX_XG_SESSION'] = $Session
        $start.Environment['VRX_XG_SHIFT'] = $Shift.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
        $start.Environment['VRX_XG_LOG'] = Join-Path $runRoot 'layer.log'
        $tuningPath = Join-Path $runRoot 'stereo.txt'
        ($Shift.ToString('R',[Globalization.CultureInfo]::InvariantCulture) + ' ' + $Convergence.ToString('R',[Globalization.CultureInfo]::InvariantCulture)) | Set-Content -LiteralPath $tuningPath
        $start.Environment['VRX_XG_TUNING'] = $tuningPath
        # Opaque descriptor buffers are not supported by the current stereo layer.
        $disabled = @($start.Environment['VKD3D_DISABLE_EXTENSIONS'] -split '[,;]' | Where-Object { $_ })
        $start.Environment['VKD3D_DISABLE_EXTENSIONS'] = (@($disabled + @('VK_EXT_descriptor_buffer','VK_EXT_descriptor_heap','VK_NV_low_latency2',
            'VK_EXT_shader_module_identifier','VK_KHR_pipeline_binary','VK_EXT_graphics_pipeline_library')) | Select-Object -Unique) -join ','
        # The current layer requires exactly one graphics queue. NVIDIA latency
        # support otherwise requests an additional out-of-band graphics queue.
        $config = @($start.Environment['VKD3D_CONFIG'] -split '[,;]' | Where-Object { $_ })
        $start.Environment['VKD3D_CONFIG'] = (@($config + @('single_queue')) | Select-Object -Unique) -join ','
    }
    $start.Environment['VK_INSTANCE_LAYERS'] = $layers -join ';'
    $start.Environment['VKD3D_DEBUG'] = 'info'
    $start.Environment['VKD3D_LOG_FILE'] = Join-Path $runRoot 'vkd3d.log'
    $start.Environment['VKD3D_SHADER_CACHE_PATH'] = $runRoot
    $start.Environment['DXVK_LOG_PATH'] = $runRoot
    $start.Environment['DXVK_STATE_CACHE_PATH'] = $runRoot
    $configPath = Join-Path $runRoot 'dxvk.conf'
    @('dxvk.enableGraphicsPipelineLibrary = False','dxvk.enableDescriptorBuffer = False','dxvk.disableNvLowLatency2 = True') | Set-Content -LiteralPath $configPath
    $start.Environment['DXVK_CONFIG_FILE'] = $configPath
    if ($SteamAppId) {
        $start.Environment['SteamAppId'] = $SteamAppId
        $start.Environment['SteamGameId'] = $SteamAppId
    }
    $child = [Diagnostics.Process]::Start($start)
    if (-not $child) { throw 'Application did not start.' }
    try {
        [pscustomobject]@{Pid=$child.Id;StartTicks=$child.StartTime.ToUniversalTime().Ticks;Executable=$application;Session=$Session;Stereo=$Stereo;Root=$runRoot} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'run.json')
        Write-Output "VKD3D game PID $($child.Id); session $Session; run $runRoot"
        $child.WaitForExit()
        $result = $child.ExitCode
        $result | Set-Content -LiteralPath (Join-Path $runRoot 'exit-code.txt')
    } finally { $child.Dispose() }
} finally {
    foreach ($record in $installed) {
        if (Test-Path -LiteralPath $record.Path) {
            if ((Get-FileHash -LiteralPath $record.Path -Algorithm SHA256).Hash -eq $record.SHA256) { Remove-Item -LiteralPath $record.Path }
            else { Write-Warning "Leaving a changed file in place: $($record.Path)" }
        }
    }
}
exit $result
