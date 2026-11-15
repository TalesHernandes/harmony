# Only the single-cabinet, fixed-alias subset emitted by package.ps1 is accepted.
# CFHEADER/CFFILE layout: Microsoft's MS-CAB specification, sections 2.1/2.3.
function Test-HarmonyCabinet([string]$Cabinet, $Files, [string]$Destination) {
    $destination=[IO.Path]::GetFullPath($Destination).TrimEnd('\')
    if (-not $Files -or $Files.Count -gt 4096) { throw 'Invalid cabinet file count.' }
    $paths=@{}; [long]$total=0
    foreach ($file in $Files) {
        if ($file.path -notmatch '^app/' -or $file.path.Contains(':') -or $file.path.Contains('\') -or $file.path -match '(^|/)\.\.?(/|$)' -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or [long]$file.bytes -lt 0) { throw 'Invalid cabinet manifest entry.' }
        $path=[IO.Path]::GetFullPath((Join-Path $destination $file.path))
        if (-not $path.StartsWith($destination+'\app\',[StringComparison]::OrdinalIgnoreCase) -or $paths.ContainsKey($path)) { throw 'Unsafe or duplicate cabinet path.' }
        $paths[$path]=$true; $total += [long]$file.bytes
        if ($total -gt 2GB) { throw 'Expanded cabinet limit exceeded.' }
    }
    $reader=[IO.BinaryReader]::new([IO.File]::OpenRead($Cabinet))
    try {
        $length=$reader.BaseStream.Length
        if ($length -lt 36 -or $length -gt 512MB -or $reader.ReadUInt32() -ne 0x4643534d -or $reader.ReadUInt32() -ne 0 -or $reader.ReadUInt32() -ne $length -or $reader.ReadUInt32() -ne 0) { throw 'Invalid cabinet header.' }
        $offset=$reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0 -or $reader.ReadByte() -ne 3 -or $reader.ReadByte() -ne 1) { throw 'Unsupported cabinet version.' }
        $folders=$reader.ReadUInt16(); $count=$reader.ReadUInt16(); $flags=$reader.ReadUInt16()
        $null=$reader.ReadUInt16(); $part=$reader.ReadUInt16()
        if (-not $folders -or $folders -gt $count -or $count -ne $Files.Count -or $flags -ne 0 -or $part -ne 0 -or $offset -lt 36+8*$folders -or $offset+23*$count -gt $length) { throw 'Linked, reserved or malformed cabinet refused.' }
        for ($i=0;$i -lt $folders;$i++) {
            $data=$reader.ReadUInt32(); $blocks=$reader.ReadUInt16(); $compression=$reader.ReadUInt16()
            if ($data -ge $length -or -not $blocks -or ($compression -band 15) -ne 3 -or ($compression -band 0xe0f0) -ne 0 -or (($compression -shr 8) -band 31) -lt 15 -or (($compression -shr 8) -band 31) -gt 21) { throw 'Unsupported cabinet folder.' }
        }
        $reader.BaseStream.Position=$offset; $seen=@{}
        for ($i=0;$i -lt $count;$i++) {
            $bytes=$reader.ReadUInt32(); $null=$reader.ReadUInt32(); $folder=$reader.ReadUInt16()
            $null=$reader.ReadUInt16(); $null=$reader.ReadUInt16(); $attributes=$reader.ReadUInt16()
            $name=$reader.ReadBytes(7)
            if ($name.Length -ne 7 -or $name[6] -ne 0) { throw 'Cabinet name is not a fixed alias.' }
            $alias=[Text.Encoding]::ASCII.GetString($name,0,6)
            if ($alias -notmatch '^f[0-9]{5}$' -or $seen.ContainsKey($alias)) { throw 'Unsafe or duplicate cabinet alias.' }
            $index=[int]$alias.Substring(1)
            if ($index -ge $count -or $folder -ge $folders -or ($attributes -band 0xff58) -ne 0 -or $bytes -ne [long]$Files[$index].bytes) { throw 'Cabinet entry differs from its manifest.' }
            $seen[$alias]=$true
        }
    } finally { $reader.Dispose() }
}

function Expand-HarmonyCabinet([string]$Cabinet, [string]$Manifest, [string]$Destination, [scriptblock]$Progress) {
    $manifestFile=Get-Item -Force -LiteralPath $Manifest
    if ($manifestFile.Length -gt 4MB) { throw 'Cabinet manifest limit exceeded.' }
    $index=[IO.File]::ReadAllText($Manifest) | ConvertFrom-Json
    if ($index.format -cne 'harmony-cabinet-lzx-1') { throw 'Unsupported payload format.' }
    $files=@($index.files)
    Test-HarmonyCabinet $Cabinet $files $Destination
    $aliases=Join-Path $Destination '.cab-files'
    if (Test-Path -LiteralPath $aliases) { throw 'Cabinet staging already exists.' }
    [IO.Directory]::CreateDirectory($aliases) | Out-Null
    if ($Progress) { & $Progress 0 $files.Count '' }
    $arguments='"{0}" -F:* "{1}"' -f $Cabinet,$aliases
    $process=Start-Process -FilePath "$env:windir\System32\expand.exe" -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $Destination 'cab-expand.log') -RedirectStandardError (Join-Path $Destination 'cab-expand.err')
    $null=$process.Handle
    if (-not $process.WaitForExit(120000)) { $process.Kill(); throw 'Cabinet extraction timed out.' }
    if ($process.ExitCode -ne 0 -or @(Get-ChildItem -LiteralPath $aliases -Force).Count -ne $files.Count) { throw 'Cabinet extraction failed.' }
    for ($i=0;$i -lt $files.Count;$i++) {
        $file=$files[$i]; $source=Join-Path $aliases ('f{0:D5}' -f $i)
        $actual=Get-Item -Force -LiteralPath $source
        if ($actual.PSIsContainer -or ($actual.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $actual.Length -ne [long]$file.bytes -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) { throw 'Cabinet file integrity check failed.' }
        $target=[IO.Path]::GetFullPath((Join-Path $Destination $file.path))
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.File]::Move($source,$target)
        if ($Progress) { & $Progress ($i+1) $files.Count ([IO.Path]::GetFileName($target)) }
    }
    [IO.Directory]::Delete($aliases)
}
