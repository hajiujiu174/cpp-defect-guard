#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ToolchainRoot,
    [ValidateSet('Core','Analysis')][string]$Mode='Analysis',
    [ValidateRange(1,16)][int]$Jobs=2,
    [switch]$Package,
    [string]$BuildDirectory='',
    [string]$OutputDirectory=''
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$toolchain=(Resolve-Path -LiteralPath $ToolchainRoot).Path
$lock=Join-Path $repo 'config/windows-toolchain.lock.json'
if((Get-FileHash $lock).Hash -ne (Get-FileHash (Join-Path $toolchain 'toolchain.lock.json')).Hash){throw 'Toolchain lock differs; restore a new toolchain directory'}
$bin=Join-Path $toolchain 'mingw64/bin'
$git=(Get-Command git.exe).Source
$saved=@{}
$variables=@('PATH','CC','CXX','CMAKE_PREFIX_PATH','CMAKE_TOOLCHAIN_FILE','CMAKE_GENERATOR','CMAKE_GENERATOR_PLATFORM','CMAKE_GENERATOR_TOOLSET','Qt6_DIR','LLVM_DIR','Clang_DIR','QT_PLUGIN_PATH','QML2_IMPORT_PATH','QTDIR','QT_QPA_PLATFORM','CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','INCLUDE','LIB','CFLAGS','CXXFLAGS','LDFLAGS','PKG_CONFIG_PATH')
foreach($name in $variables){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process');[Environment]::SetEnvironmentVariable($name,$null,'Process')}
$env:PATH=$bin+';'+(Split-Path $git)+';'+(Join-Path $env:SystemRoot 'System32')+';'+$env:SystemRoot
$env:CC=Join-Path $bin 'clang.exe';$env:CXX=Join-Path $bin 'clang++.exe'
$env:QT_PLUGIN_PATH=Join-Path $toolchain 'mingw64/share/qt6/plugins';$env:QT_QPA_PLATFORM='offscreen'
$build=if($BuildDirectory){[IO.Path]::GetFullPath($BuildDirectory)}else{Join-Path $repo ('build/windows-ci-'+$Mode.ToLowerInvariant())}
$output=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $repo 'artifacts/windows-ci'}
$logs=Join-Path $output $Mode.ToLowerInvariant()
[IO.Directory]::CreateDirectory($logs)|Out-Null
function Invoke-Logged([string]$Program,[string[]]$Arguments,[string]$Label){
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logs "$Label.log") | Write-Host
    if($LASTEXITCODE -ne 0){throw "$Label failed with exit code $LASTEXITCODE; see $logs"}
}
Push-Location $repo
try {
    if(Test-Path -LiteralPath $build){throw "Use a clean build directory: $build already exists"}
    $versions=[ordered]@{commit=(& $git rev-parse HEAD);mode=$Mode;lock_sha256=(Get-FileHash $lock).Hash;os=[Environment]::OSVersion.VersionString;ci_image=$env:ImageVersion;powershell=$PSVersionTable.PSVersion.ToString();git=(& $git --version);clang=(& "$bin/clang.exe" --version);cmake=(& "$bin/cmake.exe" --version);ninja=(& "$bin/ninja.exe" --version);qt=(& "$bin/qtpaths6.exe" --qt-version)}
    $versions|ConvertTo-Json -Depth 4|Set-Content (Join-Path $logs 'environment.json') -Encoding utf8
    $analysis=if($Mode -eq 'Analysis'){'ON'}else{'OFF'}
    $configuration=if($Mode -eq 'Analysis'){'Release'}else{'Debug'}
    Invoke-Logged "$bin/cmake.exe" @('-S',$repo,'-B',$build,'-G','Ninja',"-DCMAKE_BUILD_TYPE=$configuration",'-DBUILD_TESTING=ON',"-DCODEGUARD_ENABLE_ANALYSIS=$analysis", "-DCODEGUARD_BUILD_GUI=$analysis",'-DCODEGUARD_REQUIRE_GTEST=ON',"-DCMAKE_C_COMPILER=$env:CC","-DCMAKE_CXX_COMPILER=$env:CXX","-DCMAKE_MAKE_PROGRAM=$bin/ninja.exe","-DCMAKE_PREFIX_PATH=$toolchain/mingw64",'-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF') 'configure'
    Invoke-Logged "$bin/cmake.exe" @('--build',$build,'--parallel',"$Jobs") 'build'
    Invoke-Logged "$bin/ctest.exe" @('--test-dir',$build,'--output-on-failure','--no-tests=error','--parallel','1','--output-junit',(Join-Path $logs 'tests.xml')) 'tests'
    if($Package){
        if($Mode -ne 'Analysis'){throw 'Packaging requires Analysis mode'}
        $packageDirectory=Join-Path $output 'CodeGuard-Windows-x64'
        & (Join-Path $PSScriptRoot 'package_codeguard.ps1') -BuildDirectory $build -Output $packageDirectory
        & (Join-Path $PSScriptRoot 'verify_codeguard_package.ps1') -Package $packageDirectory -Output (Join-Path $output 'package-qa')
        Compress-Archive -LiteralPath $packageDirectory -DestinationPath ($packageDirectory+'.zip') -CompressionLevel Optimal
        (Get-FileHash ($packageDirectory+'.zip') -Algorithm SHA256).Hash+'  CodeGuard-Windows-x64.zip' | Set-Content ($packageDirectory+'.zip.sha256') -Encoding ascii
    }
} finally {
    Pop-Location
    foreach($name in $variables){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
