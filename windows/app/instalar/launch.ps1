param([switch]$PassThru, [switch]$Bandeja)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'live-app.ps1')
if (-not (Test-Path -LiteralPath (Join-Path $root '.harmony-native-install'))) { throw 'Installation marker missing.' }
Assert-HarmonyTree $root
$lock = [IO.File]::Open((Join-Path $root '.install.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    $pointer = Get-Content -LiteralPath (Join-Path $root 'current.json') -Raw | ConvertFrom-Json
    if ($pointer.version -lt 1 -or $pointer.directory -cne "app-$([int]$pointer.version)" -or ($pointer.live -and $pointer.live -cne 'app')) { throw 'Invalid version pointer.' }
    $depot = Join-Path $root $pointer.directory
    $manifest = Read-HarmonyManifest $depot $pointer.version
    $directory = Join-Path $root 'app'
    if (-not (Test-Path -LiteralPath $directory)) {
        if ($pointer.live -eq 'app') { Promote-HarmonyVersion $root $pointer.version }
        else { $directory = $depot }
    }
    try { Test-HarmonyVersion $directory $manifest $depot }
    catch {
        if ($directory -eq $depot) { throw }
        $failure=$_
        Assert-HarmonyTree $directory
        $liveManifest=$null
        try { $liveManifest=[IO.File]::ReadAllText((Join-Path $directory 'manifest.json')) | ConvertFrom-Json } catch {}
        Promote-HarmonyVersion $root $pointer.version
        # A different version is an interrupted swap; same-version tampering
        # is repaired too, but this launch still reports the integrity failure.
        if (-not $liveManifest.complete -or $liveManifest.version -lt 1 -or $liveManifest.version -eq $pointer.version) { throw $failure }
        Test-HarmonyVersion $directory $manifest $depot
    }
} finally { $lock.Dispose() }
$exe = Join-Path $directory 'harmony-nativo.exe'
# Launch only the verified native application.
$arguments = @()
if ($Bandeja) { $arguments += '--bandeja' }
if (-not ('HarmonyLaunch' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HarmonyLaunch {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Startup {
        public int cb; public string reserved, desktop, title;
        public int x,y,w,h,cx,cy,fill,flags; public short show,reservedSize;
        public IntPtr reservedData,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app,string command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup startup,out ProcessInfo info);
    [DllImport("user32.dll")] static extern bool AllowSetForegroundWindow(uint pid);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static int Start(string exe,string args,bool foreground) {
        var startup=new Startup(); startup.cb=Marshal.SizeOf(startup);
        ProcessInfo info;
        if(!CreateProcess(exe,"\""+exe+"\" "+args,IntPtr.Zero,IntPtr.Zero,false,4,IntPtr.Zero,System.IO.Path.GetDirectoryName(exe),ref startup,out info)) throw new System.ComponentModel.Win32Exception();
        try { if(foreground) AllowSetForegroundWindow(info.pid); ResumeThread(info.thread); return (int)info.pid; }
        finally { CloseHandle(info.thread); CloseHandle(info.process); }
    }
}
'@
}
$pidNative = [HarmonyLaunch]::Start($exe,($arguments -join ' '),(!$Bandeja))
if ($PassThru) { Get-Process -Id $pidNative }
