[CmdletBinding()]
param([string]$Python = "", [switch]$DownloadEncoder)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$savedCache = $env:UV_CACHE_DIR
$savedPythonPath = $env:PYTHONPATH
$savedEncoding = $env:PYTHONIOENCODING
try {
    $uv = Get-Command uv -ErrorAction SilentlyContinue
    if (-not $Python) {
        if ($uv) { $Python = (& $uv.Source python find 3.12).Trim() }
        else { $Python = (& py -3.12 -c "import sys; print(sys.executable)").Trim() }
        if ($LASTEXITCODE -ne 0) { throw "请用 -Python 指定现有 Python 3.12 或更高版本的完整路径。" }
    }
    & $Python -c "import sys; assert sys.version_info >= (3, 12), 'ML environment requires Python >= 3.12'"
    if ($LASTEXITCODE -ne 0) { throw "模型环境要求 Python 3.12 或更高版本。" }
    $environmentRoot = Join-Path $projectRoot ".venv-ml"
    $environmentPython = Join-Path $environmentRoot "Scripts\python.exe"
    if (-not (Test-Path -LiteralPath $environmentPython)) {
        if (Test-Path -LiteralPath $environmentRoot) { throw ".venv-ml 已存在但不是有效环境；不覆盖该目录。" }
        & $Python -m venv $environmentRoot
        if ($LASTEXITCODE -ne 0) { throw "创建独立环境失败" }
    }
    $env:UV_CACHE_DIR = Join-Path $projectRoot "artifacts\dependency-cache"
    if ($uv) {
        & $uv.Source pip install --python $environmentPython torch==2.14.0 --index-url https://download.pytorch.org/whl/cpu
        if ($LASTEXITCODE -ne 0) { throw "CPU PyTorch 安装失败" }
        & $uv.Source pip install --python $environmentPython -r (Join-Path $projectRoot "requirements-ml.txt")
    } else {
        & $environmentPython -m pip install torch==2.14.0 --index-url https://download.pytorch.org/whl/cpu
        if ($LASTEXITCODE -ne 0) { throw "CPU PyTorch 安装失败" }
        & $environmentPython -m pip install -r (Join-Path $projectRoot "requirements-ml.txt")
    }
    if ($LASTEXITCODE -ne 0) { throw "模型依赖安装失败" }
    & $environmentPython -c "import torch, transformers, torch_geometric; print(torch.__version__, transformers.__version__, torch_geometric.__version__)"
    if ($LASTEXITCODE -ne 0) { throw "依赖导入验证失败" }
    if ($DownloadEncoder) {
        $env:PYTHONPATH = Join-Path $projectRoot "src"
        $env:PYTHONIOENCODING = "utf-8"
        & $environmentPython -m defectguard download-encoder --output (Join-Path $projectRoot "artifacts\models\graphcodebert-base")
        if ($LASTEXITCODE -ne 0) { throw "编码器下载失败" }
    }
    Write-Host "独立模型环境：$environmentPython"
} finally {
    $env:UV_CACHE_DIR = $savedCache
    $env:PYTHONPATH = $savedPythonPath
    $env:PYTHONIOENCODING = $savedEncoding
}
