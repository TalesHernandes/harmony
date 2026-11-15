[CmdletBinding()]
param(
    [ValidateRange(1,2147483647)][int]$Versao = 1,
    [string]$Origem,
    [string]$Harmony = "$PSScriptRoot\..\build\harmony-w1.exe",
    [string]$Destino = (Join-Path $env:LOCALAPPDATA 'Programs\harmony-prova'),
    [string]$Relatorio,
    [switch]$Remover,
    [switch]$Limpar
)
$ErrorActionPreference = 'Stop'
if (-not $Origem) { $Origem = Join-Path $PSScriptRoot '..\telas\out' }
$timer = [Diagnostics.Stopwatch]::StartNew()
$root = [IO.Path]::GetFullPath($Destino).TrimEnd('\')
$marker = Join-Path $root '.harmony-proof-install'
$lock = $null
$stage = $null

function Assert-Tree([string]$Path) {
    if ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $Path" }
    foreach ($item in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $($item.FullName)" }
    }
}
function Assert-Child([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not $resolved.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Operation outside destination refused.' }
}
function Remove-Old([string]$Path) {
    Assert-Child $Path
    Assert-Tree $Path
    # Do not partially remove a running version. Unknown process state is deferred too.
    foreach ($process in Get-Process) {
        try {
            if ($process.Path -and $process.Path.StartsWith($Path + '\', [StringComparison]::OrdinalIgnoreCase)) { return $false }
        } catch { if ($process.ProcessName -in @('telas','harmony-w1')) { return $false } }
    }
    $handles = @()
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -Force) {
            $handles += [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        }
    } catch { return $false }
    finally { foreach ($handle in $handles) { $handle.Dispose() } }
    try { Remove-Item -LiteralPath $Path -Recurse -Force; return $true }
    catch [IO.IOException] { return $false }
    catch [UnauthorizedAccessException] { return $false }
}
function Write-Atomic([string]$Path, [string]$Text) {
    Assert-Child $Path
    $temp = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    [IO.File]::WriteAllText($temp, $Text, [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $Path) {
        $backup = $temp + '.previous'
        [IO.File]::Replace($temp, $Path, $backup)
        Remove-Item -LiteralPath $backup -Force
    }
    else { [IO.File]::Move($temp, $Path) }
}
try {
    if ($root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { throw 'Drive root refused.' }
    if (Test-Path -LiteralPath $root) {
        Assert-Tree $root
        if (-not (Test-Path -LiteralPath $marker)) { throw 'Destination is not an installation owned by this proof.' }
    } elseif ($Remover) { return }
    else {
        New-Item -ItemType Directory -Path $root | Out-Null
        [IO.File]::WriteAllText($marker, 'Harmony W1 installation proof v1')
    }
    $lock = [IO.File]::Open((Join-Path $root '.install.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $pointer = Join-Path $root 'current.json'
    if ($Limpar -and -not $Remover) {
        $current = Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force) {
            if (($dir.Name -match '^app-(\d+)$' -and [int]$Matches[1] -lt [int]$current.version) -or $dir.Name -like '.staging-*') {
                if (-not (Remove-Old $dir.FullName)) { Write-Output "Deferred: $($dir.Name)" }
            }
        }
        return
    }
    if ($Remover) {
        $busy = @()
        foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force) {
            if (-not (Remove-Old $dir.FullName)) { $busy += $dir.Name }
        }
        if ($busy.Count) { throw "Close applications and retry removal; retained: $($busy -join ', ')" }
        $lock.Dispose(); $lock = $null
        # The exact absolute root was checked above and is marked as owned.
        Remove-Item -LiteralPath $root -Recurse -Force
        return
    }
    $source = (Resolve-Path -LiteralPath $Origem).Path
    Assert-Tree $source
    if (-not (Test-Path -LiteralPath (Join-Path $source 'telas.exe') -PathType Leaf)) { throw 'telas.exe not found in source.' }
    $harmonyFile = Get-Item -LiteralPath $Harmony
    if ($harmonyFile.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Harmony source reparse point refused.' }
    if (Test-Path -LiteralPath $pointer) {
        $current = Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        if ($Versao -le [int]$current.version) { throw 'Versions are immutable; select a newer version.' }
    }
    $final = Join-Path $root "app-$Versao"
    if (Test-Path -LiteralPath $final) {
        # A crash after the directory rename but before the pointer switch can leave
        # a complete, unselected version. Recopy it only when it is not in use.
        if (-not (Remove-Old $final)) { throw 'Unselected version is in use; retry after closing it.' }
    }
    $stage = Join-Path $root ('.staging-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $entries = @()
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File -Force) {
        $relative = $file.FullName.Substring($source.TrimEnd('\').Length + 1)
        if ($relative -in @('manifest.json','harmony-w1.exe')) { throw "Reserved file name in source: $relative" }
        $target = Join-Path $stage $relative
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($target)) | Out-Null
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $file.FullName -Destination $target
        $entries += [ordered]@{ path=$relative; bytes=$file.Length; sha256=$hash }
    }
    $hash = (Get-FileHash -LiteralPath $harmonyFile.FullName -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $harmonyFile.FullName -Destination (Join-Path $stage 'harmony-w1.exe')
    $entries += [ordered]@{ path='harmony-w1.exe'; bytes=$harmonyFile.Length; sha256=$hash }
    foreach ($entry in $entries) {
        $file = Join-Path $stage $entry.path
        if ((Get-Item -LiteralPath $file).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "SHA-256 verification failed: $($entry.path)" }
    }
    $manifest = [ordered]@{ schema=1; version=$Versao; complete=$true; files=$entries }
    [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    # Rename is on the same volume; incomplete staging is never referenced.
    [IO.Directory]::Move($stage, $final); $stage = $null
    Write-Atomic (Join-Path $root 'launch.ps1') ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'launch.ps1')))
    Write-Atomic (Join-Path $root 'harmony.cmd') "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0launch.ps1`"`r`n"
    Write-Atomic $pointer (([ordered]@{version=$Versao; directory="app-$Versao"} | ConvertTo-Json))
    $deferred = @()
    foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force) {
        if ($dir.Name -match '^app-(\d+)$' -and [int]$Matches[1] -lt $Versao) {
            if (-not (Remove-Old $dir.FullName)) { $deferred += $dir.Name }
        } elseif ($dir.Name -like '.staging-*') {
            if (-not (Remove-Old $dir.FullName)) { $deferred += $dir.Name }
        }
    }
    $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)
    $activeFiles = @(Get-ChildItem -LiteralPath $final -Recurse -File -Force)
    $result = [ordered]@{
        version=$Versao; destination=$root; seconds=$timer.Elapsed.TotalSeconds
        installed_mb=($files | Measure-Object Length -Sum).Sum / 1MB
        file_count=$files.Count; sha256_verified=$true; deferred_versions=$deferred
        active_version_mb=($activeFiles | Measure-Object Length -Sum).Sum / 1MB
        active_version_file_count=$activeFiles.Count
        elevation_requested=$false; registry_writes=$false
        identity_is_admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    $json = $result | ConvertTo-Json -Depth 4
    if ($Relatorio) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($Relatorio), $json, [Text.UTF8Encoding]::new($false)) }
    $json
} finally {
    if ($lock) { $lock.Dispose() }
    # On interruption or failure, staging stays for inspection/retry; current.json is unchanged.
}
