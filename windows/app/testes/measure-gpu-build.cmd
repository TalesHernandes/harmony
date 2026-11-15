@echo off
setlocal
set "HERE=%~dp0..\..\native\"
set "DEST=%HERE%build"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
set "N=%USERPROFILE%\harmony-deps\nvidia\Video_Codec_SDK_13.0.37"
set "SDK=%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.26100.0\cppwinrt"
set "VS="
for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo ERROR: Visual Studio C++ toolchain is unavailable.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%DEST%" mkdir "%DEST%"
pushd "%DEST%"
cl /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /Zc:__cplusplus /DNDEBUG /DWEBRTC_WIN /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE ^
  /I "%W%\include" /I "%W%\include\third_party\abseil-cpp" /I "%W%\include\third_party\libyuv\include" ^
  /I "%N%\Interface" /I "%SDK%" /I "%HERE%gpu" ^
  device.obj capture.obj nvenc.obj mf.obj encoder.obj decoder.obj presenter.obj log.obj perf.obj json.obj ^
  "%HERE%gpu\selftest\main.cpp" /Fe:gpu_selftest.exe ^
  /link "%W%\lib\webrtc.lib" winmm.lib ws2_32.lib secur32.lib iphlpapi.lib msdmo.lib dmoguids.lib wmcodecdspuuid.lib strmiids.lib ^
  d3d11.lib dxgi.lib crypt32.lib ole32.lib oleaut32.lib user32.lib advapi32.lib shell32.lib ^
  mfplat.lib mfuuid.lib mf.lib windowsapp.lib runtimeobject.lib psapi.lib
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
