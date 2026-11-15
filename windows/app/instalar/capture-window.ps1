param([Parameter(Mandatory=$true)][int]$ProcessId, [Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class WindowPhoto {
    public static uint Dpi;
    public static int Width, Height;
    delegate bool Callback(IntPtr window,IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int left,top,right,bottom; }
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback,IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
    public static void Save(uint pid,string output) {
        SetThreadDpiAwarenessContext((IntPtr)(-4));
        IntPtr found=IntPtr.Zero;
        EnumWindows((w,p)=> {uint owner;GetWindowThreadProcessId(w,out owner);if(owner==pid&&IsWindowVisible(w))found=w;return true;},IntPtr.Zero);
        if(found==IntPtr.Zero)throw new Exception("Visible process window not found");
        Rect r;GetWindowRect(found,out r);
        Dpi=GetDpiForWindow(found);Width=r.right-r.left;Height=r.bottom-r.top;
        using(var bitmap=new Bitmap(r.right-r.left,r.bottom-r.top)) {
            using(var graphics=Graphics.FromImage(bitmap)) {
                var dc=graphics.GetHdc();try {if(!PrintWindow(found,dc,2))throw new Exception("PrintWindow failed");}finally{graphics.ReleaseHdc(dc);}
            }
            bitmap.Save(output,ImageFormat.Png);
        }
    }
}
'@
[WindowPhoto]::Save($ProcessId,[IO.Path]::GetFullPath($Output))
@{dpi=[WindowPhoto]::Dpi;width=[WindowPhoto]::Width;height=[WindowPhoto]::Height} | ConvertTo-Json | Set-Content -LiteralPath ($Output+'.json') -Encoding UTF8
