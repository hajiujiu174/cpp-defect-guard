#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$PackageCache='',
    [string]$LockFile=(Join-Path $PSScriptRoot '../config/windows-toolchain.lock.json')
)
$ErrorActionPreference='Stop'
if(-not $IsWindows){throw 'This toolchain is for Windows x64'}
if(Test-Path -LiteralPath $Destination){throw 'Destination must be a new directory; existing toolchains are never overwritten'}
$root=[IO.Path]::GetFullPath($Destination)
$lock=Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json
if($lock.schema -ne 1 -or $lock.platform -ne 'windows-x64-mingw64' -or -not $lock.packages.Count){throw 'Unsupported or empty toolchain lock'}
if(-not $PackageCache){$PackageCache=Join-Path (Split-Path $root) 'package-cache'}
$cache=[IO.Path]::GetFullPath($PackageCache)
[IO.Directory]::CreateDirectory($cache)|Out-Null
[IO.Directory]::CreateDirectory($root)|Out-Null
$tar=Join-Path $env:SystemRoot 'System32/tar.exe'
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($package in $lock.packages){
    if($package.file -notmatch '^mingw-w64-x86_64-[A-Za-z0-9_.+~\-]+\.pkg\.tar\.(zst|xz)$' -or -not $seen.Add($package.file) -or $package.sha256 -notmatch '^[a-f0-9]{64}$') {throw 'Invalid package lock entry'}
    if($package.url -ne "https://repo.msys2.org/mingw/mingw64/$($package.file)"){throw 'Unexpected package source'}
    $archive=Join-Path $cache $package.file
    if(-not(Test-Path -LiteralPath $archive)){
        Write-Output "Downloading $($package.file)"
        # Failed downloads keep a unique partial file, never a reusable cache entry.
        $partial=$archive+'.'+[guid]::NewGuid().ToString('N')+'.partial'
        Invoke-WebRequest -Uri $package.url -OutFile $partial -MaximumRetryCount 3 -RetryIntervalSec 3
        if((Get-Item $partial).Length -ne $package.bytes -or (Get-FileHash $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.sha256){throw "Downloaded package checksum mismatch: $($package.file)"}
        Move-Item -LiteralPath $partial -Destination $archive
    }
    if((Get-Item $archive).Length -ne $package.bytes -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.sha256){throw "Cached package checksum mismatch: $($package.file)"}
    Write-Output "Restoring $($package.name) $($package.version)"
    # Only restore the native prefix; no pacman hooks or changes to installed MSYS2.
    & $tar -xf $archive -C $root 'mingw64'
    if($LASTEXITCODE -ne 0){throw "Cannot extract $($package.file)"}
}
Copy-Item -LiteralPath $LockFile -Destination (Join-Path $root 'toolchain.lock.json')
[ordered]@{lock_sha256=(Get-FileHash $LockFile -Algorithm SHA256).Hash;packages=$lock.packages.Count;restored_at=(Get-Date -Format o)} | ConvertTo-Json | Set-Content (Join-Path $root 'restore.json') -Encoding utf8
Write-Output "TOOLCHAIN_READY=$root"
