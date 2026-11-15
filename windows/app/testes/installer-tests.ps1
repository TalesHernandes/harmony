param([switch]$MutateStartup)
$ErrorActionPreference = 'Stop'
$env:TEMP = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'test-output/temp'))
$env:TMP = $env:TEMP
[IO.Directory]::CreateDirectory($env:TEMP) | Out-Null
# Execute the real registration function with registry/process adapters. No
# HKCU writes, installation outside the worktree, or real application launch.
$root = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'test-output/startup-fixture'))
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
$shortcut = Join-Path $root 'shortcut.lnk'
$desktopShortcut = Join-Path $root 'desktop.lnk'
$uninstallKey = 'fixture-uninstall'
$runKey = 'fixture-run'
$firstInstall = $true
$SomenteArquivos = $false
$Versao = 2
$script:commands = @()
$script:shortcuts = @()
$script:icons = @()
# A run that died mid-test leaves the fixture shortcut behind, and the first
# check would read it as one the installer made.
if (Test-Path -LiteralPath $desktopShortcut) { [IO.File]::Delete($desktopShortcut) }
function New-Item {
    param($Path, $ItemType, $Target, [switch]$Force, $ErrorAction)
    if ($ItemType -eq 'HardLink' -and $script:forceCopy) { throw 'Fixture hardlink unavailable.' }
    if ($Path -notlike 'fixture-*') { Microsoft.PowerShell.Management\New-Item @PSBoundParameters }
}
function New-ItemProperty {
    param($Path, $Name, $Value, $PropertyType, [switch]$Force)
    if ($Name -eq 'DisplayIcon') { $script:icons += $Value }
    if ($Name -eq 'harmony-nativo') { $script:commands += $Value }
}
function Get-ItemProperty { param($Path, $Name, $ErrorAction) return $null }
function Start-Process {
    param($FilePath, $ArgumentList, $WindowStyle, [switch]$PassThru, $WorkingDirectory)
    $script:shortcuts += [string]$ArgumentList
    $process = [pscustomobject]@{ExitCode=0; FilePath=$FilePath; ArgumentList=$ArgumentList}
    $process | Add-Member ScriptMethod WaitForExit {}
    return $process
}
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path (Split-Path $PSScriptRoot -Parent) 'instalar/instalar.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Installer syntax error' }
$register = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Register-Native' }, $true)
$definition = $register.Extent.Text
if ($MutateStartup) {
    $definition = $definition.Replace('-f (Join-Path $root ''launch.ps1'')', '-f $Executable')
}
. ([scriptblock]::Create($definition))
Register-Native (Join-Path $root 'app/harmony-nativo.exe')
Register-Native (Join-Path $root 'app/harmony-nativo.exe')
$expected = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -Bandeja' -f (Join-Path $root 'launch.ps1')
if ($commands.Count -ne 2 -or $commands[0] -cne $expected -or $commands[1] -cne $expected) { throw 'Installer startup is tied to a version' }
if (@($icons | Where-Object { $_ -cne (Join-Path $root 'app/harmony-nativo.exe') }).Count) { throw 'DisplayIcon is tied to a version.' }
# Refresh only the desktop shortcut the owner keeps.
if (@($shortcuts | Where-Object { $_ -like "*$desktopShortcut*" }).Count) { throw 'Installer created a desktop shortcut nobody asked for' }
[IO.Directory]::CreateDirectory($root) | Out-Null
[IO.File]::WriteAllText((Join-Path $root '.harmony-native-install'), 'fixture')
[IO.File]::WriteAllText($desktopShortcut, 'fixture')
$script:shortcuts = @()
Register-Native (Join-Path $root 'app/harmony-nativo.exe')
$desktop = '--atalho "{0}" "{1}"' -f $desktopShortcut,(Join-Path $root 'app/harmony-nativo.exe')
if ($shortcuts -cnotcontains $desktop) { throw 'Installer left the desktop shortcut on the old version' }
Remove-Item -LiteralPath $desktopShortcut -Force
# Execute the actual launcher against two complete fixture versions and an
# updated pointer, including tray arguments. OS process creation stays mocked;
# the integration harness exercises the suspended process and foreground grant.
Add-Type -TypeDefinition @'
public static class HarmonyLaunch {
    public static string Executable, Arguments;
    public static bool Foreground;
    public static int Start(string exe,string args,bool foreground) {
        Executable=exe; Arguments=args; Foreground=foreground; return 123;
    }
}
'@
function Get-Process {
    param($Id)
    if (-not $PSBoundParameters.ContainsKey('Id')) { return @() }
    if ($Id -ne 123) { throw 'Unexpected process lookup.' }
    return [pscustomobject]@{ FilePath=[HarmonyLaunch]::Executable; ArgumentList=[HarmonyLaunch]::Arguments }
}
Copy-Item -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'instalar/live-app.ps1') -Destination (Join-Path $root 'live-app.ps1') -Force
. (Join-Path $root 'live-app.ps1')
Copy-Item -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'instalar/launch.ps1') -Destination (Join-Path $root 'launch.ps1') -Force
foreach ($version in 1,2) {
    $dir = Join-Path $root "app-$version"
    [IO.Directory]::CreateDirectory($dir) | Out-Null
    $exe = Join-Path $dir 'harmony-nativo.exe'
    [IO.File]::WriteAllText($exe, "fixture-$version")
    $manifest = @{version=$version; complete=$true; files=@(@{path='harmony-nativo.exe'; bytes=(Get-Item $exe).Length; sha256=(Get-FileHash $exe).Hash})}
    [IO.File]::WriteAllText((Join-Path $dir 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5))
    [IO.File]::WriteAllText((Join-Path $root 'current.json'), (@{version=$version; directory="app-$version"} | ConvertTo-Json))
    if ($version -eq 1) {
        $legacy = & (Join-Path $root 'launch.ps1') -PassThru
        if ($legacy.FilePath -cne $exe) { throw 'Legacy installation no longer launches.' }
    }
    $script:forceCopy=$version -eq 2
    Promote-HarmonyVersion $root $version
    $script:forceCopy=$false
    [IO.File]::WriteAllText((Join-Path $root 'current.json'), (@{version=$version; directory="app-$version";live='app'} | ConvertTo-Json))
    $exe=Join-Path $root 'app/harmony-nativo.exe'
    $launched = & (Join-Path $root 'launch.ps1') -Bandeja -PassThru
    if ($launched.FilePath -cne $exe -or $launched.ArgumentList -cne '--bandeja' -or [HarmonyLaunch]::Foreground) { throw 'Launcher did not select current version in tray mode' }
    $launched = & (Join-Path $root 'launch.ps1') -PassThru
    if ($launched.FilePath -cne $exe -or $launched.ArgumentList -cne '' -or -not [HarmonyLaunch]::Foreground) { throw 'User launch did not request foreground for current version.' }
    [IO.File]::WriteAllText($exe, "gixture-$version")
    $refused=$false
    try { & (Join-Path $root 'launch.ps1') -PassThru | Out-Null }
    catch { $refused=$_.Exception.Message -like 'Invalid installed file:*' }
    if(-not $refused) { throw 'Launcher accepted changed bytes with the same length.' }
    [IO.File]::WriteAllText($exe, "fixture-$version")
    Write-Output 'PASS: launcher still rejects same-length file tampering after hash-loop optimization'
}
# Both crash windows must restore the selected version without changing its path.
[IO.Directory]::Move((Join-Path $root 'app'),(Join-Path $root ('.retired-app-'+[Guid]::NewGuid().ToString('N'))))
$recovered=& (Join-Path $root 'launch.ps1') -PassThru
if ($recovered.FilePath -cne $exe) { throw 'Missing live directory was not recovered.' }
Promote-HarmonyVersion $root 1
$recovered=& (Join-Path $root 'launch.ps1') -PassThru
if ($recovered.FilePath -cne $exe -or [IO.File]::ReadAllText($exe) -ne 'fixture-2') { throw 'Interrupted pointer commit was not recovered.' }
$extra=Join-Path $root 'app/extra.dll'
[IO.File]::WriteAllText($extra,'unlisted')
$refused=$false
try { & (Join-Path $root 'launch.ps1') -PassThru | Out-Null } catch { $refused=$true }
if (-not $refused) { throw 'Launcher accepted an unlisted library.' }
# Use a directory junction, which requires no symlink privilege.
$junction=Join-Path $root 'app/junction'
Microsoft.PowerShell.Management\New-Item -ItemType Junction -Path $junction -Target (Join-Path $root 'app-1') | Out-Null
$refused=$false
try { & (Join-Path $root 'launch.ps1') -PassThru | Out-Null } catch { $refused=$true }
[IO.Directory]::Delete($junction)
if (-not $refused) { throw 'Launcher accepted a reparse point.' }
Write-Output 'PASS: fixed path survives promotion, legacy migration and both interrupted rename windows; unlisted library and reparse point refused.'
Write-Output 'PASS: promotion falls back to copies when hardlinks are unavailable.'
Write-Output 'PASS: installer stable Run destination and current launcher (two versions, mocked HKCU)'
# Execute the real uninstall function against an in-memory HKCU adapter too.
$script:removed = @()
$script:historyCleared = 0
function Clear-NativeNotices { $script:historyCleared++ }
function Test-Path {
    param($LiteralPath)
    if ($LiteralPath -like 'HKCU:*') { return $true }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath
}
function Remove-Item {
    param($LiteralPath, [switch]$Force, [switch]$Recurse)
    $script:removed += $LiteralPath
}
function Remove-ItemProperty { param($Path, $Name, $ErrorAction) $script:removed += "$Path/$Name" }
$unregister = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Unregister-Native' }, $true)
. ([scriptblock]::Create($unregister.Extent.Text))
Unregister-Native
$expectedClass = 'HKCU:\Software\Classes\CLSID\{549D187E-812E-46F4-9AE0-D754C89F43A1}'
if ($historyCleared -ne 1 -or $removed -notcontains $expectedClass) { throw 'Uninstaller retained classic toast registration or history' }
if (@($removed | Where-Object { $_ -like '*B9588A4C*' }).Count) { throw 'Uninstaller touched Audit identity' }
$SomenteArquivos = $true
$before = $removed.Count
Unregister-Native
if ($removed.Count -ne $before -or $historyCleared -ne 1) { throw 'Files-only uninstall touched registry or history' }
Write-Output 'PASS: uninstaller clears classic toast CLSID and history (mocked HKCU, Audit untouched)'
