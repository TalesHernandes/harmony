param([string]$Source)
$ErrorActionPreference='Stop'
$app=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=Join-Path $app 'test-output\packages'
New-Item -ItemType Directory -Force $output | Out-Null
$env:TEMP=Join-Path $app 'test-output\temp'
$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
if (-not $Source) {
    & (Join-Path $PSScriptRoot '..\build.cmd') -Publish
    if ($LASTEXITCODE) { throw 'Publication fixture build failed.' }
    $Source=Join-Path $app 'out'
}
& (Join-Path $PSScriptRoot 'package.ps1') -TestOnly -Release 126 -Source $Source
# A new native version exists only in the disposable fixture, never in out/.
$next=Join-Path $app 'test-output\next-native'
New-Item -ItemType Directory -Force $next | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $next -Recurse -Force
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FixtureVersion {
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryEx(string path,IntPtr file,uint flags);
    [DllImport("kernel32.dll")] static extern IntPtr FindResource(IntPtr module,IntPtr name,IntPtr type);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module,IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module,IntPtr resource);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr BeginUpdateResource(string path,bool delete);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool UpdateResource(IntPtr handle,IntPtr type,IntPtr name,ushort lang,byte[] data,uint size);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool EndUpdateResource(IntPtr handle,bool discard);
    public static void Next(string path) {
        var module=LoadLibraryEx(path,IntPtr.Zero,2);
        if(module==IntPtr.Zero) throw new Exception("Fixture resource load failed");
        byte[] data;
        try {
            var resource=FindResource(module,(IntPtr)1,(IntPtr)16);
            data=new byte[SizeofResource(module,resource)];
            Marshal.Copy(LockResource(LoadResource(module,resource)),data,0,data.Length);
        } finally { FreeLibrary(module); }
        // Change the string and both fixed version DWORDs in this copy only.
        bool changed=false;
        for(int i=0;i+5<data.Length;i+=2) if(data[i]==50&&data[i+1]==0&&data[i+2]==54&&data[i+3]==0&&data[i+4]==0&&data[i+5]==0) { data[i+2]=55; changed=true; }
        if(!changed) throw new Exception("Fixture native version 26 missing");
        for(int i=0;i+24<data.Length;i+=4) if(BitConverter.ToUInt32(data,i)==0xFEEF04BD) { Array.Copy(BitConverter.GetBytes(27),0,data,i+12,4); Array.Copy(BitConverter.GetBytes(27),0,data,i+20,4); break; }
        var update=BeginUpdateResource(path,false);
        if(update==IntPtr.Zero) throw new Exception("Fixture update failed");
        if(!UpdateResource(update,(IntPtr)16,(IntPtr)1,1033,data,(uint)data.Length)) { EndUpdateResource(update,true); throw new Exception("Fixture version write failed"); }
        if(!EndUpdateResource(update,false)) throw new Exception("Fixture version commit failed");
    }
}
'@
[FixtureVersion]::Next((Join-Path $next 'harmony-nativo.exe'))
if ((Get-Item (Join-Path $next 'harmony-nativo.exe')).VersionInfo.ProductPrivatePart -ne 27) { throw 'Next native fixture version mismatch.' }
& (Join-Path $PSScriptRoot 'package.ps1') -TestOnly -Release 127 -Source $next
$vc=Join-Path $app '..\native\vc.cmd'
& $vc cl /nologo /std:c++20 /EHsc /MT /O1 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /DHARMONY_INSTALLER_TEST_ONLY "/Fo$output\\" "/Fe$output\session-fixture.exe" (Join-Path $PSScriptRoot 'session-fixture.cpp') (Join-Path $app 'src/platform.cpp') (Join-Path $app '..\native\src\net.cpp') (Join-Path $app '..\native\src\json.cpp') (Join-Path $app '..\native\src\log.cpp') (Join-Path $app '..\native\src\perf.cpp') /link shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if ($LASTEXITCODE) { throw 'Session fixture compilation failed.' }
& $vc cl /nologo /std:c++20 /EHsc /MT /O1 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /DHARMONY_INSTALLER_TEST_ONLY "/Fo$output\\" "/Fe$output\user-launch-fixture.exe" (Join-Path $PSScriptRoot 'user-launch-fixture.cpp') (Join-Path $app 'src/platform.cpp') (Join-Path $app '..\native\src\net.cpp') (Join-Path $app '..\native\src\json.cpp') (Join-Path $app '..\native\src\log.cpp') (Join-Path $app '..\native\src\perf.cpp') /link /SUBSYSTEM:WINDOWS shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if ($LASTEXITCODE) { throw 'User launch fixture compilation failed.' }
# A real executable that exits before UI startup, inside an otherwise complete
# package. It exercises readiness failure after successful hashes/installation.
$broken=Join-Path $app 'test-output\broken-native'
New-Item -ItemType Directory -Force $broken | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $broken -Recurse -Force
$source=Join-Path $output 'broken.cpp'
[IO.File]::WriteAllText($source,'#include <windows.h>' + "`n" + 'int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){return 7;}')
& (Join-Path $app '..\native\vc.cmd') cl /nologo /MT /O1 "/Fo$output\broken.obj" "/Fe$broken\harmony-nativo.exe" $source /link /SUBSYSTEM:WINDOWS
if($LASTEXITCODE) { throw 'Failure fixture compilation failed.' }
& (Join-Path $PSScriptRoot 'package.ps1') -TestOnly -Release 128 -Source $broken
& (Join-Path $PSScriptRoot 'package.ps1') -TestOnly -Release 129 -Source $next
