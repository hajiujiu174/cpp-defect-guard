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
    $cases = @(
        @{
            Name = "vulnerable"
            Source = Join-Path $projectRoot "samples\vulnerable"
            Config = Join-Path $projectRoot "config\gcc-demo.toml"
        },
        @{
            Name = "fixed"
            Source = Join-Path $projectRoot "samples\fixed"
            Config = Join-Path $projectRoot "config\gcc-fixed-demo.toml"
        }
    )
    foreach ($case in $cases) {
        & $python.Source -m defectguard scan $case.Source `
            --config $case.Config `
            --output (Join-Path $projectRoot "artifacts\comparison\$($case.Name)") `
            --verify
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}
finally {
    $env:PYTHONPATH = $previousPythonPath
}
