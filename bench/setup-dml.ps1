$ErrorActionPreference = 'Continue'
$bench = 'C:\Users\paulj\dev\VRX\bench'
$venv = Join-Path $bench '.venv-dml'
$py = Join-Path $venv 'Scripts\python.exe'
if (-not (Test-Path $py)) {
    python -m venv $venv
}
& $py -m pip install --quiet --upgrade pip
& $py -m pip install --quiet onnxruntime-directml onnx numpy pillow
& $py -c "import onnxruntime as ort; print('ORT', ort.__version__); print('providers:', ort.get_available_providers())"
