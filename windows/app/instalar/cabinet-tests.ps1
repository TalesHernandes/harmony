param([string]$PackageDirectory, [switch]$Observe)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'cabinet.ps1')
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$root=Join-Path $repo ('windows\app\test-output\cabinet-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
if (-not $PackageDirectory) {
    $PackageDirectory=Join-Path $root 'input'
    [IO.Directory]::CreateDirectory($PackageDirectory) | Out-Null
    $entries=@(); $ddf=@('.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON','.Set CompressionType=LZX','.Set CompressionMemory=21','.Set MaxDiskSize=0','.Set CabinetNameTemplate=payload.cab',('.Set DiskDirectoryTemplate="'+$PackageDirectory+'"'),'.Set RptFileName=NUL','.Set InfFileName=NUL')
    foreach ($i in 0..2) {
        $file=Join-Path $PackageDirectory "$i.txt"
        [IO.File]::WriteAllText($file,('fixture '+$i)*100,[Text.UTF8Encoding]::new($false))
        $entries+=@{path=$(if($i -eq 2){'app/harmony-nativo.exe'}elseif($i){'app/b.txt'}else{'app/pasta com espaço/ação.txt'});bytes=(Get-Item -LiteralPath $file).Length;sha256=(Get-FileHash -LiteralPath $file).Hash}
        $ddf+='"{0}" "f{1:D5}"' -f $file,$i
    }
    $ddfFile=Join-Path $root 'fixture.ddf'; [IO.File]::WriteAllLines($ddfFile,$ddf,[Text.Encoding]::Default)
    $p=Start-Process -FilePath "$env:windir\System32\makecab.exe" -ArgumentList ('/F "{0}"' -f $ddfFile) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $root 'makecab.log') -RedirectStandardError (Join-Path $root 'makecab.err')
    $null=$p.Handle
    if (-not $p.WaitForExit(30000)) { $p.Kill(); throw 'Fixture compression timed out.' }
    if ($p.ExitCode -ne 0) { throw 'Fixture compression failed.' }
    @{format='harmony-cabinet-lzx-1';files=$entries} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PackageDirectory 'cabinet-files.json') -Encoding UTF8
}
$cabinet=Join-Path ([IO.Path]::GetFullPath($PackageDirectory)) 'payload.cab'
$manifest=Join-Path ([IO.Path]::GetFullPath($PackageDirectory)) 'cabinet-files.json'
$files=@(([IO.File]::ReadAllText($manifest) | ConvertFrom-Json).files)
$expanded=Join-Path $root 'expanded'
Expand-HarmonyCabinet $cabinet $manifest $expanded
foreach ($file in $files) {
    if ((Get-FileHash -LiteralPath (Join-Path $expanded $file.path)).Hash -ne $file.sha256) { throw 'Round-trip mismatch.' }
}
Write-Output 'PASS: Windows LZX restores every runtime path and SHA-256.'
function Reject([scriptblock]$Action,[string]$Label) {
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    if (-not $rejected) { throw "Accepted invalid cabinet: $Label" }
    Write-Output "PASS: cabinet rejects $Label."
}
$original=$files[0].path
$files[0].path='app/../../outside.exe'
Reject { Test-HarmonyCabinet $cabinet $files $root } 'path traversal before extraction'
$files[0].path=$original
$second=$files[1].path; $files[1].path=$original.ToUpperInvariant()
Reject { Test-HarmonyCabinet $cabinet $files $root } 'case-insensitive duplicate destination'
$files[1].path=$second
$size=$files[0].bytes; $files[0].bytes=2GB+1
Reject { Test-HarmonyCabinet $cabinet $files $root } 'expanded-size overflow'
$files[0].bytes=$size
$bytes=[IO.File]::ReadAllBytes($cabinet); $offset=[BitConverter]::ToUInt32($bytes,16)
$mutant=Join-Path $root 'mutant.cab'
foreach ($case in @('continuation','unsafe-name','duplicate-alias','size','continued-file','attributes','header-size')) {
    $copy=$bytes.Clone()
    switch ($case) {
        'continuation' { $copy[30]=1 }
        'unsafe-name' { [Array]::Copy([Text.Encoding]::ASCII.GetBytes('../bad'),0,$copy,$offset+16,6) }
        'duplicate-alias' { [Array]::Copy($copy,$offset+16,$copy,$offset+23+16,6) }
        'size' { [Array]::Copy([BitConverter]::GetBytes([uint32]($size+1)),0,$copy,$offset,4) }
        'continued-file' { $copy[$offset+8]=255; $copy[$offset+9]=255 }
        'attributes' { $copy[$offset+14]=16 }
        'header-size' { $copy[8]=$copy[8] -bxor 1 }
    }
    [IO.File]::WriteAllBytes($mutant,$copy)
    Reject { Test-HarmonyCabinet $mutant $files $root } $case
}
$index=[IO.File]::ReadAllText($manifest) | ConvertFrom-Json
$index.files[0].sha256='0'*64
$badManifest=Join-Path $root 'wrong-hash.json'
$index | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $badManifest -Encoding UTF8
Reject { Expand-HarmonyCabinet $cabinet $badManifest (Join-Path $root 'wrong-hash') } 'incorrect decompressed SHA-256'
Write-Output 'PASS: cabinet safety and real extraction complete.'
# Exercise the real installer with a disposable artifact allowlist. Only the
# pinned DFN sizes/hashes change; no real native binary or registry is used.
$scripts=Join-Path $root 'scripts'
[IO.Directory]::CreateDirectory($scripts) | Out-Null
foreach ($name in @('launch.ps1','live-app.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $scripts $name) }
$installer=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'instalar.ps1'))
$source=Join-Path $expanded 'app'
foreach ($artifact in @(
    @{name='df.dll';bytes='16678912';hash='4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f'},
    @{name='DeepFilterNet3_onnx.tar.gz';bytes='7983136';hash='c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616'})) {
    $file=Join-Path $source $artifact.name
    [IO.File]::WriteAllText($file,'disposable DFN fixture')
    $installer=$installer.Replace($artifact.bytes,[string](Get-Item $file).Length).Replace($artifact.hash,(Get-FileHash $file).Hash.ToLowerInvariant())
}
$licenses=Join-Path $source 'licenses/DeepFilterNet-0.5.6'
[IO.Directory]::CreateDirectory($licenses) | Out-Null
foreach ($name in @('LICENSE','LICENSE-MIT','LICENSE-APACHE')) { [IO.File]::WriteAllText((Join-Path $licenses $name),'fixture') }
$installScript=Join-Path $scripts 'instalar.ps1'
[IO.File]::WriteAllText($installScript,$installer,[Text.UTF8Encoding]::new($true))
$installed=Join-Path $root 'installed'
function Observe-Installation {
    if (-not $Observe) { return $null }
    $exe=Join-Path $repo 'windows/app/test-output/harmony-fast-launch-tests.exe'
    & $exe --warm $installed
    if ($LASTEXITCODE) { throw 'Observer fixture could not warm the verified record.' }
    $record=[IO.File]::ReadAllText((Join-Path $installed 'verified-launch.json')) | ConvertFrom-Json
    $name='Local\harmony-native-observer-'+($installed.ToLowerInvariant() -replace '[\\/:]','_')
    $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($name+'-test-ready'))
    $release=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($name+'-test-release'))
    $child=Start-Process -FilePath $exe -ArgumentList ('--observe "{0}" {1}' -f $installed,$record.id) -WindowStyle Hidden -PassThru
    $null=$child.Handle
    if(-not $ready.WaitOne(10000)) { throw 'Observer fixture did not open an installed file.' }
    return @{child=$child;ready=$ready;release=$release}
}
function Check-Observer($Worker,[string]$Label) {
    if (-not $Worker) { return }
    try {
        if(-not $Worker.child.WaitForExit(10000) -or $Worker.child.ExitCode) { throw 'Installer failed to cancel background reader.' }
        Write-Output "PASS: active background reader does not prevent $Label."
    } finally { $Worker.ready.Dispose(); $Worker.release.Dispose(); $Worker.child.Dispose() }
}
& $installScript -Origem $source -Destino $installed -Versao 1 -SomenteArquivos | Out-Null
$live=Join-Path $installed 'app/harmony-nativo.exe'
$firstHash=(Get-FileHash $live).Hash
$pointer=Get-Content (Join-Path $installed 'current.json') -Raw | ConvertFrom-Json
if ($pointer.live -ne 'app' -or $pointer.directory -ne 'app-1') { throw 'First installation did not select the fixed path.' }
$worker=Observe-Installation
if ($Observe) {
    & $installScript -Origem $source -Destino $installed -Versao 2 -SomenteArquivos | Out-Null
    Check-Observer $worker 'an update'
    if(Test-Path -LiteralPath (Join-Path $installed 'verified-launch.json')) { throw 'Update retained the old launch record.' }
}
# Simulate a pre-migration installation before the next real installer run.
[IO.Directory]::Move((Join-Path $installed 'app'),(Join-Path $installed '.retired-app-legacy'))
[IO.File]::WriteAllText((Join-Path $installed 'current.json'),'{"version":1,"directory":"app-1"}')
[IO.File]::WriteAllText((Join-Path $source 'harmony-nativo.exe'),'updated fixture')
& $installScript -Origem $source -Destino $installed -Versao 2 -SomenteArquivos | Out-Null
if ((Get-FileHash $live).Hash -eq $firstHash -or -not (Test-Path (Join-Path $installed 'app-1/manifest.json'))) { throw 'Update failed to replace app or retain the previous depot.' }
Write-Output 'PASS: first install and update use the same executable path and retain the previous depot.'
# A locked live file must leave both the old selection and live bytes intact.
$handle=[IO.File]::Open($live,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try { Reject { & $installScript -Origem $source -Destino $installed -Versao 3 -SomenteArquivos | Out-Null } 'promotion while live files are held open' }
finally { $handle.Dispose() }
if ((Get-Content (Join-Path $installed 'current.json') -Raw | ConvertFrom-Json).version -ne 2) { throw 'Busy promotion changed current.json.' }
# Releases need not be consecutive. Cleanup must retain the actual predecessor.
& $installScript -Origem $source -Destino $installed -Versao 5 -SomenteArquivos | Out-Null
$installScript=Join-Path $installed 'instalar.ps1'
if ($Observe) {
    $worker=Observe-Installation
    [IO.File]::Delete($live)
    [IO.File]::WriteAllText($live,'damaged live copy')
    & $installScript -Destino $installed -Versao 5 -Limpar -SomenteArquivos | Out-Null
    Check-Observer $worker 'same-version repair'
    if((Get-FileHash $live).Hash -ne (Get-FileHash (Join-Path $installed 'app-5/harmony-nativo.exe')).Hash) { throw 'Repair failed to restore live bytes.' }
}
$retired=Join-Path $installed '.retired-app-busy'
[IO.Directory]::CreateDirectory($retired) | Out-Null
$held=Join-Path $retired 'held.dll'; [IO.File]::WriteAllText($held,'busy')
$handle=[IO.File]::Open($held,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {
    & $installScript -Destino $installed -Limpar -SomenteArquivos | Out-Null
    if (-not (Test-Path -LiteralPath $held)) { throw 'Cleanup partially deleted a busy retired tree.' }
} finally { $handle.Dispose() }
& $installScript -Destino $installed -Limpar -SomenteArquivos | Out-Null
if (Test-Path -LiteralPath $retired) { throw 'Cleanup retained an unlocked retired tree.' }
if (-not (Test-Path -LiteralPath $live)) { throw 'Cleanup removed the live tree.' }
if (-not (Test-Path -LiteralPath (Join-Path $installed 'app-2/manifest.json')) -or (Test-Path -LiteralPath (Join-Path $installed 'app-1'))) { throw 'Cleanup did not retain exactly the current and previous depots.' }
$worker=Observe-Installation
& $installScript -Destino $installed -Remover -SomenteArquivos | Out-Null
Check-Observer $worker 'uninstallation'
if (Test-Path -LiteralPath $installed) { throw 'Removal retained live or staging trees.' }
Write-Output 'PASS: cleanup defers busy retired trees, preserves app, and removal deletes the owned fixture.'
