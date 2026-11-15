[CmdletBinding()]
param([switch]$Segoe, [string]$Arquivo = 'janela.png')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'out\telas.exe'
$launch = @{FilePath=$exe; WorkingDirectory=([IO.Path]::GetDirectoryName($exe)); WindowStyle='Hidden'; PassThru=$true}
if ($Segoe) { $launch.ArgumentList='--segoe' }
$app = Start-Process @launch
$null = $app.Handle
try {
    Start-Sleep -Seconds 3
    $app.Refresh()
    if ($app.HasExited) { throw "Window exited early ($($app.ExitCode))." }
    $env:TEMP = Join-Path $PSScriptRoot 'temp'
    New-Item -ItemType Directory -Force $env:TEMP | Out-Null
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
public static class ProofWindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left,top,right,bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    delegate bool EnumWindow(IntPtr window,IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback,IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window,StringBuilder title,int length);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
    public static string Title(IntPtr window) { var title=new StringBuilder(256); GetWindowText(window,title,256); return title.ToString(); }
    public static void Close(IntPtr window) { PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero); }
    public static IntPtr Find(uint pid) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((window,parameter)=> {
            uint owner; GetWindowThreadProcessId(window,out owner);
            if (owner==pid) {
                var title=new StringBuilder(256); GetWindowText(window,title,256);
                if (title.ToString().StartsWith("Harmony W1")) { result=window; return false; }
            }
            return true;
        },IntPtr.Zero);
        return result;
    }
    public static void Save(IntPtr window,string path) {
        Rect rect;
        if (!GetWindowRect(window,out rect)) throw new Exception("GetWindowRect failed.");
        using (var bitmap = new Bitmap(rect.right-rect.left,rect.bottom-rect.top))
        using (var graphics = Graphics.FromImage(bitmap)) {
            var dc=graphics.GetHdc();
            bool result;
            try { result=PrintWindow(window,dc,2); } finally { graphics.ReleaseHdc(dc); }
            if (!result) throw new Exception("PrintWindow failed.");
            bitmap.Save(path,System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@
    $window = [ProofWindowCapture]::Find($app.Id)
    if (-not $window) { throw 'Harmony proof window not found after three seconds.' }
    [ProofWindowCapture]::Save($window,(Join-Path $PSScriptRoot $Arquivo))
    [ordered]@{opened=$true; pid=$app.Id; window_title=[ProofWindowCapture]::Title($window); window_handle=$window.ToInt64(); capture='PrintWindow of this proof window only'} |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'abertura.json')
    Get-Content (Join-Path $PSScriptRoot 'abertura.json')
} finally {
    if (-not $app.HasExited) {
        if ($window) { [ProofWindowCapture]::Close($window) } else { $app.CloseMainWindow() | Out-Null }
        if (-not $app.WaitForExit(10000)) { Stop-Process -Id $app.Id }
    }
}
