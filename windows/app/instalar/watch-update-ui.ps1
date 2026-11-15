param([Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($Root)
if (-not (Test-Path -LiteralPath (Join-Path $root '.harmony-installer-test'))) {throw 'Isolated UI fixture required.'}
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
$capture=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'capture-window.ps1'))
$code=[regex]::Match($capture,"@'\r?\n([\s\S]*?)\r?\n'@").Groups[1].Value
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition $code
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class UpdateFrame {
 public delegate bool Callback(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumWindows(Callback c,IntPtr p);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int left,top,right,bottom;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr z,int x,int y,int w,int hgt,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
 public static IntPtr Find(string root,out uint owner,out string title) {
  IntPtr found=IntPtr.Zero;uint pid=0;string text="";
  EnumWindows((h,p)=> {
   if(!IsWindowVisible(h))return true;
   var name=new StringBuilder(256);GetClassName(h,name,256);
   if(name.ToString()!="harmony.installer")return true;
   uint candidate;GetWindowThreadProcessId(h,out candidate);
   try {if(!System.Diagnostics.Process.GetProcessById((int)candidate).MainModule.FileName.StartsWith(root+"\\",StringComparison.OrdinalIgnoreCase))return true;}catch{return true;}
   GetWindowText(h,name,256);text=name.ToString();pid=candidate;found=h;return false;
  },IntPtr.Zero);owner=pid;title=text;return found;
 }
}
'@
[void][UpdateFrame]::SetThreadDpiAwarenessContext([IntPtr](-4))
$secondary=[Windows.Forms.Screen]::AllScreens | Where-Object {-not $_.Primary} | Select-Object -First 1
$bounds=if($secondary){@{left=$secondary.Bounds.Left;top=$secondary.Bounds.Top;right=$secondary.Bounds.Right;bottom=$secondary.Bounds.Bottom}}else{$null}
$output=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$records=@(); $seen=@{}; $trayVisible=0
$ready=Join-Path $root 'ui-watch.ready'; [IO.File]::WriteAllText($ready,'ready')
$deadline=[DateTime]::UtcNow.AddMinutes(8)
while ([DateTime]::UtcNow -lt $deadline -and -not (Test-Path -LiteralPath (Join-Path $root 'ui-watch.stop'))) {
    [uint32]$owner=0;[string]$title=''
    $handle=[UpdateFrame]::Find($root,[ref]$owner,[ref]$title)
    if ($handle -ne [IntPtr]::Zero -and $title -match 'Atualizando|Voltando') {
        $mode=if(Test-Path -LiteralPath (Join-Path $root 'ui-watch.tray')){'tray'}else{'visible'}
        if($mode -eq 'tray') {$trayVisible++}
        $stage=-1; $percent=0
        $progress=Join-Path $root ("logs\progress-$owner.json")
        if(Test-Path -LiteralPath $progress) {
            try {
                $stream=[IO.File]::Open($progress,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                $reader=[IO.StreamReader]::new($stream)
                try {$record=$reader.ReadToEnd() | ConvertFrom-Json; $stage=[int]$record.step;$percent=[int]$record.percent}finally{$reader.Dispose()}
            } catch [IO.IOException] {}
        }
        $rollback=$title -match 'Voltando'
        $key=if($rollback){'rollback'}else{"progress-$stage"}
        if(-not $seen.ContainsKey($key) -and ($rollback -or ($stage -ge 1 -and $percent -gt 5))) {
            $rect=[UpdateFrame+Rect]::new();[void][UpdateFrame]::GetWindowRect($handle,[ref]$rect)
            if($secondary -and -not $secondary.Bounds.Contains($rect.left,$rect.top)) {
                [void][UpdateFrame]::SetWindowPos($handle,[IntPtr]::Zero,($secondary.WorkingArea.Left+320),($secondary.WorkingArea.Top+240),0,0,0x15)
                Start-Sleep -Milliseconds 100
            }
            $path=Join-Path $output ("w7g-foto-atualizando-$key.png")
            try {
                [WindowPhoto]::Save($owner,$path)
                [void][UpdateFrame]::GetWindowRect($handle,[ref]$rect)
                $records+=@{title=$title;stage=$stage;percent=$percent;path=$path;dpi=[WindowPhoto]::Dpi;width=[WindowPhoto]::Width;height=[WindowPhoto]::Height;left=$rect.left;top=$rect.top}
                $seen[$key]=$true
            } catch { } # The applying window can finish between observing and capturing it.
        }
    }
    @{frames=$records;tray_visible_frames=$trayVisible;secondary_bounds=$bounds;single_monitor=(-not $secondary)} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'update-ui.json') -Encoding UTF8
    Start-Sleep -Milliseconds 50
}
