$ErrorActionPreference = 'Continue'
$refs = 'C:\Users\paulj\dev\VRX\refs'

# 1) OpenXR 1.1 spec (24MB)
try {
    Invoke-WebRequest -Uri 'https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html' -OutFile (Join-Path $refs 'openxr-spec.html') -UseBasicParsing
    Write-Output ('spec: ' + (Get-Item (Join-Path $refs 'openxr-spec.html')).Length)
} catch { Write-Output ('spec FAIL: ' + $_.Exception.Message) }

# 2) OpenXR header
try {
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/main/include/openxr/openxr.h' -OutFile (Join-Path $refs 'openxr.h') -UseBasicParsing
    Write-Output ('header: ' + (Get-Item (Join-Path $refs 'openxr.h')).Length)
} catch { Write-Output ('header FAIL: ' + $_.Exception.Message) }

# 3) ORT CUDA page (has full EP nav)
try {
    Invoke-WebRequest -Uri 'https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html' -OutFile (Join-Path $refs 'ort-cuda.html') -UseBasicParsing
    $ort = Select-String -Path (Join-Path $refs 'ort-cuda.html') -Pattern 'execution-providers/[A-Za-z0-9/_-]+\.html' -AllMatches
    $ort.Matches.Value | Sort-Object -Unique | ForEach-Object { Write-Output $_ }
} catch { Write-Output ('ort FAIL: ' + $_.Exception.Message) }
