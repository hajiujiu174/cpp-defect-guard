[CmdletBinding()]
param(
    [switch]$Gui,
    [switch]$Analysis,
    [switch]$RunTests,
    [string]$Project = "",
    [string]$Database = "",
    [string]$CompileCommands = "",
    [ValidateRange(0,64)][int]$Threads = 0
)
$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$preset = if ($Gui) { "desktop-debug" } else { "core-debug" }
$buildName = if ($Gui) { "codeguard-desktop" } else { "codeguard-core" }
if ($Analysis -or $CompileCommands) { $preset = "analysis-release"; $buildName = "codeguard-analysis" }
Push-Location $projectRoot
try {
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CodeGuard CMake 配置失败。请检查 SQLite/Qt 开发包与编译器。" }
    cmake --build --preset $preset --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "CodeGuard 编译失败" }
    if ($RunTests) {
        ctest --preset $preset
        if ($LASTEXITCODE -ne 0) { throw "CodeGuard 测试失败" }
    }
    if ($Gui) {
        $arguments = @()
        if ($Database) { $arguments = @("--database", $Database) }
        if ($CompileCommands) { $arguments += @("--compile-commands", $CompileCommands) }
        if ($Project) { $arguments += @("--project", $Project) }
        & "$projectRoot\build\$buildName\gui\qt\codeguard-gui.exe" @arguments
        if ($LASTEXITCODE -ne 0) { throw "CodeGuard GUI 退出异常" }
    } elseif ($Project) {
        if (-not $Database) { throw "扫描须显式指定源码目录外的 -Database 路径。" }
        $scanArguments = @("scan", $Project, "--database", $Database)
        $scanArguments += @("--threads", "$Threads")
        if ($CompileCommands) { $scanArguments += @("--compile-commands", $CompileCommands) }
        & "$projectRoot\build\$buildName\codeguard-cli.exe" @scanArguments
        if ($LASTEXITCODE -ne 0) { throw "CodeGuard 扫描失败或不完整，检查诊断。" }
    }
} finally { Pop-Location }
