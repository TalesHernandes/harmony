# Cancel observers before requesting exclusive live-file handles.
function Enter-HarmonyMutation([string]$Root) {
    $name='Local\harmony-native-observer-'+([IO.Path]::GetFullPath($Root).TrimEnd('\').ToLowerInvariant() -replace '[\\/:]','_')
    $cancel=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($name+'-cancel'))
    $gate=[Threading.Mutex]::new($false,$name)
    try {
        # Signal again while waiting: an observer taking the gate just now clears the event once.
        $owned=$false
        for($i=0; $i -lt 300 -and -not $owned; $i++) {
            $null=$cancel.Set()
            try { $owned=$gate.WaitOne(100) } catch [Threading.AbandonedMutexException] { $owned=$true }
        }
        if(-not $owned) { throw 'Observer cancellation timed out.' }
        return $gate
    } catch { $gate.Dispose(); throw }
    finally { $cancel.Dispose() }
}
# The versioned depot remains the authority for the fixed executable path.
function Assert-HarmonyTree([string]$Path) {
    for ($parent=[IO.Path]::GetFullPath($Path); $parent; $parent=[IO.Path]::GetDirectoryName($parent)) {
        $item=Get-Item -Force -LiteralPath $parent
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point refused.' }
    }
    foreach ($item in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point refused.' }
    }
}
function Read-HarmonyManifest([string]$Depot, [int]$Version) {
    Assert-HarmonyTree $Depot
    $manifest=[IO.File]::ReadAllText((Join-Path $Depot 'manifest.json')) | ConvertFrom-Json
    if ($Version -lt 1 -or -not $manifest.complete -or $manifest.version -ne $Version -or $manifest.files -isnot [Array]) { throw 'Incomplete version refused.' }
    return $manifest
}
function Test-HarmonyVersion([string]$Directory, $Manifest, [string]$Depot) {
    Assert-HarmonyTree $Directory
    $seen=@{}
    $hasher=[Security.Cryptography.SHA256]::Create()
    try { foreach ($entry in $Manifest.files) {
        $relative=[string]$entry.path
        if (-not $relative -or [IO.Path]::IsPathRooted($relative)) { throw 'Manifest path outside version refused.' }
        foreach ($part in $relative.Split([char[]]'\/')) {
            if (-not $part -or $part -in @('.','..') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.Contains(':')) { throw 'Manifest path outside version refused.' }
        }
        $file=[IO.Path]::GetFullPath((Join-Path $Directory $relative))
        if (-not $file.StartsWith($Directory+'\',[StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($file) -or $relative -ieq 'manifest.json') { throw 'Duplicate or reserved manifest entry.' }
        $seen[$file]=$true
        $item=Get-Item -Force -LiteralPath $file
        if ($item.PSIsContainer -or $entry.bytes -lt 0 -or $item.Length -ne $entry.bytes -or $entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw "Invalid installed file: $relative" }
        $stream=[IO.File]::OpenRead($file)
        try { $hash=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
        finally { $stream.Dispose() }
        if ($hash -ne $entry.sha256) { throw "Invalid installed file: $relative" }
    } } finally { $hasher.Dispose() }
    if (-not $seen.ContainsKey((Join-Path $Directory 'harmony-nativo.exe'))) { throw 'harmony-nativo.exe missing from manifest.' }
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File -Force) {
        if ($file.FullName -ine (Join-Path $Directory 'manifest.json') -and -not $seen.ContainsKey($file.FullName)) { throw 'Unlisted installed file refused.' }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $Directory 'manifest.json')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $Depot 'manifest.json')).Hash) { throw 'Live manifest differs from depot.' }
}
function Promote-HarmonyVersion([string]$Root, [int]$Version) {
    $observer=Enter-HarmonyMutation $Root
    try {
    $launchRecord=(Join-Path $Root 'verified-launch.json'); if([IO.File]::Exists($launchRecord)) { [IO.File]::Delete($launchRecord) }
    $depot=Join-Path $Root "app-$Version"
    $manifest=Read-HarmonyManifest $depot $Version
    Test-HarmonyVersion $depot $manifest $depot
    $live=Join-Path $Root 'app'
    if (Test-Path -LiteralPath $live) {
        Assert-HarmonyTree $live
        # A direct installer invocation must respect the same closed-app rule.
        foreach ($process in Get-Process) {
            try { $path=$process.Path } catch { if ($process.ProcessName -eq 'harmony-nativo') { throw 'Close the native application before promotion.' }; continue }
            if ($path -and $path.StartsWith($live+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Close the native application before promotion.' }
        }
        foreach ($file in Get-ChildItem -LiteralPath $live -Recurse -File -Force) {
            $handle=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
            $handle.Dispose()
        }
    }
    $stage=Join-Path $Root ('.staging-app-'+[Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    foreach ($entry in $manifest.files) {
        $source=Join-Path $depot $entry.path; $target=Join-Path $stage $entry.path
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        try { New-Item -ItemType HardLink -Path $target -Target $source -ErrorAction Stop | Out-Null }
        catch { Copy-Item -LiteralPath $source -Destination $target }
    }
    Copy-Item -LiteralPath (Join-Path $depot 'manifest.json') -Destination (Join-Path $stage 'manifest.json')
    Test-HarmonyVersion $stage $manifest $depot
    if (Test-Path -LiteralPath $live) { [IO.Directory]::Move($live,(Join-Path $Root ('.retired-app-'+[Guid]::NewGuid().ToString('N')))) }
    # A crash here is repaired from the still-selected depot on the next launch.
    [IO.Directory]::Move($stage,$live)
    } finally { $observer.ReleaseMutex(); $observer.Dispose() }
}
