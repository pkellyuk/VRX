$ErrorActionPreference = 'Continue'
$bench = 'C:\Users\paulj\dev\VRX\bench'
New-Item -ItemType Directory -Force -Path $bench | Out-Null

# venv
$py = 'C:\Users\paulj\dev\VRX\bench\.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    python -m venv (Join-Path $bench '.venv')
}
& $py -m pip install --quiet --upgrade pip
& $py -m pip install --quiet onnxruntime-gpu onnx numpy pillow
& $py -c "import onnxruntime as ort; print('ORT version:', ort.__version__); print('providers:', ort.get_available_providers())"

# TensorRT-RTX EP docs: download and strip to text
Invoke-WebRequest -Uri 'https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html' -OutFile C:\Users\paulj\dev\VRX\refs\ort-trtx.html -UseBasicParsing
$html = Get-Content -Raw -LiteralPath C:\Users\paulj\dev\VRX\refs\ort-trtx.html
# keep only the main content div
$idx = $html.IndexOf('<div class="main-content"')
if ($idx -gt 0) { $html = $html.Substring($idx) }
$text = $html -replace '<script[\s\S]*?</script>', ' ' -replace '<style[\s\S]*?</style>', ' ' -replace '<[^>]+>', ' ' -replace '&amp;', '&' -replace '&#8201;', ' ' -replace '\s+', ' '
Set-Content -Path C:\Users\paulj\dev\VRX\refs\ort-trtx.txt -Value $text -Encoding UTF8
Write-Output ('trtx text chars: ' + $text.Length)
