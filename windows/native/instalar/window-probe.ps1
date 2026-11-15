$env:TEMP = Join-Path $PSScriptRoot 'test-fixtures\temp'
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
if (-not ('InstalledProofWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class InstalledProofWindow {
    delegate bool Callback(IntPtr window,IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback,IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window,StringBuilder text,int length);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
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
    public static bool Close(IntPtr window) { return PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero); }
}
'@
}
