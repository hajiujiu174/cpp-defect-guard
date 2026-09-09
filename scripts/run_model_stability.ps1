[CmdletBinding()]
param(
    [string]$Dataset = "",
    [string]$OutputDirectory = "",
    [int[]]$Seeds = @(42, 43, 44),
    [int]$Epochs = 40,
    [switch]$Resume,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$savedPath = $env:PATH
$savedPythonPath = $env:PYTHONPATH
$savedEncoding = $env:PYTHONIOENCODING
$savedNative = $env:DEFECTGUARD_REQUIRE_NATIVE
$savedML = $env:DEFECTGUARD_REQUIRE_ML
try {
    $python = Join-Path $projectRoot ".venv-ml\Scripts\python.exe"
    if (-not (Test-Path -LiteralPath $python)) { throw "请先准备 .venv-ml 模型环境。" }
    if (-not $Dataset) { $Dataset = Join-Path $projectRoot "artifacts\layer2\demo-20260906\dataset" }
    if (-not (Test-Path -LiteralPath (Join-Path $Dataset "splits.json"))) {
        throw "未找到数据集。请先准备独立标注的数据集，再用 -Dataset 指定目录；本脚本不会自动重划分数据。"
    }
    if ($Resume -and -not $OutputDirectory) { throw "恢复实验须用 -OutputDirectory 明确指定原实验目录。" }
    if (-not $OutputDirectory) {
        $OutputDirectory = Join-Path $projectRoot ("artifacts\layer2\stability-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    }
    $env:PATH = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
        [Environment]::GetEnvironmentVariable("Path", "User") + ";" + $savedPath
    $env:PYTHONPATH = Join-Path $projectRoot "src"
    $env:PYTHONIOENCODING = "utf-8"
    $sweepArguments = @("-m", "defectguard", "sweep-models", "--dataset", $Dataset,
        "--output", $OutputDirectory, "--encoder", (Join-Path $projectRoot "artifacts\models\graphcodebert-base"),
        "--epochs", "$Epochs", "--cache", (Join-Path $projectRoot "artifacts\layer2\stability-embedding-cache"), "--seeds")
    foreach ($seed in $Seeds) { $sweepArguments += "$seed" }
    if ($Resume) { $sweepArguments += "--resume" }
    & $python @sweepArguments
    if ($LASTEXITCODE -ne 0) { throw "实验失败，已完成的运行记录会保留。请检查 progress.json。" }
    if ($RunTests) {
        $env:DEFECTGUARD_REQUIRE_NATIVE = "1"
        $env:DEFECTGUARD_REQUIRE_ML = "1"
        & $python -m unittest discover -s (Join-Path $projectRoot "tests") -v
        if ($LASTEXITCODE -ne 0) { throw "自动化回归测试失败" }
    }
    Write-Host "实验汇总：$(Join-Path $OutputDirectory 'summary.md')"
    Write-Host "随机种子并非新的独立样本；均值和标准差不代表真实项目精度。"
} finally {
    $env:PATH = $savedPath
    $env:PYTHONPATH = $savedPythonPath
    $env:PYTHONIOENCODING = $savedEncoding
    $env:DEFECTGUARD_REQUIRE_NATIVE = $savedNative
    $env:DEFECTGUARD_REQUIRE_ML = $savedML
}
