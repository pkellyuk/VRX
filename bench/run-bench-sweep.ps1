$ErrorActionPreference = 'Continue'
$py = 'C:\Users\paulj\dev\VRX\bench\.venv-dml\Scripts\python.exe'
$bench = 'C:\Users\paulj\dev\VRX\bench'
Set-Location $bench
$log = Join-Path $bench 'bench-sweep-results.txt'
"" | Set-Content $log
$m = 'models\onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx'

function Run($label, $cmd) {
    Write-Output ("##### " + $label)
    & $py $bench\bench_depth.py @cmd 2>&1 | Tee-Object -FilePath $log -Append
    Write-Output ''
}

# Resolution sweep, fp16 / DML, inference-only + full pipeline at each size
foreach ($s in 518, 392, 252, 224, 196, 140) {
    Run ("fp16 / DML  size=$s" ) @('--model', $m, '--ep', 'dml', '--size', $s, '--full-pipeline')
}
Write-Output "ALL DONE"
