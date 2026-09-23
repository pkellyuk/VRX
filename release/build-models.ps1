# Builds the two depth models VRX ships from their upstream sources, pinned, and checks
# each result against the SHA-256 of the model in the tested releases. The exports are
# deterministic, so a mismatch means an input or tool changed and the build stops.
#   Depth Anything V2 Small: onnx-community's FP16 export, given fixed shapes (make_fixed_shape.py)
#   ZipDepth-base:           the authors' checkpoint, exported by zipdepth_export.py
# Existing models whose hash already matches are kept. Needs Python 3.12 (py launcher or PATH) and git.
param([string]$Python = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$models = Join-Path $projectRoot 'bench\models'

$daRevision = '4472b7362082ad9968fee890ca0f1e5aca36b93d'     # huggingface.co/onnx-community/depth-anything-v2-small
$daSourceHash = '2df6223f206b5164e21f664ace61dabeb9bb6a49b8b5a3e00510b4807d0f5b04'
$zdCommit = '91f3fd21e131641f51e8d35736d1958350180e3a'       # github.com/fabiotosi92/ZipDepth
$zdCheckpointHash = 'a55910bb0b99c8c5e641cb9206e810b269690ad94e8a2ef08c827c4679391a65'
$outputs = [ordered]@{
    'model_fixed_686x392.onnx'             = '849995e5cd40d3de8df198b8b7616a0bdfd8cea917e0c8a66a68d2de8d924671'
    'zipdepth_faithful_fp16_672x384.onnx' = '614925332b4f4ade6460279609b94a93eeae42fc1cd4be29283b0b51334acf61'
}

function Get-Sha256([string]$Path)
{
    if (!(Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-Hash([string]$Path, [string]$Expected)
{
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected) { throw "$Path has SHA-256 '$actual', expected $Expected" }
    Write-Output "ok  $Expected  $(Split-Path $Path -Leaf)"
}

Write-Output "build-models: enter, models in $models"
New-Item -ItemType Directory -Path $models -Force | Out-Null
$missing = @($outputs.Keys | Where-Object { (Get-Sha256 (Join-Path $models $_)) -ne $outputs[$_] })
if ($missing.Count -eq 0)
{
    Write-Output 'build-models: both models already present with the expected hashes'
    return
}
Write-Output "build-models: to build: $($missing -join ', ')"

# Python 3.12 with the pinned export tools, in the git-ignored venv the export notes use.
$venv = Join-Path $projectRoot 'bench\.venv-zipdepth'
$venvPython = Join-Path $venv 'Scripts\python.exe'
if (!(Test-Path -LiteralPath $venvPython))
{
    if ($Python) { & $Python -m venv $venv }
    elseif (Get-Command py -ErrorAction SilentlyContinue) { & py -3.12 -m venv $venv }
    else { & python -m venv $venv }
    if ($LASTEXITCODE) { throw 'Could not create the Python venv' }
}
& $venvPython -m pip install --quiet --disable-pip-version-check -r (Join-Path $PSScriptRoot 'models-requirements.txt')
if ($LASTEXITCODE) { throw 'pip install of the pinned export tools failed' }
$env:PYTHONUTF8 = '1'

if ($missing -contains 'model_fixed_686x392.onnx')
{
    $source = Join-Path $models 'onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx'
    if ((Get-Sha256 $source) -ne $daSourceHash)
    {
        New-Item -ItemType Directory -Path (Split-Path $source) -Force | Out-Null
        $url = "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/$daRevision/onnx/model_fp16.onnx"
        Write-Output "build-models: downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $source -UseBasicParsing
    }
    Assert-Hash $source $daSourceHash
    & $venvPython (Join-Path $projectRoot 'bench\make_fixed_shape.py') $source (Join-Path $models 'model_fixed_686x392.onnx') 392 686
    if ($LASTEXITCODE) { throw 'make_fixed_shape.py failed' }
}

if ($missing -contains 'zipdepth_faithful_fp16_672x384.onnx')
{
    $repo = Join-Path $models 'zipdepth'
    if (!(Test-Path -LiteralPath (Join-Path $repo '.git')))
    {
        & git init --quiet $repo
        & git -C $repo remote add origin https://github.com/fabiotosi92/ZipDepth
    }
    & git -C $repo fetch --quiet --depth 1 origin $zdCommit
    if ($LASTEXITCODE) { throw "Could not fetch ZipDepth $zdCommit" }
    & git -C $repo checkout --quiet --force $zdCommit
    if ($LASTEXITCODE) { throw "Could not check out ZipDepth $zdCommit" }
    $checkpoint = Join-Path $repo 'checkpoints\zipdepth_base.pth'
    Assert-Hash $checkpoint $zdCheckpointHash
    & $venvPython (Join-Path $projectRoot 'bench\zipdepth_export.py') $checkpoint (Join-Path $models 'zipdepth_faithful_fp16_672x384.onnx') 384 672 --fp16
    if ($LASTEXITCODE) { throw 'zipdepth_export.py failed' }
}

foreach ($name in $outputs.Keys) { Assert-Hash (Join-Path $models $name) $outputs[$name] }
Write-Output 'build-models: exit, both models built and verified'
