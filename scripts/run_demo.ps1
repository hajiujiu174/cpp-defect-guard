[CmdletBinding()]
param(
    [string]$OutputDirectory = "",
    [switch]$SkipVerify
)

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$python = Get-Command py -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $python -or $python.Source -like "*Microsoft\WindowsApps*") {
    throw "未找到 Python 3.11 或更高版本。"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "artifacts\layer1"
}

$previousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = Join-Path $projectRoot "src"
try {
    $config = Join-Path $projectRoot "config\default.toml"
    $arguments = @(
        "-m", "defectguard", "scan",
        (Join-Path $projectRoot "samples\vulnerable"),
        "--config", $config,
        "--output", $OutputDirectory
    )
    if (-not $SkipVerify) {
        $arguments[5] = Join-Path $projectRoot "config\gcc-demo.toml"
        $arguments += "--verify"
    }
    & $python.Source @arguments
    exit $LASTEXITCODE
}
finally {
    $env:PYTHONPATH = $previousPythonPath
}
