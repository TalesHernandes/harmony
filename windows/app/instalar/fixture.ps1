param([ValidateSet('Login','SelectLocation','SeedBroken','CheckRemoved','Primary','Secondary','Desktop','Startup','UiState','Capture','CheckHidden','CheckForeground','StopNative','Check','CheckRunning','Uninstall','Cleanup')][string]$Action, [int]$Expected=126, [string]$Destination)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$test=[IO.Path]::GetFullPath($env:HARMONY_INSTALLER_TEST_ROOT).TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $test '.harmony-installer-test')) -or $env:HARMONY_URL -notmatch '^http://127\.0\.0\.1:\d+$') { throw 'Isolated fixture required.' }
$root=Join-Path $test 'programs\harmony'
if($env:HARMONY_INSTALLER_EXPECTED_ROOT) {
    $root=[IO.Path]::GetFullPath($env:HARMONY_INSTALLER_EXPECTED_ROOT)
    if(-not $root.StartsWith($test+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Expected root outside fixture.'}
}
$key='HKCU:\Software\harmony-installer-tests\' + [IO.Path]::GetFileName($test)
if ($Action -eq 'Login') {
    # Credentials enter through stdin, never through command arguments or logs.
    try {
        $credentials=[Console]::ReadLine() | ConvertFrom-Json
        Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
        $owned=@(Get-Process -Name harmony-nativo -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root+'\native\',[StringComparison]::OrdinalIgnoreCase) })
        if ($owned.Count -ne 1 -or -not $owned[0].MainWindowHandle) { throw 'Fixture window missing.' }
        $element=[Windows.Automation.AutomationElement]::FromHandle($owned[0].MainWindowHandle)
        foreach($entry in @(@('Nome ou e-mail',[string]$credentials.username),@('Senha',[string]$credentials.password))) {
            $condition=[Windows.Automation.AndCondition]::new(
                [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty,$entry[0]),
                [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty,[Windows.Automation.ControlType]::Edit))
            $field=$element.FindFirst([Windows.Automation.TreeScope]::Descendants,$condition)
            if (-not $field) { throw 'Fixture field missing.' }
            $pattern=$field.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
            $pattern.SetValue($entry[1])
        }
        $condition=[Windows.Automation.AndCondition]::new(
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty,'Entrar'),
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty,[Windows.Automation.ControlType]::Button))
        $button=$element.FindFirst([Windows.Automation.TreeScope]::Descendants,$condition)
        if (-not $button) { throw 'Fixture submit missing.' }
        $deadline=[DateTime]::UtcNow.AddSeconds(5)
        while(-not $button.Current.IsEnabled -and [DateTime]::UtcNow -lt $deadline) {Start-Sleep -Milliseconds 50}
        $button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
        Write-Output 'PASS: login submitted through the real native controls.'
    } catch { throw 'Native fixture login could not be submitted.' }
    finally {if($credentials){$credentials.password=$null}}
    exit 0
}
if ($Action -eq 'SeedBroken') {
    New-Item -ItemType Directory -Path (Join-Path $root 'native\app-5'),(Join-Path $root '.w7-debris') -Force | Out-Null
    foreach ($name in @('.harmony-w7','.install.lock','harmony.download-old.exe','failed-release.json','native\.install.lock')) { [IO.File]::WriteAllText((Join-Path $root $name),'old debris') }
    New-Item -Path "$key\Uninstall\harmony" -Force | Out-Null
    foreach ($field in @{DisplayName='harmony';DisplayVersion='1.0';DisplayIcon=(Join-Path $root 'harmony.exe');UninstallString=('"' + (Join-Path $root 'harmony.exe') + '" --uninstall')}.GetEnumerator()) { New-ItemProperty -Path "$key\Uninstall\harmony" -Name $field.Key -Value $field.Value -Force | Out-Null }
    Write-Output 'PASS: owner-style broken state seeded without a native or WebView2 profile.'
    exit 0
}
if ($Action -in @('SelectLocation','Primary','Secondary','Desktop','Startup','UiState','Capture')) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class InstallerWindow {
    delegate bool Callback(IntPtr window,IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback,IntPtr parameter);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window,StringBuilder text,int count);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window,StringBuilder text,int count);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wp,IntPtr lp);
    public static IntPtr Find(string root) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((window,param)=> {
            var text=new StringBuilder(256);GetClassName(window,text,256);
            if(text.ToString()!="harmony.installer")return true;
            uint pid;GetWindowThreadProcessId(window,out pid);
            try { if(System.Diagnostics.Process.GetProcessById((int)pid).MainModule.FileName.StartsWith(root+"\\",StringComparison.OrdinalIgnoreCase))found=window; } catch {}
            return true;
        },IntPtr.Zero);return found;
    }
    [StructLayout(LayoutKind.Sequential)] struct CopyData { public IntPtr kind; public int size; public IntPtr text; }
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr window,uint message,IntPtr wp,ref CopyData data);
    public static void SelectLocation(IntPtr window,string path) {
        var text=Marshal.StringToHGlobalUni(path);
        try {var data=new CopyData{kind=(IntPtr)7,size=(path.Length+1)*2,text=text};SendMessage(window,74,IntPtr.Zero,ref data);}finally{Marshal.FreeHGlobal(text);}
    }
    public static string Title(IntPtr window) {var text=new StringBuilder(256);GetWindowText(window,text,256);return text.ToString();}
    public static uint Owner(IntPtr window) {uint pid;GetWindowThreadProcessId(window,out pid);return pid;}
}
'@
    $handle=[InstallerWindow]::Find($test)
    if ($handle -eq [IntPtr]::Zero) {
        # A read-only poll can observe the gap between two installer processes.
        if ($Action -eq 'UiState') { exit 0 }
        throw 'Isolated installer window not found.'
    }
    if ($Action -eq 'SelectLocation') { [InstallerWindow]::SelectLocation($handle,$Destination);exit 0 }
    if ($Action -eq 'UiState') { [InstallerWindow]::Title($handle);exit 0 }
    if ($Action -eq 'Capture') {
        $title=[InstallerWindow]::Title($handle)
        $name=($title -replace '[^\p{L}\p{N}]+','-').Trim('-')
        $locationFile=Join-Path $test 'location-result.json'
        if ($title -like '*Instalar o harmony' -and (Test-Path -LiteralPath $locationFile) -and (Get-Item -LiteralPath $locationFile).LastWriteTime -ge (Get-Process -Id ([InstallerWindow]::Owner($handle))).StartTime) {
            $location=Get-Content -LiteralPath (Join-Path $test 'location-result.json') -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($location.error) { $name+='-recusa-pasta' }
            elseif ($location.root -ne (Join-Path $test 'programs\harmony')) { $name+='-pasta-personalizada' }
        }
        if ($title -match 'Instalando|Desinstalando') {
            $record=Join-Path $test ('logs\progress-' + [InstallerWindow]::Owner($handle) + '.json')
            if (Test-Path -LiteralPath $record) {
                try {
                    $stream=[IO.File]::Open($record,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                    $reader=[IO.StreamReader]::new($stream)
                    try { $name += '-etapa-' + ($reader.ReadToEnd() | ConvertFrom-Json).step } finally { $reader.Dispose() }
                } catch [IO.IOException] { } # A progress frame is optional; the screen itself is still captured.
            }
        }
        $prefix=if($env:HARMONY_INSTALLER_PHOTO_PREFIX){$env:HARMONY_INSTALLER_PHOTO_PREFIX}else{'w7f-foto'}
        if ($prefix -notmatch '^[A-Za-z0-9-]{1,96}$') { throw 'Invalid fixture photo prefix.' }
        & (Join-Path $PSScriptRoot 'capture-window.ps1') -ProcessId ([InstallerWindow]::Owner($handle)) -Output (Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))) ("$prefix-$name.png"))
        exit 0
    }
    $value=@{Primary=1;Secondary=2;Desktop=3;Startup=4}[$Action]
    $null=[InstallerWindow]::PostMessage($handle,32778,[IntPtr]$value,[IntPtr]::Zero)
    exit 0
}
if ($Action -eq 'CheckForeground' -or $Action -eq 'CheckHidden') {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FixtureWindow {
    delegate bool Callback(IntPtr window,IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback,IntPtr parameter);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    public static bool Visible(uint pid) {
        bool visible=false;
        EnumWindows((window,param)=> { uint owner; GetWindowThreadProcessId(window,out owner); if(owner==pid&&IsWindowVisible(window)) visible=true; return true; },IntPtr.Zero);
        return visible;
    }
}
'@
    if ($Action -eq 'CheckHidden') {
        $found=@(Get-Process | Where-Object { try { $_.Path -eq (Join-Path $root 'native\app\harmony-nativo.exe') } catch { $false } })
        if ($found.Count -ne 1 -or [FixtureWindow]::Visible($found[0].Id)) { throw 'Tray launch must keep the native window hidden.' }
        Write-Output "PASS: release $Expected tray launch keeps native window hidden."
        exit 0
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do {
        $window=[FixtureWindow]::GetForegroundWindow()
        [uint32]$owner=0
        $null=[FixtureWindow]::GetWindowThreadProcessId($window,[ref]$owner)
        $process=Get-Process -Id $owner -ErrorAction SilentlyContinue
        if ($process -and $process.Path -eq (Join-Path $root 'native\app\harmony-nativo.exe') -and [FixtureWindow]::IsWindowVisible($window) -and -not [FixtureWindow]::IsIconic($window)) {
            Write-Output "PASS: release $Expected native window is visible and in foreground."
            exit 0
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Native window did not become visible and foreground.'
}
if($Action -eq 'Cleanup') {
    foreach($process in Get-Process) {
        try { $path=$process.Path } catch { continue }
        if($path -and $path.StartsWith($test+'\',[StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $process.Id -Force; $process.WaitForExit() }
    }
    Start-Sleep -Seconds 3
    & (Join-Path $PSScriptRoot 'bridge-host.ps1') -Root $root -Remove
    if($LASTEXITCODE) { throw 'Fixture cleanup failed.' }
    if(Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    exit
}
if ($Action -eq 'StopNative') {
    foreach($process in Get-Process) {
        try { $path=$process.Path } catch { continue }
        if (-not $path) { continue }
        $owned = $path.StartsWith((Join-Path $root 'native')+'\',[StringComparison]::OrdinalIgnoreCase)
        if($owned) { Stop-Process -Id $process.Id -Force; $process.WaitForExit() }
    }
    exit
}
if ($Action -eq 'CheckRunning') {
    $expectedPath=Join-Path $root 'native\app\harmony-nativo.exe'
    $found=@(Get-Process | Where-Object { try { $_.Path -eq $expectedPath } catch { $false } })
    if ($found.Count -ne 1) { throw 'Expected exactly one installed native process.' }
    Write-Output "PASS: exactly one installed native process for release $Expected."
}
if ($Action -eq 'Check') {
    foreach ($debris in @('.harmony-w7','.w7-debris','.install.lock','harmony.download-old.exe','native\app-5')) {
        if (Test-Path -LiteralPath (Join-Path $root $debris)) { throw 'Legacy failed-install debris remains.' }
    }
    $entry=Get-ItemProperty "$key\Uninstall\harmony"
    if($entry.DisplayName -ne 'harmony' -or [int]$entry.DisplayVersion -ne ($Expected - 100) -or $entry.InstallLocation -ne $root -or $entry.UninstallString -notlike '*harmony.exe" --uninstall') { throw 'Uninstall entry mismatch.' }
    $run=(Get-ItemProperty "$key\Run").harmony
    if ($run -cne ('"{0}" --tray' -f (Join-Path $root 'harmony.exe'))) { throw 'Startup entry mismatch.' }
    $shell=New-Object -ComObject Shell.Application
    $wsh=New-Object -ComObject WScript.Shell
    foreach($name in @('start','desktop')) {
        $link=Join-Path $test "shortcuts\$name\harmony.lnk"
        if($wsh.CreateShortcut($link).TargetPath -ne (Join-Path $root 'harmony.exe')) { throw 'Shortcut target mismatch.' }
        $folder=$shell.NameSpace([IO.Path]::GetDirectoryName($link))
        $item=$folder.ParseName([IO.Path]::GetFileName($link))
        if($item.ExtendedProperty('System.AppUserModel.ID') -ne 'taleshernandes.harmony') { throw 'Shortcut AUMID mismatch.' }
    }
    Write-Output "PASS: release $Expected registry, startup, both shortcuts, native version $($Expected - 100) and stable AUMID."
}
if ($Action -eq 'Uninstall') {
    $command=(Get-ItemProperty "$key\Uninstall\harmony").UninstallString
    # Execute the registered command itself, with the inherited isolated context.
    if ($command -cne ('"{0}" --uninstall' -f (Join-Path $root 'harmony.exe'))) { throw 'Unexpected registered command.' }
    # Start-Process -Wait also waits for descendants, including the confirmation
    # window that the test needs to drive next. Wait only for this launcher.
    $p=Start-Process -FilePath (Join-Path $root 'harmony.exe') -ArgumentList '--uninstall' -WindowStyle Hidden -PassThru
    $null=$p.Handle
    if (-not $p.WaitForExit(15000)) { throw 'Registered uninstall launcher did not exit.' }
    if($p.ExitCode) { throw 'Registered uninstall did not open confirmation.' }
    Write-Output 'PASS: registered uninstall opened the confirmation UI.'
    exit 0
}
if ($Action -eq 'CheckRemoved') {
    if ((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath "$key\Uninstall\harmony")) { throw 'Removal retained installation or uninstall entry.' }
    foreach($name in @('start','desktop')) { if(Test-Path -LiteralPath (Join-Path $test "shortcuts\$name\harmony.lnk")) { throw 'Uninstall retained shortcut.' } }
    if((Get-ItemProperty "$key\Run" -ErrorAction SilentlyContinue).harmony) { throw 'Uninstall retained startup.' }
    if(Test-Path -LiteralPath (Join-Path $test 'data\native')) { throw 'Uninstall retained a native session/profile.' }
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    Write-Output 'PASS: registered uninstall removed native tree, bridge, test registry, shortcuts and startup.'
}
