$ErrorActionPreference = 'Continue'
$py = 'C:\Users\paulj\dev\VRX\bench\.venv\Scripts\python.exe'
$bench = 'C:\Users\paulj\dev\VRX\bench'
Set-Location $bench
$log = Join-Path $bench 'bench-results.txt'
"" | Set-Content $log
$m = 'models\onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx'
$mi = 'models\onnx-community-depth-anything-v2-small\onnx\model_int8.onnx'

function Run($label, $cmd) {
    Write-Output ("##### " + $label)
    & $py $bench\bench_depth.py @cmd 2>&1 | Tee-Object -FilePath $log -Append
    Write-Output ''
}

Run "fp16 / CUDA EP"     @('--model', $m,  '--ep', 'cuda', '--full-pipeline')
Run "int8 / CUDA EP"     @('--model', $mi, '--ep', 'cuda', '--full-pipeline')
Run "fp16 / CPU EP"      @('--model', $m,  '--ep', 'cpu',  '--iters', '50')
Write-Output "ALL DONE"
