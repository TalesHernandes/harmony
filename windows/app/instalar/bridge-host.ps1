param([Parameter(Mandatory=$true)][string]$Root, [string]$Package, [string]$Bridge, [int]$Release, [switch]$Remove, [switch]$Recover, [switch]$Tray, [switch]$Fresh, [switch]$Desktop, [switch]$Startup, [switch]$Interactive, [switch]$Reinstall, [switch]$CleanBroken, [string]$ProgressFile)
$ErrorActionPreference = 'Stop'
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

$operationClock = [Diagnostics.Stopwatch]::StartNew()
$root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$test = $env:HARMONY_INSTALLER_TEST_ROOT
# The on-disk fixture marker survives launches without inherited environment.
for ($ancestor=$root; $ancestor; $ancestor=[IO.Path]::GetDirectoryName($ancestor)) {
    if ((Test-Path -LiteralPath (Join-Path $ancestor '.harmony-installer-test')) -and -not $test) { throw 'Fixture lost isolated context.' }
}
if ($test) {
    $test = [IO.Path]::GetFullPath($test).TrimEnd('\')
    if (-not (Test-Path -LiteralPath (Join-Path $test '.harmony-installer-test')) -or $env:HARMONY_URL -notmatch '^http://(127\.0\.0\.1|localhost):\d+$' -or -not $root.StartsWith($test + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid isolated installer context.' }
    $identity = 'HKCU:\Software\harmony-installer-tests\' + [IO.Path]::GetFileName($test)
    $uninstall = "$identity\Uninstall\harmony"
    $run = "$identity\Run"
    $links = @((Join-Path $test 'shortcuts\start\harmony.lnk'), (Join-Path $test 'shortcuts\desktop\harmony.lnk'))
} else {
    $expected = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs\harmony'
    # InstallLocation is persisted below; each launcher/updater carries its own root.
    # A fresh custom root has the installer marker before this helper starts.
    $uninstall = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\harmony'
    $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $links = @((Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\harmony.lnk'), (Join-Path ([Environment]::GetFolderPath('Desktop')) 'harmony.lnk'))
}
# Refuse protected, remote, root and reparse destinations independently of the UI.
$volume=[IO.Path]::GetPathRoot($root)
if ($root -notmatch '^[A-Za-z]:\\' -or $root -eq $volume.TrimEnd('\') -or ([IO.DriveInfo]::new($volume)).DriveType -ne [IO.DriveType]::Fixed) { throw 'Local directory required.' }
foreach ($protected in @($env:ProgramFiles,${env:ProgramFiles(x86)},$env:windir)) {
    if ($protected -and ($root -eq $protected -or $root.StartsWith($protected+'\',[StringComparison]::OrdinalIgnoreCase))) { throw 'Administrator directory refused.' }
}
function Assert-Tree([string]$Path) {
    for ($parent=[IO.Path]::GetFullPath($Path); $parent -and $parent -ne [IO.Path]::GetPathRoot($parent); $parent=[IO.Path]::GetDirectoryName($parent)) {
        if ((Get-Item -Force -LiteralPath $parent).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse ancestor refused.' }
    }
    if ((Get-Item -Force -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point refused.' }
    foreach ($entry in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point refused.' }
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $root '.harmony-installer'))) { throw 'Installer marker missing.' }
$hash = [Security.Cryptography.SHA256]::Create()
try { $lockID = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))).Replace('-','') } finally { $hash.Dispose() }
$transition = [Threading.Mutex]::new($false, ('Local\harmony-installer-install-' + $lockID))
$ownsTransition = $false
$observer = $null
try {
    try { $ownsTransition = $transition.WaitOne(180000) } catch [Threading.AbandonedMutexException] { $ownsTransition = $true }
    if (-not $ownsTransition) { throw 'Another installation is still finishing; retry removal later.' }
$observer=Enter-HarmonyMutation (Join-Path $root 'native')
$launchRecord=(Join-Path $root 'native/verified-launch.json'); if([IO.File]::Exists($launchRecord)) { [IO.File]::Delete($launchRecord) }
Assert-Tree $root
$native = Join-Path $root 'native'
$stable = Join-Path $root 'harmony.exe'
$journal = Join-Path $root 'recovery.json'
function Remove-Owned([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if ($resolved -ne $root -and -not $resolved.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Removal outside installation refused.' }
    if (Test-Path -LiteralPath $resolved) { Assert-Tree $resolved; Remove-Item -LiteralPath $resolved -Recurse -Force }
}
# Shared by archive extraction, verified copying and Windows registration.
# Atomic replacement prevents the UI from observing a partial JSON record.
function Report-InstallProgress([int]$Value, [int]$Step, [string]$Status) {
    if (-not $ProgressFile) { return }
    $text = @{percent=$Value;step=$Step;status=$Status} | ConvertTo-Json -Compress
    $pending = $ProgressFile + '.pending'
    [IO.File]::WriteAllText($pending,$text,[Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($ProgressFile)) { [IO.File]::Replace($pending,$ProgressFile,($ProgressFile + '.previous')); [IO.File]::Delete($ProgressFile + '.previous') }
    else { [IO.File]::Move($pending,$ProgressFile) }
}

if ($CleanBroken) {
    if (Test-Path -LiteralPath (Join-Path $native 'current.json')) { throw 'Selected installation cannot be cleaned as debris.' }
    foreach ($link in $links) {
        if ((Test-Path -LiteralPath $link) -and (New-Object -ComObject WScript.Shell).CreateShortcut($link).TargetPath -eq $stable) { Remove-Item -LiteralPath $link -Force }
    }
    if ((Get-ItemProperty -Path $run -Name harmony -ErrorAction SilentlyContinue).harmony -like ('*"{0}"*' -f $stable)) { Remove-ItemProperty -Path $run -Name harmony -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $uninstall) {
        $old = Get-ItemProperty -LiteralPath $uninstall
        if ($old.InstallLocation -eq $root -or $old.UninstallString -like ('*' + $root + '*') -or $old.DisplayIcon -like ('*' + $stable + '*')) { Remove-Item -LiteralPath $uninstall -Recurse -Force }
        else { throw 'Unrelated uninstall identity refused.' }
    }
    foreach ($item in Get-ChildItem -LiteralPath $root -Force) {
        if ($item.Name -notlike '.installer-*') { Remove-Owned $item.FullName }
    }
    [IO.File]::WriteAllText((Join-Path $root '.harmony-installer'), 'Harmony installer v1')
    exit 0
}
function Restore-State($State) {
    if ($State.backup -and (Test-Path -LiteralPath $State.backup)) {
        if (-not ([IO.Path]::GetFullPath($State.backup)).StartsWith($root + '\.installer-', [StringComparison]::OrdinalIgnoreCase)) { throw 'Recovery backup outside root refused.' }
        Assert-Tree $State.backup
        foreach ($file in Get-ChildItem -LiteralPath $State.backup -Recurse -File) {
            $relative = $file.FullName.Substring($State.backup.Length + 1)
            if ($relative.StartsWith('previous-app\')) { $relative = 'native\app-' + $State.release + '\' + $relative.Substring(13) }
            $target = Join-Path $root $relative
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
    }
    $pointer = Join-Path $native 'current.json'
    if ($State.previous) {
        . (Join-Path $native 'live-app.ps1')
        $selection=[string]$State.previous | ConvertFrom-Json
        if ($selection.directory -cne "app-$([int]$selection.version)") { throw 'Invalid rollback pointer.' }
        Promote-HarmonyVersion $native $selection.version
        $selection | Add-Member -NotePropertyName live -NotePropertyValue app -Force
        $State.previous=$selection | ConvertTo-Json
        $pending = $pointer + '.rollback'
        [IO.File]::WriteAllText($pending, [string]$State.previous)
        if (Test-Path -LiteralPath $pointer) {
            # Windows PowerShell marshals $null to an empty string here. Use a
            # real, owned backup path rather than an invalid empty pathname.
            $backup = $pointer + '.before-rollback'
            [IO.File]::Replace($pending, $pointer, $backup)
            Remove-Item -LiteralPath $backup -Force
        }
        else { [IO.File]::Move($pending, $pointer) }
    } elseif (Test-Path -LiteralPath $pointer) { Remove-Item -LiteralPath $pointer -Force }
    foreach ($path in @($uninstall, $run)) {
        $fields = $State.registry.PSObject.Properties[$path].Value
        $names = if ($path -eq $run) { @('harmony') } else { @('DisplayName','DisplayVersion','Publisher','InstallLocation','DisplayIcon','UninstallString','QuietUninstallString','NoModify','NoRepair') }
        foreach ($name in $names) {
            $field = $fields.PSObject.Properties[$name]
            if ($field) {
                if (-not (Test-Path -LiteralPath $path)) { New-Item -Path $path -Force | Out-Null }
                New-ItemProperty -Path $path -Name $name -Value $field.Value.value -PropertyType ([Microsoft.Win32.RegistryValueKind][int]$field.Value.kind) -Force | Out-Null
            } else { Remove-ItemProperty -Path $path -Name $name -ErrorAction SilentlyContinue }
        }
    }
    foreach ($link in $links) {
        $backup = $State.links.PSObject.Properties[$link]
        if ($backup) { [IO.File]::WriteAllBytes($link,[Convert]::FromBase64String($backup.Value)) }
        elseif (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
    }
    if (-not @($State.registry.PSObject.Properties[$uninstall].Value.PSObject.Properties).Count -and (Test-Path -LiteralPath $uninstall)) { Remove-Item -LiteralPath $uninstall -Recurse -Force }
}
if ($Recover) {
    $state = Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json
    # The unconfirmed process may have survived its parent. Stop only that
    # installation's version, before restoring a durable previous selection.
    $candidate = Join-Path $native ("app-{0}" -f [int]$state.release)
    foreach ($process in Get-Process) {
        try { $path = $process.Path } catch { continue }
        if ($path -and ($path.StartsWith($candidate + '\',[StringComparison]::OrdinalIgnoreCase) -or $path.StartsWith($native + '\app\',[StringComparison]::OrdinalIgnoreCase))) { Stop-Process -Id $process.Id -Force; $process.WaitForExit() }
    }
    if ($state.backup) { Remove-Owned $candidate }
    Restore-State $state
    [IO.File]::WriteAllText((Join-Path $root 'failed-release.json'), (@{version=[int]$state.release} | ConvertTo-Json))
    Remove-Item -LiteralPath $journal -Force
    Write-Output 'Installer: interrupted transaction restored.'
    exit 0
}
if ($Remove) {
    Report-InstallProgress 2 0 'Encerrando o harmony'
    # Stop only programs whose resolved executable belongs to this installation.
    foreach ($process in Get-Process) {
        try { $processPath = $process.Path } catch { continue }
        if ($process.Id -ne $PID -and $processPath -and $processPath.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
            $process.WaitForExit()
        }
    }
    # Recheck immediately before recursively removing the exact marked install root.
    Assert-Tree $root
    # Both locations are fixed by the validated shell/test context, never by a
    # package manifest or an arbitrary command-line data-directory argument.
    $legacyData = $null # The old profile is never owned by this installer.
    $nativeData = if ($test) { Join-Path $test 'data\native' } else { $null }
    foreach ($data in @($legacyData, $nativeData)) {
        if ($data -and (Test-Path -LiteralPath $data)) { Assert-Tree $data; Remove-Item -LiteralPath $data -Recurse -Force }
    }
    $files=@(Get-ChildItem -LiteralPath $root -Recurse -File -Force)
    $index=0
    foreach ($file in $files) {
        $index++
        Report-InstallProgress (5 + [int](65*$index/[Math]::Max(1,$files.Count))) 1 ("Removendo arquivos — $index de $($files.Count) ($($file.Name))")
        Remove-Item -LiteralPath $file.FullName -Force
    }
    Remove-Owned $root
    Report-InstallProgress 75 2 'Removendo os atalhos do Menu Iniciar e da área de trabalho'
    foreach ($link in $links) {
        if (Test-Path -LiteralPath $link) {
            $target = (New-Object -ComObject WScript.Shell).CreateShortcut($link).TargetPath
            if ($target -eq $stable) { Remove-Item -LiteralPath $link -Force }
        }
    }
    Report-InstallProgress 85 3 'Removendo o início com o Windows'
    if ((Get-ItemProperty -Path $run -Name harmony -ErrorAction SilentlyContinue).harmony -eq ('"{0}" --tray' -f $stable)) { Remove-ItemProperty -Path $run -Name harmony -ErrorAction SilentlyContinue }
    Report-InstallProgress 92 4 'Removendo o registro em Aplicativos instalados'
    if (Test-Path -LiteralPath $uninstall) { Remove-Item -LiteralPath $uninstall -Recurse -Force }
    if (-not $test) {
        try { [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime]::History.Clear('taleshernandes.harmony') } catch { Write-Output 'Installer: notification history could not be cleared.' }
        $class = 'HKCU:\Software\Classes\CLSID\{549D187E-812E-46F4-9AE0-D754C89F43A1}'
        if (Test-Path -LiteralPath $class) { Remove-Item -LiteralPath $class -Recurse -Force }
    }
    Report-InstallProgress 100 5 'Desinstalação concluída'
    Write-Output 'Installer: uninstall complete.'
    exit 0
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Package))
if (-not $stage.StartsWith($root + '\.installer-', [StringComparison]::OrdinalIgnoreCase)) { throw 'Package staging outside installation.' }
$expanded = Join-Path $stage 'expanded'
$zip = [IO.Compression.ZipFile]::OpenRead($Package)
try {
    [long]$total = 0; $seen = @{}
    if ($zip.Entries.Count -gt 4096) { throw 'Package file count exceeded.' }
    foreach ($entry in $zip.Entries) {
        $path = [IO.Path]::GetFullPath((Join-Path $expanded $entry.FullName))
        if (-not $path.StartsWith($expanded + '\', [StringComparison]::OrdinalIgnoreCase) -or $entry.FullName.Contains(':') -or $seen.ContainsKey($path)) { throw 'Unsafe archive path.' }
        $seen[$path] = $true; $total += $entry.Length
        if ($total -gt 2GB) { throw 'Expanded package limit exceeded.' }
    }
} finally { $zip.Dispose() }
$zip = [IO.Compression.ZipFile]::OpenRead($Package)
try {
    $index=0; $count=$zip.Entries.Count
    foreach ($entry in $zip.Entries) {
        $index++
        Report-InstallProgress (2 + [int](3*$index/$count)) 1 ("Preparando arquivos — $index de $count")
        $target=Join-Path $expanded $entry.FullName
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target)
    }
} finally { $zip.Dispose() }
Assert-Tree $expanded
if (Test-Path -LiteralPath (Join-Path $expanded 'payload.cab')) {
    . (Join-Path $expanded 'instalar\cabinet.ps1')
    Expand-HarmonyCabinet (Join-Path $expanded 'payload.cab') (Join-Path $expanded 'cabinet-files.json') $expanded {
        param($done,$count,$name)
        Report-InstallProgress (5 + [int](25*$done/$count)) 1 ("Extraindo arquivos — $done de $count ($name)")
    }
    Assert-Tree $expanded
}
$pointer = Join-Path $native 'current.json'
$previous = if (Test-Path -LiteralPath $pointer) { [IO.File]::ReadAllText($pointer) } else { $null }
# Keep fixed-path launch/recovery support even when the native release rolls back.
if ($previous) {
    foreach ($name in @('live-app.ps1','launch.ps1')) {
        Copy-Item -LiteralPath (Join-Path $expanded ('instalar/'+$name)) -Destination (Join-Path $native $name) -Force
    }
}
$committed = $false; $process = $null; $ready = $null
$snapshot = @{}
foreach ($path in @($uninstall, $run)) {
    $fields = @{}
    if (Test-Path -LiteralPath $path) {
        $key = Get-Item -Force -LiteralPath $path
        $owned = if ($path -eq $run) { @('harmony') } else { @('DisplayName','DisplayVersion','Publisher','InstallLocation','DisplayIcon','UninstallString','QuietUninstallString','NoModify','NoRepair') }
        foreach ($name in $owned) {
            if ($key.GetValueNames() -contains $name) { $fields[$name] = @{value=$key.GetValue($name); kind=$key.GetValueKind($name)} }
        }
    }
    $snapshot[$path] = $fields
}
$linkBackup = @{}
foreach ($link in $links) { if (Test-Path -LiteralPath $link) { $linkBackup[$link] = [Convert]::ToBase64String([IO.File]::ReadAllBytes($link)) } }
# Persist recovery before selecting any new files or touching shell identity.
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $root 'recover-native.ps1') -Force
$fileBackup = Join-Path $stage 'backup'
New-Item -ItemType Directory -Path $fileBackup | Out-Null
foreach ($name in @('uninstall-native.ps1','recover-native.ps1','install-result.json')) {
    $file = Join-Path $root $name
    if (Test-Path -LiteralPath $file) {
        $target = Join-Path $fileBackup $name
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Copy-Item -LiteralPath $file -Destination $target -Force
    }
}
if ($Reinstall -and $previous) { Copy-Item -LiteralPath (Join-Path $native "app-$Release") -Destination (Join-Path $fileBackup 'previous-app') -Recurse }
$state = @{backup=$fileBackup; previous=$previous; release=$Release; registry=$snapshot; links=$linkBackup}
$stateText = $state | ConvertTo-Json -Depth 8
$journalBytes = [Text.UTF8Encoding]::new($false).GetBytes($stateText)
$journalFile = [IO.File]::Open($journal + '.pending', [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $journalFile.Write($journalBytes,0,$journalBytes.Length); $journalFile.Flush($true) } finally { $journalFile.Dispose() }
[IO.File]::Move($journal + '.pending', $journal)
try {
    Report-InstallProgress 30 2 "Copiando os arquivos do aplicativo"
    if ($previous) {
        foreach ($owned in Get-Process) {
            try { $ownedPath = $owned.Path } catch { continue }
            if ($ownedPath -and $ownedPath.StartsWith($native + '\', [StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $owned.Id -Force; $owned.WaitForExit() }
        }
    }
    if ($Reinstall -and $previous) {
        Report-InstallProgress 30 2 'Removendo a versão anterior'
        foreach ($owned in Get-Process) {
            try { $ownedPath = $owned.Path } catch { continue }
            if ($ownedPath -and $ownedPath.StartsWith($native + '\', [StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $owned.Id -Force; $owned.WaitForExit() }
        }
        Remove-Owned $pointer
        Remove-Owned (Join-Path $native "app-$Release")
    }
    # The launcher understands both legacy and fixed paths; retain it on rollback.
    if ($Bridge) {
        Report-InstallProgress 30 2 'Conferindo e salvando o iniciador do harmony'
        $bridgePath = [IO.Path]::GetFullPath($Bridge)
        if ($bridgePath -ne $stable -and $bridgePath -ne (Join-Path $root 'harmony.next.exe') -and ([IO.Path]::GetDirectoryName($bridgePath) -ne $root -or [IO.Path]::GetFileName($bridgePath) -notmatch '^harmony\.download-\{[0-9a-fA-F-]{36}\}\.exe$')) { throw 'Bridge outside installation refused.' }
        if ($bridgePath -ne $stable) {
            $pendingBridge = Join-Path $root 'harmony.launcher.pending'
            Copy-Item -LiteralPath $bridgePath -Destination $pendingBridge -Force
            if ((Get-FileHash -LiteralPath $bridgePath).Hash -ne (Get-FileHash -LiteralPath $pendingBridge).Hash) { throw 'Launcher copy verification failed.' }
            if (Test-Path -LiteralPath $stable) {
                $backup = 'harmony.previous-bridge.exe'
                [IO.File]::Replace($pendingBridge, $stable, (Join-Path $root $backup))
            } else { [IO.File]::Move($pendingBridge, $stable) }
        }
    }
    & (Join-Path $expanded 'instalar\instalar.ps1') -Origem (Join-Path $expanded 'app') -Destino $native -Versao $Release -SomenteArquivos -PreservarAnterior -ProgressFile $ProgressFile | Out-Null
    $allowLogin = $false
    if (-not $Interactive) {
    $eventName = 'Local\harmony-installer-ready-' + [Guid]::NewGuid().ToString('N')
    $ready = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $eventName)
    $authenticatedReady = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, ($eventName + '-authenticated'))
    $env:HARMONY_INSTALLER_READY_EVENT = $eventName
    $process = & (Join-Path $native 'launch.ps1') -Bandeja:$Tray -PassThru
    $null = $process.Handle
    $confirmed = $false
    for ($i=0; $i -lt 120; $i++) {
        if ($ready.WaitOne(500)) { $confirmed = $true; break }
        if ($process.HasExited) { break }
    }
    if (-not $confirmed -or $process.HasExited) { throw 'Native application did not confirm login or authenticated UI readiness.' }
    if ($ProgressFile) { [IO.File]::WriteAllText(($ProgressFile + '.native-ready'),'ready') }
    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw 'Native application exited during readiness confirmation.' }
    $allowLogin = -not $authenticatedReady.WaitOne(0)
    Remove-Item Env:\HARMONY_INSTALLER_READY_EVENT
    }
    $exe = Join-Path $native 'app\harmony-nativo.exe'
    Report-InstallProgress 80 3 'Conferindo a versão do aplicativo'
    $nativeVersion = (Get-Item -Force -LiteralPath $exe).VersionInfo.ProductPrivatePart.ToString()
    if ($nativeVersion -notmatch '^[1-9][0-9]*$') { throw 'Native product version missing.' }
    Report-InstallProgress 80 4 "Criando o atalho no Menu Iniciar"
    if ($test -and $env:HARMONY_INSTALLER_FAIL_PHASE -eq 'shortcuts') { throw 'Injected shortcut failure.' }
    foreach ($link in $links) {
        if ($link -eq $links[1] -and (($Interactive -and -not $Desktop) -or (-not $Interactive -and -not $Fresh -and -not (Test-Path -LiteralPath $link)))) {
            if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
            continue
        }
        Report-InstallProgress $(if ($link -eq $links[0]) {80} else {85}) 4 $(if ($link -eq $links[0]) {'Criando o atalho no Menu Iniciar'} else {'Criando o atalho na área de trabalho'})
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($link)) -Force | Out-Null
        $p = Start-Process -FilePath $exe -ArgumentList ('--atalho "{0}" "{1}"' -f $link,$stable) -WindowStyle Hidden -PassThru -Wait
        if ($p.ExitCode -ne 0) { throw 'Could not refresh shell shortcut.' }
    }
    Report-InstallProgress 90 5 "Registrando o harmony em Aplicativos instalados"
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $root 'uninstall-native.ps1') -Force
    if (-not (Test-Path -LiteralPath $uninstall)) { New-Item -Path $uninstall -Force | Out-Null }
    $removeCommand = '"{0}" --uninstall' -f $stable
    foreach ($pair in @{
        DisplayName='harmony'; DisplayVersion=$nativeVersion; Publisher='harmony'; InstallLocation=$root
        DisplayIcon=$stable; UninstallString=$removeCommand; QuietUninstallString=$removeCommand
    }.GetEnumerator()) { New-ItemProperty -Path $uninstall -Name $pair.Key -Value $pair.Value -PropertyType String -Force | Out-Null }
    New-ItemProperty -Path $uninstall -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $uninstall -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
    Report-InstallProgress 94 6 'Configurando para abrir com o Windows'
    if (($Interactive -and $Startup) -or (-not $Interactive -and ($Fresh -or $snapshot[$run].ContainsKey('harmony')))) {
        if (-not (Test-Path -LiteralPath $run)) { New-Item -Path $run -Force | Out-Null }
        New-ItemProperty -Path $run -Name harmony -Value ('"{0}" --tray' -f $stable) -PropertyType String -Force | Out-Null
    }
    elseif ($Interactive) { Remove-ItemProperty -Path $run -Name harmony -ErrorAction SilentlyContinue }
    Report-InstallProgress 100 7 "Instalação concluída"
    $committed = $true
    [IO.File]::WriteAllText((Join-Path $root 'install-result.json'), (@{release=$Release; native_ready=(!$Interactive); authenticated_ui=(!$Interactive -and !$allowLogin); login_ui=(!$Interactive -and $allowLogin); aumid='taleshernandes.harmony'; elapsed_ms=$operationClock.Elapsed.TotalMilliseconds} | ConvertTo-Json))
    $readiness = if ($Interactive) { 'installed files' } elseif ($allowLogin) { 'login' } else { 'authenticated' }
    Write-Output "Installer: release $Release confirmed $readiness; Windows registration complete."
    Add-Content -LiteralPath (Join-Path $root 'installer.log') -Value "Installer: release $Release confirmed $readiness; Windows registration complete."
    Remove-Item -LiteralPath $journal -Force
} catch {
    # A candidate can exit between observing HasExited and stopping it. A
    # failed stop must never prevent restoring the pointer and registration.
    if ($process) {
        try { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $null = $process.WaitForExit(5000) } } catch {}
    }
    Remove-Owned (Join-Path $native "app-$Release")
    Restore-State ($stateText | ConvertFrom-Json)
    Remove-Item -LiteralPath $journal -Force
    [IO.File]::WriteAllText((Join-Path $root 'failed-release.json'), (@{version=$Release} | ConvertTo-Json))
    Write-Output "Installer: release $Release rolled back; UI readiness not confirmed or registration failed."
    Add-Content -LiteralPath (Join-Path $root 'installer.log') -Value "Installer: release $Release rolled back: $($_.Exception.Message)"
    if ($previous) {
        if (-not $Interactive -and -not $Tray -and $ProgressFile) {
            [IO.File]::WriteAllText(($ProgressFile + '.rollback'),'rollback')
            Start-Sleep -Milliseconds 2200
        }
        & (Join-Path $native 'launch.ps1') -Bandeja:$Tray
    }
    exit 10
} finally {
    if ($ready) { $ready.Dispose() }
    if ($authenticatedReady) { $authenticatedReady.Dispose() }
    Remove-Item Env:\HARMONY_INSTALLER_READY_EVENT -ErrorAction SilentlyContinue
}

} finally {
    if ($ownsTransition) { $transition.ReleaseMutex() }
    if ($observer) { $observer.ReleaseMutex(); $observer.Dispose() }
    $transition.Dispose()
}
