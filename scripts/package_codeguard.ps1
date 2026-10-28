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
Copy-Item -LiteralPath (Join-Path $repo 'samples/repair-demo') -Destination $examples -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'samples/repair-set-demo') -Destination $examples -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'samples/repair-test-demo') -Destination $examples -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'samples/repair-comparison-demo') -Destination $examples -Recurse
$benchmarks=Join-Path $out 'benchmarks';[IO.Directory]::CreateDirectory($benchmarks)|Out-Null
foreach($name in @('libgit2-error-return','libgit2-worktree-return')){Copy-Item -LiteralPath (Join-Path $repo "benchmarks/$name") -Destination $benchmarks -Recurse}
$benchmarkScripts=Join-Path $out 'scripts';[IO.Directory]::CreateDirectory($benchmarkScripts)|Out-Null
foreach($name in @('accept_real_repair.py','real_repair_reference.py','real_repair_process.py','real_repair_evidence.py')){Copy-Item -LiteralPath (Join-Path $repo "scripts/$name") -Destination $benchmarkScripts}
$proposalHelper=Join-Path $build 'tests/codeguard/codeguard-real-proposal-helper.exe'
if(Test-Path -LiteralPath $proposalHelper){Copy-Item -LiteralPath $proposalHelper -Destination (Join-Path $out 'codeguard-real-proposal-helper.exe')}
Copy-Item -LiteralPath (Join-Path $repo 'docs/level3-acceptance.md') -Destination (Join-Path $out 'Level3说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-candidates.md') -Destination (Join-Path $out '修复候选说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-batch.md') -Destination (Join-Path $out '批量验证说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-coverage.md') -Destination (Join-Path $out '覆盖率说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-set.md') -Destination (Join-Path $out '组合补丁说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-test-coverage.md') -Destination (Join-Path $out '逐测试覆盖说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-coverage-comparison.md') -Destination (Join-Path $out '多轮覆盖对照说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/real-repair-benchmark.md') -Destination (Join-Path $out '真实修复评测说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/repair-validation-performance.md') -Destination (Join-Path $out '验证性能说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/git-provenance.md') -Destination (Join-Path $out 'Git归属说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'docs/validation-inputs.md') -Destination (Join-Path $out '验证输入与内存说明.md')
foreach($name in @('repair-candidates.md','repair-batch.md','repair-coverage.md','repair-set.md','repair-test-coverage.md','repair-coverage-comparison.md','real-repair-benchmark.md','repair-validation-performance.md','git-provenance.md','validation-inputs.md')){Copy-Item -LiteralPath (Join-Path $repo "docs/$name") -Destination (Join-Path $out $name)}
Copy-Item -LiteralPath (Join-Path $repo 'docs/evidence') -Destination (Join-Path $out 'evidence') -Recurse
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

问题列表右键可生成支持规则的修复候选，查看只读差异、导出或在源码副本中验证。
examples/repair-demo 的原始行为测试故意失败，可对比条件比较、显式赋值和有界格式化候选。
候选仅在副本验证，不自动写回原工程；验证门槛通过仍需人工评估修复正确性。
“验证全部并排序”逐个验证当前候选，保留失败/取消/未运行记录；CLI repair evaluate 支持重复 --candidate。
排序依据验证证据与修改字节，不提供正确性概率；默认关闭覆盖率采集时保持未测量。
可选 gcov 采集默认关闭，开启需要 GNU GCC 和匹配的 gcov，按工程设置选择编译器。
开启后另要求所选行有执行记录；覆盖率不是正确性证明，工具链不随运行包提供。
Windows 开启 gcov 时请选择完整路径仅含 ASCII 字符的输出目录；源码文件名可含中文和空格。
Ctrl/Shift 选择多个问题后可右键组合候选，明确勾选 2—16 项后在同一副本验证。
CLI repair compose / repair validate-set 读写 repair.cgset；重叠范围或同问题备选会被拒绝。
examples/repair-set-demo 演示单项修改测试失败、两项比较修改组合后测试通过。
逐目标记录及副本完整性范围见“组合补丁说明.md”；门槛通过仍不表示正确性已评估。
可选逐测试组重跑：CLI --coverage gcov-tests，Qt 在 gcov 下勾选逐测试组；需要 CTest 3.29+。
每组包含依赖和自动 fixture，重置计数并记录目标行执行，默认声明测试数上限 64。
examples/repair-test-demo 与“逐测试覆盖说明.md”介绍归因及重跑不一致的拒绝规则。
CLI repair compare 支持 --candidate / --set、--runs 2..5，多轮独立构建原版与补丁并逐组采集。
Qt 对话框提供独立多轮对照按钮。examples/repair-comparison-demo 与“多轮覆盖对照说明.md”介绍行映射与边界。
正常返回失败的原版测试仅作诊断；崩溃/跳过/未运行不算完整观察，覆盖变化不证明修复正确。
CG003 增加整数赋值右侧比较的优先级提示及“先保存返回值，再比较”候选，仍需核对意图和转换。
benchmarks/libgit2-error-return 保存已合并上游修复的来源、回归程序和限制，见“真实修复评测说明.md”。
scripts/accept_real_repair.py 支持 Git 来源或校验过的离线源码归档；Python 仅用于可选评测，不影响主程序运行。
文件清单与副本复制的排除路径优化及测量边界见“验证性能说明.md”，没有减少静态检查或测试门槛。
Git 版本采集要求工程根目录有本地 .git 目录/文件；归档或普通子目录保留未知，见“Git归属说明.md”。
验证流程按需读取文件、配置和编译元数据，完整快照接口保留；测量与门槛范围见“验证输入与内存说明.md”。

sdk/ 提供本机 MinGW ABI 的 Core 静态库及头文件。该静态库不适用于 MSVC。
第三方组件许可见 third-party-licenses；原始第三方库与 Qt DLL 独立分发。
'@ | Set-Content -LiteralPath (Join-Path $out 'README.txt') -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $out -Recurse -File | ForEach-Object {[pscustomobject]@{path=[IO.Path]::GetRelativePath($out,$_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}}
$manifest|ConvertTo-Json -Depth 3|Set-Content -LiteralPath (Join-Path $out 'manifest.json') -Encoding utf8
Write-Output "PACKAGE_DIRECTORY=$out"
