[CmdletBinding()]
param([int]$TranslationUnits=128,[int]$Repeats=3,[string]$Output='')
$ErrorActionPreference='Stop'
if($TranslationUnits -lt 8 -or $TranslationUnits -gt 1024 -or $Repeats -lt 1 -or $Repeats -gt 10){throw 'Invalid benchmark size/repeats'}
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cli=Join-Path $repo 'build/codeguard-analysis/codeguard-cli.exe'
if(-not (Test-Path -LiteralPath $cli)){throw 'Build analysis-release first'}
if(-not $Output){$Output=Join-Path $repo ('artifacts/level3-benchmark-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $Output){throw 'Use a new output directory so benchmark evidence is not overwritten'}
$out=[IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory($out) | Out-Null
$source=Join-Path $out 'source';[IO.Directory]::CreateDirectory($source) | Out-Null
$commands=@()
for($i=0;$i -lt $TranslationUnits;$i++){
    $name='unit{0:d4}.cpp' -f $i
    $lines=for($j=0;$j -lt 32;$j++){"int unit_${i}_function_${j}(int n){int values[4]={}; if(n>0&&n<4){for(int k=0;k<n;++k)values[k]=k;} return values[0];}"}
    [IO.File]::WriteAllText((Join-Path $source $name),($lines -join "`n"),[Text.UTF8Encoding]::new($false))
    $commands+=@{directory=$source.Replace('\','/');file=$name;arguments=@('clang++','-std=c++20','-c',$name)}
}
$commandFile=Join-Path $out 'compile_commands.json'
$commands | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $commandFile -Encoding utf8
$measurements=@()
foreach($repeat in 1..$Repeats){
    foreach($threads in @(1,2,4,8)){
        $database=Join-Path $out "threads-$threads-repeat-$repeat.sqlite3"
        $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$cli;$info.WorkingDirectory=$repo
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        foreach($arg in @('scan',$source,'--database',$database,'--compile-commands',$commandFile,'--threads',"$threads")){$info.ArgumentList.Add($arg)}
        $process=[Diagnostics.Process]::new();$process.StartInfo=$info
        $watch=[Diagnostics.Stopwatch]::StartNew();[void]$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $peak=0L;$cpu=0.0
        while(-not $process.WaitForExit(20)){
            try{$process.Refresh();$peak=[Math]::Max($peak,$process.PeakWorkingSet64);$cpu=[Math]::Max($cpu,$process.TotalProcessorTime.TotalMilliseconds)}catch [InvalidOperationException]{ }
        }
        $watch.Stop();$text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $out "threads-$threads-repeat-$repeat.log"),$text+"`n"+$errors)
        if($process.ExitCode -ne 0){throw "Benchmark failed ($threads threads, repeat $repeat): $errors"}
        $elapsed=[int64]([regex]::Match($text,'(?m)^analysis_ms=(\d+)').Groups[1].Value)
        $functions=[int]([regex]::Match($text,'(?m)^function_metrics=(\d+)').Groups[1].Value)
        if($functions -ne $TranslationUnits*32){throw 'Analysis coverage changed across benchmark runs'}
        $measurements+=[pscustomobject]@{threads=$threads;repeat=$repeat;translation_units=$TranslationUnits;functions=$functions;analysis_ms=$elapsed;total_ms=$watch.ElapsedMilliseconds;cpu_ms_sampled=[Math]::Round($cpu);peak_working_set_bytes_sampled=$peak}
        $process.Dispose()
        Write-Output "threads=$threads repeat=$repeat analysis_ms=$elapsed total_ms=$($watch.ElapsedMilliseconds)"
    }
}
$measurements | Export-Csv -LiteralPath (Join-Path $out 'measurements.csv') -NoTypeInformation -Encoding utf8BOM
$means=@{}
foreach($threads in @(1,2,4,8)){$means[$threads]=($measurements | Where-Object threads -eq $threads | Measure-Object analysis_ms -Average).Average}
$summary=foreach($threads in @(1,2,4,8)){
    $mean=$means[$threads];[pscustomobject]@{threads=$threads;mean_analysis_ms=[Math]::Round($mean,2);tu_per_second=[Math]::Round($TranslationUnits*1000/$mean,2);speedup=[Math]::Round($means[1]/$mean,3);efficiency=[Math]::Round($means[1]/$mean/$threads,3)}
}
$summary | Export-Csv -LiteralPath (Join-Path $out 'summary.csv') -NoTypeInformation -Encoding utf8BOM
$metadata=@{date=(Get-Date -Format o);processor=(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);repeats=$Repeats;translation_units=$TranslationUnits;cli_sha256=(Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash;note='Synthetic local corpus; OS caches are not flushed. CPU and peak memory sampled every 20 ms. No cross-machine claims.'}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'environment.json') -Encoding utf8
$summary | Format-Table
Write-Output "BENCHMARK_OUTPUT=$out"
