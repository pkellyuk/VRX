$ErrorActionPreference = 'Continue'
$py = 'C:\Users\paulj\dev\VRX\bench\.venv-dml\Scripts\python.exe'
$bench = 'C:\Users\paulj\dev\VRX\bench'
Set-Location $bench
$log = Join-Path $bench 'bench-dml-results.txt'
"" | Set-Content $log
$m = 'models\onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx'
$mi = 'models\onnx-community-depth-anything-v2-small\onnx\model_int8.onnx'

function Run($label, $cmd) {
    Write-Output ("##### " + $label)
    & $py $bench\bench_depth.py @cmd 2>&1 | Tee-Object -FilePath $log -Append
    Write-Output ''
}

Run "fp16 / DML EP (RTX 3090)" @('--model', $m,  '--ep', 'dml', '--full-pipeline')
Run "int8 / DML EP (RTX 3090)"  @('--model', $mi, '--ep', 'dml', '--full-pipeline')
Write-Output "ALL DONE"
