[CmdletBinding()]
param([switch]$RunTests)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$savedPath = $env:PATH
$savedPythonPath = $env:PYTHONPATH
$savedPythonEncoding = $env:PYTHONIOENCODING
$savedNativeRequirement = $env:DEFECTGUARD_REQUIRE_NATIVE
try {
    $env:PATH = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
        [Environment]::GetEnvironmentVariable("Path", "User") + ";" + $savedPath
    $env:PYTHONPATH = Join-Path $projectRoot "src"
    $env:PYTHONIOENCODING = "utf-8"
    $python = Get-Command py -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
    if (-not $python -or $python.Source -like "*Microsoft\WindowsApps*") {
        throw "未找到可用的 Python 3.11 或更高版本。"
    }
    Get-Command cmake, clang, clang++, llvm-config, mingw32-make -ErrorAction Stop | Out-Null
    $llvmPrefix = (llvm-config --prefix).Trim()
    $llvmCmakeDir = (llvm-config --cmakedir).Trim()
    $nativeSource = Join-Path $projectRoot "native\clang_tool"
    $nativeBuild = Join-Path $nativeSource "build"
    cmake -S $nativeSource -B $nativeBuild -G "MinGW Makefiles" `
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        "-DCMAKE_PREFIX_PATH=$llvmPrefix" "-DLLVM_DIR=$llvmCmakeDir" `
        "-DClang_DIR=$llvmPrefix/lib/cmake/clang"
    if ($LASTEXITCODE -ne 0) { throw "原生分析器配置失败" }
    cmake --build $nativeBuild --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "原生分析器构建失败" }
    $env:PATH = $nativeBuild + ";" + $env:PATH

    foreach ($sample in @("vulnerable", "fixed")) {
        $source = Join-Path $projectRoot "samples\$sample"
        $output = Join-Path $projectRoot "artifacts\native\$sample"
        cmake -S $source -B (Join-Path $source "build") -G "MinGW Makefiles" `
            -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        if ($LASTEXITCODE -ne 0) { throw "样例编译数据库生成失败：$sample" }
        & $python.Source -m defectguard scan $source `
            --config (Join-Path $projectRoot "config\cmake-demo.toml") --verify --output $output
        if ($LASTEXITCODE -ne 0) { throw "原生扫描或隔离验证失败：$sample" }
        & $python.Source -m defectguard export-graphs $source `
            --config (Join-Path $projectRoot "config\cmake-demo.toml") --output (Join-Path $output "graphs")
        if ($LASTEXITCODE -ne 0) { throw "图数据导出失败：$sample" }
    }
    if ($RunTests) {
        $env:DEFECTGUARD_REQUIRE_NATIVE = "1"
        & $python.Source -m unittest discover -s (Join-Path $projectRoot "tests") -v
        if ($LASTEXITCODE -ne 0) { throw "自动化测试失败" }
    }
}
finally {
    $env:PATH = $savedPath
    $env:PYTHONPATH = $savedPythonPath
    $env:PYTHONIOENCODING = $savedPythonEncoding
    $env:DEFECTGUARD_REQUIRE_NATIVE = $savedNativeRequirement
}
