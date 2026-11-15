param(
    [Parameter(Mandatory=$true)][ValidateRange(2,65535)][int]$Release,
    [string]$Source,
    [string]$Output,
    [switch]$TestOnly
)
$ErrorActionPreference = 'Stop'
if (-not $Source) {
    & (Join-Path $PSScriptRoot '..\build.cmd') -Publish
    if ($LASTEXITCODE) { throw 'Publication build failed; refusing to package stale output.' }
}
& node (Join-Path $PSScriptRoot 'logo-vector.mjs') --check
if ($LASTEXITCODE) { throw 'Installer vector logo differs from its source.' }
if (-not $Source) { $Source = Join-Path $PSScriptRoot '..\out' }
if (-not $Output) { $Output = Join-Path $PSScriptRoot '..\test-output\packages' }
$app = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repo = [IO.Path]::GetFullPath((Join-Path $app '..\..'))
$output = [IO.Path]::GetFullPath($Output)
if (-not $output.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Build output must stay in this worktree.' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$resource = Join-Path $output 'bridge.rc'
$bridge = Join-Path $output "harmony-$Release-native.exe"
$icon = Join-Path $repo 'windows\res\harmony.ico'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$payload = Join-Path $output 'payload.zip'
if (Test-Path -LiteralPath $payload) { Remove-Item -LiteralPath $payload -Force }
$zip = [IO.Compression.ZipFile]::Open($payload, [IO.Compression.ZipArchiveMode]::Create)
try {
    $source = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
    $exe = Join-Path $source 'harmony-nativo.exe'
    $exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if (-not $TestOnly) {
        $proof = Get-Content -LiteralPath (Join-Path $source 'publish-build.json') -Raw | ConvertFrom-Json
        if ($proof.sha256 -ne $exeHash -or $proof.profile -ne 'publish-O2-Gy-GF-REF-ICF-no-incremental-no-selftests' -or
            $proof.symbols_sha256 -ne (Get-FileHash -LiteralPath (Join-Path $source 'harmony-nativo.pdb')).Hash) {
            throw 'Source is not the verified publication build.'
        }
    }
    # Keep the exact image and full PDB together, outside the payload and git.
    # Hash subdirectories prevent a later rebuild of the same code replacing symbols.
    $symbols = Join-Path $app "test-output\symbols\$Release\$exeHash"
    New-Item -ItemType Directory -Path $symbols -Force | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
        if ($file.Extension -notin @('.exe','.dll','.pdb') -and $file.Name -ne 'publish-build.json') { continue }
        $saved = Join-Path $symbols $file.FullName.Substring($source.Length + 1)
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($saved)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $saved -Force
    }
    $runtime=@(); $index=@()
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File | Sort-Object FullName) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse payload refused.' }
        # These are compiler/linker/debugger outputs, never runtime resources.
        if ($file.Extension -in @('.pdb','.ilk','.exp','.lib','.iobj','.ipdb','.map','.log','.obj','.idb','.tlog','.lastbuildstate') -or $file.Name -eq 'publish-build.json') { continue }
        # The project emits only these two runtime executables. Refuse an
        # unexpected test/tool executable instead of guessing from its name.
        if ($file.Extension -eq '.exe' -and $file.Name -notin @('harmony-nativo.exe','RestartAgent.exe')) { throw "Unreviewed executable in payload: $($file.Name)" }
        $name = 'app/' + $file.FullName.Substring($source.Length + 1).Replace('\','/')
        $runtime += $file
        $index += @{path=$name;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    if (-not $runtime.Count -or $runtime.Count -gt 4096 -or ($runtime | Measure-Object Length -Sum).Sum -gt 2GB) { throw 'Runtime payload limit exceeded.' }
    # LZX and both tools are supplied by Windows. Fixed aliases let the reader
    # reject every name before invoking expand, including cabinet continuation.
    $cabinet=Join-Path $output 'payload.cab'
    if (Test-Path -LiteralPath $cabinet) { Remove-Item -LiteralPath $cabinet -Force }
    $ddf=Join-Path $output 'payload.ddf'
    $lines=@('.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON','.Set CompressionType=LZX','.Set CompressionMemory=21','.Set MaxDiskSize=0','.Set CabinetNameTemplate=payload.cab',('.Set DiskDirectoryTemplate="'+$output+'"'),'.Set RptFileName=NUL','.Set InfFileName=NUL')
    for ($i=0;$i -lt $runtime.Count;$i++) { $lines += '"{0}" "f{1:D5}"' -f $runtime[$i].FullName,$i }
    [IO.File]::WriteAllLines($ddf,$lines,[Text.Encoding]::Default)
    $compression=Start-Process -FilePath "$env:windir\System32\makecab.exe" -ArgumentList ('/F "{0}"' -f $ddf) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'makecab.log') -RedirectStandardError (Join-Path $output 'makecab.err')
    $null=$compression.Handle
    if (-not $compression.WaitForExit(180000)) { $compression.Kill(); throw 'Cabinet compression timed out.' }
    if ($compression.ExitCode -ne 0) { throw 'Windows LZX compression failed.' }
    $manifest=Join-Path $output 'cabinet-files.json'
    [IO.File]::WriteAllText($manifest,(@{format='harmony-cabinet-lzx-1';files=$index} | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    . (Join-Path $PSScriptRoot 'cabinet.ps1')
    Test-HarmonyCabinet $cabinet $index (Join-Path $output 'validation')
    # Keep ZIP as the outer transport for the existing bridge. Storing the
    # already compressed cabinet avoids a second, ineffective Deflate pass.
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$cabinet,'payload.cab',[IO.Compression.CompressionLevel]::NoCompression) | Out-Null
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$manifest,'cabinet-files.json',[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    foreach ($file in @('instalar.ps1','launch.ps1','cabinet.ps1','live-app.ps1')) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $PSScriptRoot $file),"instalar/$file",[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
$version = Join-Path $output 'bundle.json'
$nativeVersion = (Get-Item -Force -LiteralPath (Join-Path $source 'harmony-nativo.exe')).VersionInfo.ProductPrivatePart
[IO.File]::WriteAllText($version, (@{version=$Release;native_version=$nativeVersion} | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
$rows = @('1 ICON "' + $icon.Replace('\','/') + '"')
$rows += @(
    '1 VERSIONINFO',
    "FILEVERSION 1,0,0,$Release",
    "PRODUCTVERSION 1,0,0,$Release",
    'FILEOS 0x40004', 'FILETYPE 1',
    'BEGIN', ' BLOCK "StringFileInfo"', ' BEGIN', '  BLOCK "040904b0"', '  BEGIN',
    ('   VALUE "FileVersion", "1.0.0.{0}\0"' -f $Release),
    '   VALUE "ProductVersion", "native\0"',
    '   VALUE "ProductName", "harmony\0"',
    '  END', ' END', ' BLOCK "VarFileInfo"', ' BEGIN', '  VALUE "Translation", 0x0409, 1200', ' END', 'END'
)
$hostScript = Join-Path $PSScriptRoot 'bridge-host.ps1'
if ($TestOnly) {
    $hostScript = Join-Path $output 'test-only-bridge-host.ps1'
    $hostText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'bridge-host.ps1'))
    $hostText = $hostText.Replace('$test = $env:HARMONY_INSTALLER_TEST_ROOT', '$test = $env:HARMONY_INSTALLER_TEST_ROOT' + "`n" + 'if (-not $test) { throw ''Test-only helper requires isolated context.'' }')
    [IO.File]::WriteAllText($hostScript,$hostText,[Text.UTF8Encoding]::new($true))
}
foreach ($item in @(@(101,$payload),@(102,$hostScript),@(103,$version),@(104,(Join-Path $app 'assets\Inter.ttf')))) {
    $rows += '{0} RCDATA "{1}"' -f $item[0],$item[1].Replace('\','/')
}
[IO.File]::WriteAllLines($resource, $rows)
$sourceFile = Join-Path $PSScriptRoot 'bridge.cpp'
$res = Join-Path $output 'bridge.res'
$obj = Join-Path $output 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$vc = Join-Path $repo 'windows\native\vc.cmd'
& $vc rc /nologo "/fo$res" $resource
if ($LASTEXITCODE) { throw 'Resource compilation failed.' }
$arguments = @('cl','/nologo','/std:c++20','/EHsc','/MT','/O1','/utf-8','/DNOMINMAX','/DUNICODE','/D_UNICODE',('/Fo'+$obj+'\'),('/Fe'+$bridge),$sourceFile)
$arguments += @((Join-Path $app 'src/platform.cpp'),(Join-Path $repo 'windows\native\src\net.cpp'),(Join-Path $repo 'windows\native\src\json.cpp'),(Join-Path $repo 'windows\native\src\log.cpp'),(Join-Path $repo 'windows\native\src\perf.cpp'))
if ($TestOnly) { $arguments += '/DHARMONY_INSTALLER_TEST_ONLY' }
$arguments += @($res,'/link','/SUBSYSTEM:WINDOWS','user32.lib','gdi32.lib','gdiplus.lib','d2d1.lib','dwrite.lib','shell32.lib','ole32.lib','advapi32.lib','crypt32.lib','winhttp.lib','bcrypt.lib','dwmapi.lib','propsys.lib','shlwapi.lib','version.lib','psapi.lib')
& $vc @arguments
if ($LASTEXITCODE) { throw 'EXE compilation failed.' }
$testProof = $bridge + '.test-only.sha256'
if ($TestOnly) { [IO.File]::WriteAllText($testProof, (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash) }
elseif (Test-Path -LiteralPath $testProof) { Remove-Item -LiteralPath $testProof -Force }
Get-Item -LiteralPath $bridge | Select-Object Name,Length
Get-FileHash -LiteralPath $bridge -Algorithm SHA256
