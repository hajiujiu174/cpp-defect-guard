#requires -Version 7.0
param([Parameter(Mandatory)][string]$ToolchainRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$testRoot=Join-Path $repo ('artifacts/ci-environment-tests/'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$variables=@('PATH','CXX','CC','CMAKE_PREFIX_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM')
$before=@{};foreach($name in $variables){$before[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
    & (Join-Path $repo 'scripts/ci_windows.ps1') -ToolchainRoot $ToolchainRoot -BuildDirectory $testRoot -OutputDirectory (Join-Path $testRoot 'logs')
    throw 'Unexpected success reusing a build directory'
} catch {
    if($_.Exception.Message -notmatch 'Use a clean build directory'){throw}
}
foreach($name in $variables){
    if($before[$name] -cne [Environment]::GetEnvironmentVariable($name,'Process')){throw "Environment changed after failure: $name"}
}
Write-Output 'CI_ENVIRONMENT_TESTS_OK: existing build refused and process environment restored'
