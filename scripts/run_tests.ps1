$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$python = Get-Command py -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $python -or $python.Source -like "*Microsoft\WindowsApps*") {
    throw "未找到 Python 3.11 或更高版本。"
}

$previousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = Join-Path $projectRoot "src"
try {
    & $python.Source -m unittest discover -s (Join-Path $projectRoot "tests") -v
    exit $LASTEXITCODE
}
finally {
    $env:PYTHONPATH = $previousPythonPath
}
