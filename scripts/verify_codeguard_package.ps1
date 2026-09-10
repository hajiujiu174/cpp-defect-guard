[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$packageRoot=(Resolve-Path -LiteralPath $Package).Path
if(Test-Path -LiteralPath $Output){throw 'Use a new QA directory'}
$qa=[IO.Path]::GetFullPath($Output);[IO.Directory]::CreateDirectory($qa)|Out-Null
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
[IO.File]::WriteAllText((Join-Path $source 'probe.cpp'),"#include <stddef.h>`nsize_t probe(size_t n){return n;}`nint main(){return (int)probe(0);}`n",[Text.UTF8Encoding]::new($false))
$commands=Join-Path $qa 'compile_commands.json'
@(@{directory=$source.Replace('\','/');file='probe.cpp';arguments=@('clang++','-std=c++20','-c','probe.cpp')}) | ConvertTo-Json -Depth 5 -AsArray | Set-Content -LiteralPath $commands -Encoding utf8
$database=Join-Path $qa 'scan.sqlite3'
$help=Invoke-Packaged 'codeguard-cli.exe' @('--help') 'help'
if($help -notmatch 'CodeGuard'){throw 'CLI did not launch'}
$scan=Invoke-Packaged 'codeguard-cli.exe' @('scan',$source,'--database',$database,'--compile-commands',$commands,'--threads','2') 'scan'
if($scan -notmatch 'analysis=complete'){throw 'Packaged Clang analysis incomplete'}
$includes=Invoke-Packaged 'codeguard-cli.exe' @('query',$source,'--database',$database,'--query',"SELECT target FROM edges WHERE kind = 'include'") 'resources'
$resourcePath=$packageRoot.Replace('\','/')+'/resources/clang/include/stddef.h'
if($includes.IndexOf($resourcePath,[StringComparison]::OrdinalIgnoreCase) -lt 0){throw 'Clang did not use the packaged builtin headers'}
$gui=Invoke-Packaged 'codeguard-gui.exe' @('--smoke-test',$source,'--database',(Join-Path $qa 'gui.sqlite3'),'--compile-commands',$commands) 'gui'
if($gui -notmatch 'GUI_SMOKE_OK'){throw 'GUI smoke checks did not complete'}
if($gui -notmatch 'GUI_FONT_OK family=Noto Sans CJK SC'){throw 'Packaged Chinese font was not verified'}
$result=@{package=$packageRoot;clean_path=$true;cli=$true;clang=$true;packaged_resource_header=$resourcePath;gui=$true;chinese_font=$true;timestamp=(Get-Date -Format o)}
$result|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $qa 'result.json') -Encoding utf8
Write-Output 'PACKAGE_QA_OK: clean PATH, CLI, Clang, packaged headers and Qt GUI'
