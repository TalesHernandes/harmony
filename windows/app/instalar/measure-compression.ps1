param([Parameter(Mandatory=$true)][string]$Payload, [Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$output = [IO.Path]::GetFullPath($Output)
if (-not $output.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) { throw 'Use a new measurement directory in this worktree.' }
New-Item -ItemType Directory -Path $output | Out-Null
$stage = Join-Path $output 'source'
[IO.Compression.ZipFile]::ExtractToDirectory([IO.Path]::GetFullPath($Payload),$stage)
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
$rows = @()
foreach ($level in @('NoCompression','Fastest','Optimal')) {
    $path = Join-Path $output "$level.zip"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $zip = [IO.Compression.ZipFile]::Open($path,[IO.Compression.ZipArchiveMode]::Create)
    try { foreach ($file in $files) {
        $name = $file.FullName.Substring($stage.Length+1).Replace('\','/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file.FullName,$name,[IO.Compression.CompressionLevel]::$level) | Out-Null
    } } finally { $zip.Dispose() }
    $packMs = $timer.Elapsed.TotalMilliseconds
    $times = @()
    foreach ($round in 1..3) {
        $dest = Join-Path $output "$level-$round"
        $timer.Restart(); [IO.Compression.ZipFile]::ExtractToDirectory($path,$dest); $times += $timer.Elapsed.TotalMilliseconds
        foreach ($file in $files) {
            $copy = Join-Path $dest $file.FullName.Substring($stage.Length+1)
            if ((Get-FileHash -LiteralPath $copy).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw 'ZIP round-trip mismatch.' }
        }
    }
    $rows += [pscustomobject]@{format="Deflate-$level";bytes=(Get-Item -LiteralPath $path).Length;pack_ms=$packMs;extract_ms=$times;median_extract_ms=($times | Sort-Object)[1];verified=$true}
}
foreach ($type in @('MSZIP','LZX')) {
    $dir = Join-Path $output $type
    New-Item -ItemType Directory -Path $dir | Out-Null
    $ddf = Join-Path $dir 'payload.ddf'
    $lines = @('.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON',".Set CompressionType=$type",'.Set MaxDiskSize=0','.Set CabinetNameTemplate=payload.cab',('.Set DiskDirectoryTemplate="'+$dir+'"'),'.Set RptFileName=NUL','.Set InfFileName=NUL')
    if ($type -eq 'LZX') { $lines += '.Set CompressionMemory=21' }
    # Cabinet uses aliases to avoid duplicate MUI leaf names. This retains the
    # same bytes and file count; all extractions verify every file by SHA-256.
    for ($i=0;$i -lt $files.Count;$i++) { $lines += '"{0}" "f{1:D5}"' -f $files[$i].FullName,$i }
    [IO.File]::WriteAllLines($ddf,$lines,[Text.Encoding]::Default)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & "$env:windir\System32\makecab.exe" /F $ddf > (Join-Path $dir 'makecab.log')
    if ($LASTEXITCODE) { throw "makecab $type failed." }
    $packMs = $timer.Elapsed.TotalMilliseconds
    $path = Join-Path $dir 'payload.cab'; $times = @()
    foreach ($round in 1..3) {
        $dest = Join-Path $dir "extract-$round"; New-Item -ItemType Directory -Path $dest | Out-Null
        $timer.Restart(); & "$env:windir\System32\expand.exe" $path '-F:*' $dest > (Join-Path $dir "expand-$round.log")
        if ($LASTEXITCODE) { throw "expand $type failed." }
        # Restore the real directory layout inside the timed operation, so
        # the CAB is compared with the same ready-to-use tree as the ZIP.
        for ($i=0;$i -lt $files.Count;$i++) {
            $copy = Join-Path $dest $files[$i].FullName.Substring($stage.Length+1)
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copy)) | Out-Null
            [IO.File]::Move((Join-Path $dest ('f{0:D5}' -f $i)),$copy)
        }
        $times += $timer.Elapsed.TotalMilliseconds
        for ($i=0;$i -lt $files.Count;$i++) {
            $copy = Join-Path $dest $files[$i].FullName.Substring($stage.Length+1)
            if ((Get-FileHash -LiteralPath $copy).Hash -ne (Get-FileHash -LiteralPath $files[$i].FullName).Hash) { throw 'CAB round-trip mismatch.' }
        }
    }
    $rows += [pscustomobject]@{format="Cabinet-$type";bytes=(Get-Item -LiteralPath $path).Length;pack_ms=$packMs;extract_ms=$times;median_extract_ms=($times | Sort-Object)[1];verified=$true}
}
$rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
$rows | Format-Table format,bytes,pack_ms,median_extract_ms,verified
