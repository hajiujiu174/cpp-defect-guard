#requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$restore=Join-Path $repo 'scripts/restore_windows_toolchain.ps1'
$testRoot=Join-Path $repo ('artifacts/restore-tests/'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$full=Get-Content (Join-Path $repo 'config/windows-toolchain.lock.json') -Raw|ConvertFrom-Json
$package=$full.packages|Where-Object name -eq 'mingw-w64-x86_64-ninja'
$lock=[ordered]@{schema=1;platform='windows-x64-mingw64';roots=@($package.name);packages=@($package)}
$lockFile=Join-Path $testRoot 'test.lock.json'
$lock|ConvertTo-Json -Depth 6|Set-Content $lockFile -Encoding utf8
$cache=Join-Path $testRoot 'cache'
$valid=Join-Path $testRoot 'valid'
& $restore -Destination $valid -PackageCache $cache -LockFile $lockFile
if(-not(Test-Path (Join-Path $valid 'mingw64/bin/ninja.exe'))){throw 'Valid archive not extracted'}
if(-not(Test-Path (Join-Path $valid 'restore.json'))){throw 'Restore marker missing'}
function Expect-Failure([string]$Name,[scriptblock]$Action,[string]$Pattern){
    try {& $Action;throw 'Unexpected success'}
    catch {if($_.Exception.Message -notmatch $Pattern){throw};Write-Output "PASS: $Name"}
}
Expect-Failure 'existing destination is preserved' {& $restore -Destination $valid -PackageCache $cache -LockFile $lockFile} 'Destination must be a new directory'
$markerHash=(Get-FileHash (Join-Path $valid 'restore.json')).Hash
$package.sha256='0'*64
$lock|ConvertTo-Json -Depth 6|Set-Content $lockFile -Encoding utf8
$bad=Join-Path $testRoot 'bad-hash'
Expect-Failure 'bad cached hash is rejected' {& $restore -Destination $bad -PackageCache $cache -LockFile $lockFile} 'Cached package checksum mismatch'
if(Test-Path (Join-Path $bad 'restore.json')){throw 'Failed restore published success marker'}
$package.url='https://invalid.example/package'
$lock|ConvertTo-Json -Depth 6|Set-Content $lockFile -Encoding utf8
Expect-Failure 'unexpected source is rejected before download' {& $restore -Destination (Join-Path $testRoot 'bad-source') -PackageCache $cache -LockFile $lockFile} 'Unexpected package source'
$package.file='../escape.pkg.tar.zst'
$lock|ConvertTo-Json -Depth 6|Set-Content $lockFile -Encoding utf8
Expect-Failure 'invalid filename is rejected' {& $restore -Destination (Join-Path $testRoot 'bad-name') -PackageCache $cache -LockFile $lockFile} 'Invalid package lock entry'
if($markerHash -ne (Get-FileHash (Join-Path $valid 'restore.json')).Hash){throw 'Existing destination was changed'}
Write-Output 'RESTORE_TESTS_OK: download, extraction, checksum rejection, destination and source guards'
