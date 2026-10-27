[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$packageRoot=(Resolve-Path -LiteralPath $Package).Path
if(Test-Path -LiteralPath $Output){throw 'Use a new QA directory'}
$qa=[IO.Path]::GetFullPath($Output);[IO.Directory]::CreateDirectory($qa)|Out-Null
# Python is a QA-host tool for the optional benchmark, not a native runtime dependency.
foreach($name in @('accept_real_repair.py','real_repair_reference.py','real_repair_process.py','real_repair_evidence.py')){
    if(-not(Test-Path -LiteralPath (Join-Path $packageRoot "scripts/$name"))){throw "Missing benchmark script: $name"}
}
foreach($name in @('accept_real_repair.py','real_repair_evidence.py')){
    & python -B -E -s (Join-Path $packageRoot "scripts/$name") --help > (Join-Path $qa "$name-help.log") 2>&1
    if($LASTEXITCODE -ne 0){throw "Packaged benchmark script could not start: $name"}
}
function Invoke-Packaged([string]$Name,[string[]]$Arguments,[string]$Label){
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=Join-Path $packageRoot $Name;$info.WorkingDirectory=$qa
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.StandardOutputEncoding=[Text.Encoding]::UTF8;$info.StandardErrorEncoding=[Text.Encoding]::UTF8
    $info.Environment['PATH']=$packageRoot+';'+(Join-Path $env:SystemRoot 'System32')
    [void]$info.Environment.Remove('QT_PLUGIN_PATH');[void]$info.Environment.Remove('QML2_IMPORT_PATH');[void]$info.Environment.Remove('QTDIR')
    $info.Environment['QT_QPA_PLATFORM']='offscreen'
    $info.Environment['CODEGUARD_REQUIRE_BUNDLED_FONT']='1'
    foreach($value in $Arguments){$info.ArgumentList.Add($value)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info;[void]$process.Start()
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(45000)){$process.Kill($true);throw "Package QA timeout: $Label"}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$exitCode=$process.ExitCode;$process.Dispose()
    [IO.File]::WriteAllText((Join-Path $qa "$Label.log"),$text+"`n"+$errors)
    if($exitCode -ne 0){throw "Package QA $Label failed ($exitCode): $errors"}
    return $text
}
$source=Join-Path $qa 'source';[IO.Directory]::CreateDirectory($source)|Out-Null
$probeSource=@'
#include <stddef.h>
size_t probe(size_t n){return n;}
template<class T> int template_probe(){T value(1);return 0;}
int null_probe(){int* p=nullptr;return *p;}
int range_probe(){int values[3]={};int i=3;return values[i];}
int main(){return (int)probe(0);}
'@
[IO.File]::WriteAllText((Join-Path $source 'probe.cpp'),$probeSource,[Text.UTF8Encoding]::new($false))
$commands=Join-Path $qa 'compile_commands.json'
@(@{directory=$source.Replace('\','/');file='probe.cpp';arguments=@('clang++','-std=c++20','-c','probe.cpp')}) | ConvertTo-Json -Depth 5 -AsArray | Set-Content -LiteralPath $commands -Encoding utf8
$database=Join-Path $qa 'scan.sqlite3'
$help=Invoke-Packaged 'codeguard-cli.exe' @('--help') 'help'
if($help -notmatch 'CodeGuard'){throw 'CLI did not launch'}
if($help -notmatch 'compare: --candidate FILE or --set FILE'){throw 'Paired coverage CLI help missing'}
foreach($relative in @('sdk/include/codeguard/repair_comparison.hpp','examples/repair-comparison-demo/CMakeLists.txt','repair-coverage-comparison.md')){if(-not(Test-Path -LiteralPath (Join-Path $packageRoot $relative))){throw "Missing paired coverage delivery: $relative"}}
$scan=Invoke-Packaged 'codeguard-cli.exe' @('scan',$source,'--database',$database,'--compile-commands',$commands,'--threads','2') 'scan'
if($scan -notmatch 'analysis=complete'){throw 'Packaged Clang analysis incomplete'}
$flow=Invoke-Packaged 'codeguard-cli.exe' @('query',$source,'--database',$database,'--query','SELECT rule_id, message FROM issues ORDER BY rule_id;') 'local-flow'
if(([regex]::Matches($flow,'(?m)^CG002\tLocal index interval ')).Count -ne 1 -or
   ([regex]::Matches($flow,'(?m)^CG005\tLocal propagation ')).Count -ne 1){throw 'Packaged local flow findings differ from the probe'}
$includes=Invoke-Packaged 'codeguard-cli.exe' @('query',$source,'--database',$database,'--query',"SELECT target FROM edges WHERE kind = 'include'") 'resources'
$resourcePath=$packageRoot.Replace('\','/')+'/resources/clang/include/stddef.h'
if($includes.IndexOf($resourcePath,[StringComparison]::OrdinalIgnoreCase) -lt 0){throw 'Clang did not use the packaged builtin headers'}
$gui=Invoke-Packaged 'codeguard-gui.exe' @('--smoke-test',$source,'--database',(Join-Path $qa 'gui.sqlite3'),'--compile-commands',$commands) 'gui'
if($gui -notmatch 'GUI_SMOKE_OK'){throw 'GUI smoke checks did not complete'}
if($gui -notmatch 'GUI_GRAPH_OK'){throw 'Packaged graph path and source navigation checks did not complete'}
if($gui -notmatch 'GUI_FONT_OK family=Noto Sans CJK SC'){throw 'Packaged Chinese font was not verified'}
$repairSource=Join-Path $qa 'repair-source';[IO.Directory]::CreateDirectory($repairSource)|Out-Null
[IO.File]::WriteAllText((Join-Path $repairSource 'problem.cpp'),"int is_zero(int n){if(n=0)return 1;return 0;}`nint main(){return is_zero(0)==1&&is_zero(1)==0?0:1;}`n",[Text.UTF8Encoding]::new($false))
$repairCompile=Join-Path $qa 'repair-compile';[IO.Directory]::CreateDirectory($repairCompile)|Out-Null
$repairCommands=Join-Path $repairCompile 'compile_commands.json'
@(@{directory=$repairSource.Replace('\','/');file='problem.cpp';arguments=@('clang++','-std=c++20','-c','problem.cpp')})|ConvertTo-Json -Depth 5 -AsArray|Set-Content -LiteralPath $repairCommands -Encoding utf8
$repairGui=Invoke-Packaged 'codeguard-gui.exe' @('--repair-smoke',$repairSource,'--database',(Join-Path $qa 'repair.sqlite3'),'--compile-commands',$repairCommands) 'repair-gui'
if($repairGui -notmatch 'GUI_REPAIR_OK' -or $repairGui -notmatch 'batch-cancel-ledger'){throw 'Packaged repair preview, export and cancelled batch ledger did not complete'}
$setSource=Join-Path $qa 'set-source';[IO.Directory]::CreateDirectory($setSource)|Out-Null
$setFile=Join-Path $setSource 'problem.cpp'
[IO.File]::WriteAllText($setFile,"int f(int n){if(n=0)return 1;return 0;}`nint g(int n){if(n=1)return 1;return 0;}`nint main(){return f(0)==1&&g(1)==1?0:1;}`n",[Text.UTF8Encoding]::new($false))
$setHash=(Get-FileHash -LiteralPath $setFile -Algorithm SHA256).Hash
$setCompile=Join-Path $qa 'set-compile';[IO.Directory]::CreateDirectory($setCompile)|Out-Null
$setCommands=Join-Path $setCompile 'compile_commands.json'
@(@{directory=$setSource.Replace('\','/');file='problem.cpp';arguments=@('clang++','-std=c++20','-c','problem.cpp')})|ConvertTo-Json -Depth 5 -AsArray|Set-Content -LiteralPath $setCommands -Encoding utf8
$setQa=Join-Path $qa 'set-gui';[IO.Directory]::CreateDirectory($setQa)|Out-Null
$setGui=Invoke-Packaged 'codeguard-gui.exe' @('--repair-set-smoke',$setSource,'--database',(Join-Path $setQa 'scan.sqlite3'),'--compile-commands',$setCommands) 'repair-set-gui'
if($setGui -notmatch 'GUI_REPAIR_SET_OK' -or (Get-FileHash -LiteralPath $setFile -Algorithm SHA256).Hash -ne $setHash){throw 'Packaged repair set preview/export/cancel or original source check failed'}
$cancelledSets=@(Get-ChildItem -LiteralPath (Join-Path $setQa 'repair-sets') -Recurse -Filter validation.json | ForEach-Object {Get-Content -LiteralPath $_.FullName -Raw|ConvertFrom-Json} | Where-Object status -eq 'cancelled')
if($cancelledSets.Count -ne 1 -or $cancelledSets[0].targets.Count -ne 2){throw 'Packaged cancelled repair set lost target records'}
$precedenceSource=Join-Path $qa 'precedence-source';[IO.Directory]::CreateDirectory($precedenceSource)|Out-Null
$precedenceText="int f(int n){int error=0;if((error=n<0)||n==0)return error;return 0;}`nint main(){return f(-7)==-7?0:1;}`n"
$precedenceFile=Join-Path $precedenceSource 'problem.cpp';[IO.File]::WriteAllText($precedenceFile,$precedenceText,[Text.UTF8Encoding]::new($false))
$precedenceHash=(Get-FileHash -LiteralPath $precedenceFile -Algorithm SHA256).Hash
$precedenceCompile=Join-Path $qa 'precedence-compile';[IO.Directory]::CreateDirectory($precedenceCompile)|Out-Null
$precedenceCommands=Join-Path $precedenceCompile 'compile_commands.json'
@(@{directory=$precedenceSource.Replace('\','/');file='problem.cpp';arguments=@('clang++','-std=c++20','-c','problem.cpp')})|ConvertTo-Json -Depth 5 -AsArray|Set-Content -LiteralPath $precedenceCommands -Encoding utf8
$precedenceDb=Join-Path $qa 'precedence.sqlite3'
$precedenceScan=Invoke-Packaged 'codeguard-cli.exe' @('scan',$precedenceSource,'--database',$precedenceDb,'--compile-commands',$precedenceCommands) 'precedence-scan'
$precedenceColumn=($precedenceText.IndexOf('error=n<0')+1).ToString()
$precedenceProposal=Invoke-Packaged 'codeguard-cli.exe' @('repair','propose',$precedenceSource,'--database',$precedenceDb,'--output',(Join-Path $qa 'precedence-candidates'),'--rule','CG003','--file','problem.cpp','--line','1','--column',$precedenceColumn) 'precedence-proposal'
if($precedenceScan -notmatch 'analysis=complete' -or $precedenceProposal -notmatch 'condition.assignment_result' -or $precedenceProposal -notmatch 'candidates=3' -or (Get-FileHash -LiteralPath $precedenceFile -Algorithm SHA256).Hash -ne $precedenceHash){throw 'Packaged precedence candidate or source integrity check failed'}
$helperVerified=$false
if(Test-Path -LiteralPath (Join-Path $packageRoot 'codeguard-real-proposal-helper.exe')){
    $helperResult=Invoke-Packaged 'codeguard-real-proposal-helper.exe' @($precedenceSource,$precedenceDb,(Join-Path $qa 'helper-candidates'),'CG003','problem.cpp','1',$precedenceColumn) 'precedence-public-core-helper'
    if($helperResult -notmatch 'condition.assignment_result' -or ([regex]::Matches($helperResult,'(?m)^cgfix-')).Count -ne 3){throw 'Packaged public-Core proposal helper failed'}
    $helperVerified=$true
}
$result=@{package=$packageRoot;clean_path=$true;cli=$true;clang=$true;local_flow=$true;template_initializer=$true;graph_path=$true;repair_preview=$true;repair_batch_cancel_ledger=$true;repair_set_preview=$true;repair_set_cancel_targets=2;repair_set_source_unchanged=$true;packaged_resource_header=$resourcePath;gui=$true;chinese_font=$true;timestamp=(Get-Date -Format o)}
$result['precedence_candidate']=$true;$result['precedence_source_unchanged']=$true
$result['public_core_proposal_helper']=$helperVerified
$result['real_repair_script_imports']=$true
$manifest=Get-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Raw | ConvertFrom-Json
$expected=@('manifest.json')+@($manifest | ForEach-Object {$_.path.Replace('\','/')})
$actual=@(Get-ChildItem -LiteralPath $packageRoot -Recurse -File | ForEach-Object {[IO.Path]::GetRelativePath($packageRoot,$_.FullName).Replace('\','/')})
if(Compare-Object $expected $actual){throw 'Package inventory changed or does not match its manifest'}
foreach($entry in $manifest){
    $file=Join-Path $packageRoot $entry.path
    if((Get-Item -LiteralPath $file).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256){throw "Package file changed or checksum differs: $($entry.path)"}
}
$result['package_manifest_verified']=$true
$result|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $qa 'result.json') -Encoding utf8
Write-Output 'PACKAGE_QA_OK: clean PATH, CLI, Clang, local flow, template initializer, packaged headers and Qt GUI'
