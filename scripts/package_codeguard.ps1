[CmdletBinding()]
param([string]$Output='', [string]$BuildDirectory='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build=if($BuildDirectory){(Resolve-Path -LiteralPath $BuildDirectory).Path}else{Join-Path $repo 'build/codeguard-analysis'}
if(-not $Output){$Output=Join-Path $repo ('artifacts/CodeGuard-Level3-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $Output){throw 'Use a new package directory'}
$out=[IO.Path]::GetFullPath($Output)
$runtime=Split-Path (Get-Command clang.exe).Source
$deploy=(Get-Command windeployqt6.exe -ErrorAction SilentlyContinue).Source
if(-not $deploy){$deploy=(Get-Command windeployqt.exe).Source}
$objdump=(Get-Command objdump.exe).Source
[IO.Directory]::CreateDirectory($out) | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'codeguard-cli.exe') -Destination $out
Copy-Item -LiteralPath (Join-Path $build 'gui/qt/codeguard-gui.exe') -Destination $out
& $deploy --no-translations --no-opengl-sw --no-compiler-runtime --dir $out (Join-Path $out 'codeguard-gui.exe')
if($LASTEXITCODE -ne 0){throw 'Qt runtime deployment failed'}
$plugins=(& (Get-Command qtpaths6.exe).Source --query QT_INSTALL_PLUGINS).Trim()
$offscreen=Join-Path $plugins 'platforms/qoffscreen.dll'
if(Test-Path -LiteralPath $offscreen){Copy-Item -LiteralPath $offscreen -Destination (Join-Path $out 'platforms/qoffscreen.dll')}
$queue=[Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object Extension -in @('.exe','.dll') | ForEach-Object {$queue.Enqueue($_.FullName)}
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while($queue.Count){
    $binary=$queue.Dequeue();if(-not $seen.Add($binary)){continue}
    $imports=& $objdump -p $binary
    if($LASTEXITCODE -ne 0){throw "Cannot inspect $binary"}
    foreach($line in $imports){
        if($line -notmatch 'DLL Name:\s*(\S+)'){continue}
        $name=$Matches[1];$destination=Join-Path $out $name
        if(Test-Path -LiteralPath $destination){continue}
        if((Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$name")) -or $name -match '^(api-ms-|ext-ms-)'){continue}
        $source=Join-Path $runtime $name
        if(-not(Test-Path -LiteralPath $source)){throw "Missing runtime dependency $name imported by $binary"}
        Copy-Item -LiteralPath $source -Destination $destination;$queue.Enqueue($destination)
    }
}
$resource=(& (Join-Path $runtime 'clang.exe') -print-resource-dir).Trim()
$resourceTarget=Join-Path $out 'resources/clang';[IO.Directory]::CreateDirectory($resourceTarget)|Out-Null
Copy-Item -LiteralPath (Join-Path $resource 'include') -Destination $resourceTarget -Recurse
& (Join-Path $PSScriptRoot 'package_windows_font.ps1') -Output (Join-Path $out 'resources/fonts')
$sdk=Join-Path $out 'sdk';[IO.Directory]::CreateDirectory($sdk)|Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'core/include') -Destination $sdk -Recurse
Copy-Item -LiteralPath (Join-Path $build 'libcodeguard-core.a') -Destination $sdk
$examples=Join-Path $out 'examples';[IO.Directory]::CreateDirectory($examples)|Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'samples/level3-demo') -Destination $examples -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'docs/level3-acceptance.md') -Destination (Join-Path $out 'Level3说明.md')
$licenses=Join-Path (Split-Path $runtime) 'share/licenses'
if(Test-Path -LiteralPath $licenses){Copy-Item -LiteralPath $licenses -Destination (Join-Path $out 'third-party-licenses') -Recurse}
@'
CodeGuard Level 3 Windows x64

双击 codeguard-gui.exe 启动。CLI: codeguard-cli.exe --help
程序自带 Qt、SQLite、Clang 运行库和 Clang 内建头文件。
中文界面使用包内 Noto Sans CJK SC 字体，原始许可见 resources/fonts/LICENSE；无需安装系统字体。
分析真实工程仍需该工程的 compile_commands.json、系统/第三方头文件。
构建测试需要另行提供 CMake、Ninja、C/C++ 编译器和 CTest；Git 功能需要 Git。
这些开发工具可通过 PATH 或界面中的编译器设置使用，未打包整套编译工具链。

examples/level3-demo 含 5 个故意保留的问题，运行测试只覆盖安全路径。
因此测试通过和规则告警同时出现是预期演示行为，不代表误报。

sdk/ 提供本机 MinGW ABI 的 Core 静态库及头文件。该静态库不适用于 MSVC。
第三方组件许可见 third-party-licenses；原始第三方库与 Qt DLL 独立分发。
'@ | Set-Content -LiteralPath (Join-Path $out 'README.txt') -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $out -Recurse -File | ForEach-Object {[pscustomobject]@{path=[IO.Path]::GetRelativePath($out,$_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}}
$manifest|ConvertTo-Json -Depth 3|Set-Content -LiteralPath (Join-Path $out 'manifest.json') -Encoding utf8
Write-Output "PACKAGE_DIRECTORY=$out"
