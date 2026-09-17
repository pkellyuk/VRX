$ErrorActionPreference = 'Continue'
$models = 'C:\Users\paulj\dev\VRX\bench\models\onnx-community-depth-anything-v2-small'
New-Item -ItemType Directory -Force -Path (Join-Path $models 'onnx') | Out-Null
$base = 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main'
$files = @('onnx/model_fp16.onnx', 'onnx/model_int8.onnx', 'config.json', 'preprocessor_config.json')
foreach ($f in $files) {
    $dest = Join-Path $models ($f -replace '/', '\')
    if (-not (Test-Path $dest)) {
        Write-Output "downloading $f"
        Invoke-WebRequest -Uri "$base/$f" -OutFile $dest -UseBasicParsing
    }
}
Get-ChildItem -Recurse $models | ForEach-Object { Write-Output ($_.Name + ' ' + [math]::Round($_.Length / 1MB, 2) + ' MB') }
