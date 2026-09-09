# Regenerate intentionally after testing a complete, consistent MSYS2 installation.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$MsysRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$roots=@('clang','clang-tools-extra','llvm','gcc','cmake','ninja','qt6-base','sqlite3','gtest','binutils') | ForEach-Object {"mingw-w64-x86_64-$_"}
$packages=@{}; $providers=@{}
foreach($directory in Get-ChildItem (Join-Path $MsysRoot 'var/lib/pacman/local') -Directory){
    $desc=@{}; $key=''
    foreach($line in Get-Content (Join-Path $directory.FullName 'desc')){
        if($line -match '^%(.+)%$'){$key=$Matches[1];$desc[$key]=@()}
        elseif($line -and $key){$desc[$key]+=$line}
    }
    $name=$desc.NAME[0];$packages[$name]=$desc
    foreach($value in $desc.PROVIDES){$providers[($value -split '[<>=]')[0]]=$name}
}
$queue=[Collections.Generic.Queue[string]]::new();foreach($name in $roots){$queue.Enqueue($name)}
$selected=[Collections.Generic.HashSet[string]]::new()
while($queue.Count){
    $name=$queue.Dequeue()
    if(-not $packages.ContainsKey($name)){$name=$providers[$name]}
    if(-not $name -or -not $packages.ContainsKey($name)){throw 'Unresolved installed dependency'}
    if(-not $name.StartsWith('mingw-w64-x86_64-')){throw "Non-native dependency: $name"}
    if(-not $selected.Add($name)){continue}
    foreach($dependency in $packages[$name].DEPENDS){$queue.Enqueue(($dependency -split '[<>=]')[0])}
}
$rows=@(foreach($name in $selected | Sort-Object){
    $desc=$packages[$name]; $version=$desc.VERSION[0];$arch=$desc.ARCH[0]
    $archives=@(Get-ChildItem (Join-Path $MsysRoot 'var/cache/pacman/pkg') -File | Where-Object {$_.Name -in @("$name-$version-$arch.pkg.tar.zst","$name-$version-$arch.pkg.tar.xz")})
    if($archives.Count -ne 1){throw "Exactly one cached archive required: $name $version"}
    $archive=$archives[0]
    [pscustomobject][ordered]@{name=$name;version=$version;file=$archive.Name;bytes=$archive.Length;sha256=(Get-FileHash $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant();url="https://repo.msys2.org/mingw/mingw64/$($archive.Name)"}
})
$lock=[ordered]@{schema=1;platform='windows-x64-mingw64';roots=$roots;packages=$rows}
$lock | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $repo 'config/windows-toolchain.lock.json') -Encoding utf8
Write-Output "Locked $($rows.Count) packages, $((($rows | Measure-Object bytes -Sum).Sum / 1MB).ToString('F1')) MiB"
