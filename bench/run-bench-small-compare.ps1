$ErrorActionPreference = 'Continue'
$py = 'C:\Users\paulj\dev\VRX\bench\.venv\Scripts\python.exe'
$bench = 'C:\Users\paulj\dev\VRX\bench'
Set-Location $bench
$log = Join-Path $bench 'bench-small-cpu-vs-dml.txt'
"" | Set-Content $log
$m = 'models\onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx'

function Run($label, $pyx, $cmd) {
    Write-Output ("##### " + $label)
    & $pyx $bench\bench_depth.py @cmd 2>&1 | Tee-Object -FilePath $log -Append
    Write-Output ''
}

$dmlpy = 'C:\Users\paulj\dev\VRX\bench\.venv-dml\Scripts\python.exe'
foreach ($s in 140, 196, 224, 252) {
    Run ("CPU  size=$s") $py     @('--model', $m, '--ep', 'cpu', '--size', $s, '--iters', '200')
    Run ("DML  size=$s") $dmlpy  @('--model', $m, '--ep', 'dml', '--size', $s, '--iters', '200')
}
Write-Output "ALL DONE"
