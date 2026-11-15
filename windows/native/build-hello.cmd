@echo off
setlocal
set "HERE=%~dp0"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%HERE%build" mkdir "%HERE%build"
pushd "%HERE%build"
cl /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /Zc:__cplusplus /DWEBRTC_WIN /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE ^
  /I "%W%\include" /I "%W%\include\third_party\abseil-cpp" /I "%W%\include\third_party\libyuv\include" ^
  "%HERE%src\hello.cpp" /Fe:hello.exe ^
  /link "%W%\lib\webrtc.lib" winmm.lib ws2_32.lib secur32.lib iphlpapi.lib msdmo.lib dmoguids.lib wmcodecdspuuid.lib strmiids.lib d3d11.lib dxgi.lib crypt32.lib ole32.lib oleaut32.lib user32.lib advapi32.lib shell32.lib
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
