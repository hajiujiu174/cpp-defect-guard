[CmdletBinding()]
param([string]$OutputDirectory = "", [int]$Epochs = 40, [switch]$Ablations, [switch]$RunTests)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$savedPath = $env:PATH
$savedPythonPath = $env:PYTHONPATH
$savedEncoding = $env:PYTHONIOENCODING
$savedNativeRequirement = $env:DEFECTGUARD_REQUIRE_NATIVE
$savedMLRequirement = $env:DEFECTGUARD_REQUIRE_ML
try {
    $env:PATH = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
        [Environment]::GetEnvironmentVariable("Path", "User") + ";" + $savedPath
    $env:PYTHONPATH = Join-Path $projectRoot "src"
    $env:PYTHONIOENCODING = "utf-8"
    $python = Join-Path $projectRoot ".venv-ml\Scripts\python.exe"
    $native = Join-Path $projectRoot "native\clang_tool\build\defectguard-clang.exe"
    $encoder = Join-Path $projectRoot "artifacts\models\graphcodebert-base"
    if (-not (Test-Path -LiteralPath $python)) { throw "请先运行 scripts/setup_ml.ps1 -DownloadEncoder" }
    if (-not (Test-Path -LiteralPath $native)) { throw "请先运行 scripts/run_native_demo.ps1 构建原生工具" }
    if (-not (Test-Path -LiteralPath (Join-Path $encoder "config.json"))) { throw "请先下载 GraphCodeBERT 编码器" }
    $env:PATH = (Split-Path -Parent $native) + ";" + $env:PATH
    Get-Command cmake, clang++, mingw32-make -ErrorAction Stop | Out-Null
    if (-not $OutputDirectory) {
        $OutputDirectory = Join-Path $projectRoot ("artifacts\layer2\demo-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    }
    $output = [IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path -LiteralPath $output) { throw "为保留实验记录，请选择尚不存在的输出目录。" }
    $corpus = Join-Path $projectRoot "samples\ml_corpus"
    $build = Join-Path $corpus "build"
    cmake -S $corpus -B $build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    if ($LASTEXITCODE -ne 0) { throw "语料编译数据库生成失败" }
    cmake --build $build --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "语料编译失败" }
    & $python -m defectguard export-graphs $corpus --config (Join-Path $projectRoot "config\cmake-demo.toml") --output (Join-Path $output "graphs")
    if ($LASTEXITCODE -ne 0) { throw "图数据导出失败" }
    $dataset = Join-Path $output "dataset"
    & $python -m defectguard prepare-dataset --graphs (Join-Path $output "graphs\graphs.jsonl") --labels (Join-Path $corpus "labels.jsonl") --output $dataset --seed 42
    if ($LASTEXITCODE -ne 0) { throw "标签校验或数据划分失败" }
    foreach ($mode in @("sequence", "gnn", "fusion")) {
        & $python -m defectguard train-model --dataset $dataset --output (Join-Path $output $mode) --mode $mode --encoder $encoder --epochs $Epochs --seed 42 --cache (Join-Path $projectRoot "artifacts\layer2\embedding-cache")
        if ($LASTEXITCODE -ne 0) { throw "模型训练失败：$mode" }
    }
    if ($Ablations) {
        foreach ($edge in @("cfg", "dfg")) {
            & $python -m defectguard train-model --dataset $dataset --output (Join-Path $output "fusion-no-$edge") --mode fusion --encoder $encoder --epochs $Epochs --seed 42 --without $edge --cache (Join-Path $projectRoot "artifacts\layer2\embedding-cache")
            if ($LASTEXITCODE -ne 0) { throw "消融实验失败：$edge" }
        }
    }
    & $python -m defectguard evaluate-model --dataset $dataset --checkpoint (Join-Path $output "fusion") --encoder $encoder --output (Join-Path $output "reloaded-test")
    if ($LASTEXITCODE -ne 0) { throw "融合检查点重载评估失败" }
    & $python -m defectguard.experiments.demo --root $output --encoder $encoder --native-tool $native
    if ($LASTEXITCODE -ne 0) { throw "实验汇总失败" }
    & $python -m defectguard scan $corpus --config (Join-Path $output "scan-model.toml") --output (Join-Path $output "model-scan")
    if ($LASTEXITCODE -ne 0) { throw "模型扫描接入失败" }
    & $python -m defectguard.experiments.demo --root $output --audit
    if ($LASTEXITCODE -ne 0) { throw "产物一致性验收失败" }
    if ($RunTests) {
        $env:DEFECTGUARD_REQUIRE_NATIVE = "1"
        $env:DEFECTGUARD_REQUIRE_ML = "1"
        & $python -m unittest discover -s (Join-Path $projectRoot "tests") -v
        if ($LASTEXITCODE -ne 0) { throw "自动化回归测试失败" }
    }
    Write-Host "第二层演示完成：$output"
    Write-Host "结果对比：$(Join-Path $output 'comparison.md')"
    Write-Host "这些小样本结果仅验证流程，不代表真实项目精度。"
} finally {
    $env:PATH = $savedPath
    $env:PYTHONPATH = $savedPythonPath
    $env:PYTHONIOENCODING = $savedEncoding
    $env:DEFECTGUARD_REQUIRE_NATIVE = $savedNativeRequirement
    $env:DEFECTGUARD_REQUIRE_ML = $savedMLRequirement
}
