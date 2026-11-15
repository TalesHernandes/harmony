[CmdletBinding()]
param(
    [ValidateRange(1,2147483647)][int]$Versao = 1,
    [string]$Origem,
    [string]$Destino = (Join-Path $env:LOCALAPPDATA 'Programs\harmony-nativo'),
    [string]$Relatorio,
    [switch]$Remover,
    [switch]$Limpar,
    [switch]$PreservarAnterior,
    [string]$ProgressFile,
    [switch]$SomenteArquivos
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'live-app.ps1')
if (-not $Origem) { $Origem = Join-Path $PSScriptRoot '..\out' }
$timer = [Diagnostics.Stopwatch]::StartNew()
$root = [IO.Path]::GetFullPath($Destino).TrimEnd('\')
$marker = Join-Path $root '.harmony-native-install'
$lock = $null
$observer = $null
$stage = $null
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Harmony.Native.Windows'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\harmony (nativo).lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Harmony Nativo.lnk'
$previousVersion = 0
$firstInstall = -not (Test-Path -LiteralPath (Join-Path $root 'current.json'))

function Register-Native([string]$Executable) {
    if ($SomenteArquivos) { return }
    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($shortcut)) | Out-Null
    $p = Start-Process -FilePath $Executable -ArgumentList ('--atalho "{0}" "{1}"' -f $shortcut,$Executable) -WindowStyle Hidden -PassThru
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw 'Native shortcut creation failed.' }
    # The desktop shortcut is the owner's choice: refresh it only when it exists, since
    # it may still name a versioned folder from before the fixed-path migration.
    if (Test-Path -LiteralPath $desktopShortcut) {
        $p = Start-Process -FilePath $Executable -ArgumentList ('--atalho "{0}" "{1}"' -f $desktopShortcut,$Executable) -WindowStyle Hidden -PassThru
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) { Write-Warning 'Desktop shortcut refresh failed.' }
    }
    New-Item -Path $uninstallKey -Force | Out-Null
    $remove = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -Destino "{1}" -Remover' -f (Join-Path $root 'instalar.ps1'),$root
    foreach ($field in @{
        DisplayName='harmony (nativo)'; DisplayVersion=[string]$Versao; Publisher='harmony'
        InstallLocation=$root; DisplayIcon=$Executable; UninstallString=$remove; QuietUninstallString=$remove
    }.GetEnumerator()) { New-ItemProperty -Path $uninstallKey -Name $field.Key -Value $field.Value -PropertyType String -Force | Out-Null }
    New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
    $enabled = $firstInstall -or ($null -ne (Get-ItemProperty -Path $runKey -Name 'harmony-nativo' -ErrorAction SilentlyContinue))
    if ($enabled) {
        if (-not (Test-Path -LiteralPath $runKey)) { New-Item -Path $runKey -Force | Out-Null }
        New-ItemProperty -Path $runKey -Name 'harmony-nativo' -Value ('powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -Bandeja' -f (Join-Path $root 'launch.ps1')) -PropertyType String -Force | Out-Null
    }
}
function Clear-NativeNotices {
    # Inbox WinRT, independent of the Windows App SDK registration path.
    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime]::History.Clear('Harmony.Native.Windows')
}
function Unregister-Native {
    if ($SomenteArquivos) { return }
    try { Clear-NativeNotices } catch { Write-Warning 'Could not clear native notification history.' }
    # Keep this stable CLSID in sync with platform.h; Audit owns another CLSID.
    $toastClass = 'HKCU:\Software\Classes\CLSID\{549D187E-812E-46F4-9AE0-D754C89F43A1}'
    if (Test-Path -LiteralPath $toastClass) { Remove-Item -LiteralPath $toastClass -Recurse -Force }
    if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
    if (Test-Path -LiteralPath $desktopShortcut) { Remove-Item -LiteralPath $desktopShortcut -Force }
    if (Test-Path -LiteralPath $uninstallKey) { Remove-Item -LiteralPath $uninstallKey -Force }
    Remove-ItemProperty -Path $runKey -Name 'harmony-nativo' -ErrorAction SilentlyContinue
}

function Assert-Tree([string]$Path) {
    Assert-HarmonyTree $Path
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
        } catch { if ($process.ProcessName -in @('harmony-nativo')) { return $false } }
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
    [IO.File]::WriteAllText($temp, $Text, [Text.UTF8Encoding]::new($Path.EndsWith('.ps1')))
    if (Test-Path -LiteralPath $Path) {
        $backup = $temp + '.previous'
        [IO.File]::Replace($temp, $Path, $backup)
        Remove-Item -LiteralPath $backup -Force
    }
    else { [IO.File]::Move($temp, $Path) }
}
try {
    for ($ancestor=$root; $ancestor; $ancestor=[IO.Path]::GetDirectoryName($ancestor)) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -Force -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse ancestor refused.' }
    }
    if ($root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { throw 'Drive root refused.' }
    if (Test-Path -LiteralPath $root) {
        Assert-Tree $root
        if (-not (Test-Path -LiteralPath $marker)) { throw 'Destination is not an installation owned by this proof.' }
    } elseif ($Remover) { return }
    else {
        New-Item -ItemType Directory -Path $root | Out-Null
        [IO.File]::WriteAllText($marker, 'Harmony Native installation v1')
    }
    $observer=Enter-HarmonyMutation $root
    $launchRecord=(Join-Path $root 'verified-launch.json'); if([IO.File]::Exists($launchRecord)) { [IO.File]::Delete($launchRecord) }
    $lock = [IO.File]::Open((Join-Path $root '.install.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $pointer = Join-Path $root 'current.json'
    if (-not $Remover -and (Test-Path -LiteralPath $pointer)) {
        $selected=Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        if ($selected.version -lt 1 -or $selected.directory -cne "app-$([int]$selected.version)") { throw 'Invalid version pointer.' }
        $depot=Join-Path $root $selected.directory
        if ($Limpar -or $Versao -le $selected.version) {
            $manifest=Read-HarmonyManifest $depot $selected.version
            try { Test-HarmonyVersion (Join-Path $root 'app') $manifest $depot }
            catch { Promote-HarmonyVersion $root $selected.version }
            if ($selected.live -cne 'app') {
                $selected | Add-Member -NotePropertyName live -NotePropertyValue app -Force
                Write-Atomic $pointer ($selected | ConvertTo-Json)
            }
        }
    }
    if ($Limpar -and -not $Remover) {
        $current = Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        $depot=Join-Path $root $current.directory
        $manifest=Read-HarmonyManifest $depot $current.version
        Test-HarmonyVersion (Join-Path $root 'app') $manifest $depot
        $previousVersion=@(Get-ChildItem -LiteralPath $root -Directory -Force | Where-Object { $_.Name -match '^app-(\d+)$' -and [int]$Matches[1] -lt [int]$current.version } | ForEach-Object { [int]$_.Name.Substring(4) } | Sort-Object -Descending | Select-Object -First 1)
        foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force) {
            if (($dir.Name -match '^app-(\d+)$' -and [int]$Matches[1] -ne [int]$current.version -and [int]$Matches[1] -notin $previousVersion) -or $dir.Name -like '.staging-*' -or $dir.Name -like '.retired-app-*') {
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
        Unregister-Native
        return
    }
    $source = (Resolve-Path -LiteralPath $Origem).Path
    Assert-Tree $source
    foreach($artifact in @(
        @{name='df.dll';bytes=16678912;hash='4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f'},
        @{name='DeepFilterNet3_onnx.tar.gz';bytes=7983136;hash='c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616'})) {
        $file=Join-Path $source $artifact.name
        if((Get-Item -Force -LiteralPath $file).Length -ne $artifact.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.hash) { throw "Invalid DFN package artifact: $($artifact.name)" }
    }
    foreach($license in @('LICENSE','LICENSE-MIT','LICENSE-APACHE')) {
        if(-not (Test-Path -LiteralPath (Join-Path $source "licenses/DeepFilterNet-0.5.6/$license") -PathType Leaf)) { throw 'DFN licenses missing.' }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $source 'harmony-nativo.exe') -PathType Leaf)) { throw 'harmony-nativo.exe not found in source.' }
    if (Test-Path -LiteralPath $pointer) {
        $current = Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json
        $previousVersion = [int]$current.version
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
    $sourceFiles=@(Get-ChildItem -LiteralPath $source -Recurse -File -Force)
    $index=0
    foreach ($file in $sourceFiles) {
        $index++
        if ($ProgressFile) { Report-InstallProgress (30 + [int](25*$index/$sourceFiles.Count)) 2 ("Copiando arquivos — $index de $($sourceFiles.Count) ($($file.Name))") }
        $relative = $file.FullName.Substring($source.TrimEnd('\').Length + 1)
        if ($relative -in @('manifest.json')) { throw "Reserved file name in source: $relative" }
        $target = Join-Path $stage $relative
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($target)) | Out-Null
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $file.FullName -Destination $target
        $entries += [ordered]@{ path=$relative; bytes=$file.Length; sha256=$hash }
    }
    $index=0
    foreach ($entry in $entries) {
        $index++
        if ($ProgressFile) { Report-InstallProgress (55 + [int](25*$index/$entries.Count)) 3 ("Conferindo a integridade dos arquivos — $index de $($entries.Count)") }
        $file = Join-Path $stage $entry.path
        if ((Get-Item -Force -LiteralPath $file).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "SHA-256 verification failed: $($entry.path)" }
    }
    $manifest = [ordered]@{ schema=1; version=$Versao; complete=$true; files=$entries }
    [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    # Rename is on the same volume; incomplete staging is never referenced.
    # Windows can briefly deny a directory rename after every file has been
    # closed. Retry the same verified tree without ever selecting staging.
    $moveDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($true) {
        try { [IO.Directory]::Move($stage, $final); $stage = $null; break }
        catch {
            if ([DateTime]::UtcNow -ge $moveDeadline) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
    Promote-HarmonyVersion $root $Versao
    Write-Atomic (Join-Path $root 'live-app.ps1') ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'live-app.ps1')))
    Write-Atomic (Join-Path $root 'launch.ps1') ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'launch.ps1')))
    Write-Atomic (Join-Path $root 'instalar.ps1') ([IO.File]::ReadAllText($PSCommandPath))
    Write-Atomic (Join-Path $root 'harmony.cmd') "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0launch.ps1`" %*`r`n"
    Write-Atomic $pointer (([ordered]@{version=$Versao; directory="app-$Versao"; live="app"} | ConvertTo-Json))
    Register-Native (Join-Path $root 'app\harmony-nativo.exe')
    $deferred = @()
    foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force) {
        if ($dir.Name -match '^app-(\d+)$' -and [int]$Matches[1] -lt $Versao -and [int]$Matches[1] -ne $previousVersion) {
            if (-not (Remove-Old $dir.FullName)) { $deferred += $dir.Name }
        } elseif ($dir.Name -like '.staging-*' -or $dir.Name -like '.retired-app-*') {
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
        elevation_requested=$false; registry_writes=(-not $SomenteArquivos)
        identity_is_admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    $json = $result | ConvertTo-Json -Depth 4
    if ($Relatorio) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($Relatorio), $json, [Text.UTF8Encoding]::new($false)) }
    $json
} finally {
    if ($lock) { $lock.Dispose() }
    if ($observer) { $observer.ReleaseMutex(); $observer.Dispose() }
    # On interruption or failure, staging stays for inspection/retry; current.json is unchanged.
}
