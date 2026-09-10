#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Output, [string]$Cache='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$lock=Get-Content (Join-Path $repo 'config/windows-font.lock.json') -Raw|ConvertFrom-Json
if($lock.schema -ne 1 -or $lock.source_commit -notmatch '^[a-f0-9]{40}$'){throw 'Invalid font lock'}
if(Test-Path -LiteralPath $Output){throw 'Use a new font output directory'}
if(-not $Cache){$Cache=Join-Path $repo 'artifacts/font-cache'}
[IO.Directory]::CreateDirectory($Cache)|Out-Null
[IO.Directory]::CreateDirectory($Output)|Out-Null
foreach($file in $lock.files){
    if($file.name -notin @('NotoSansCJKsc-Regular.otf','LICENSE') -or $file.sha256 -notmatch '^[a-f0-9]{64}$'){throw 'Invalid font entry'}
    $base="https://raw.githubusercontent.com/notofonts/noto-cjk/$($lock.source_commit)/Sans/"
    $suffix=if($file.name -eq 'LICENSE'){'LICENSE'}else{'OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf'}
    if($file.url -ne $base+$suffix){throw 'Unexpected font source'}
    $cached=Join-Path $Cache ($file.sha256+'-'+$file.name)
    if(-not(Test-Path -LiteralPath $cached)){
        $partial=$cached+'.'+[guid]::NewGuid().ToString('N')+'.partial'
        Invoke-WebRequest -Uri $file.url -OutFile $partial -MaximumRetryCount 3 -RetryIntervalSec 3
        if((Get-Item $partial).Length -ne $file.bytes -or (Get-FileHash $partial).Hash -ne $file.sha256){throw 'Font download checksum mismatch'}
        Move-Item -LiteralPath $partial -Destination $cached
    }
    if((Get-Item $cached).Length -ne $file.bytes -or (Get-FileHash $cached).Hash -ne $file.sha256){throw 'Font cache checksum mismatch'}
    Copy-Item -LiteralPath $cached -Destination (Join-Path $Output $file.name)
}
Write-Output 'FONT_PACKAGE_OK: original Noto Sans CJK SC and OFL license'
