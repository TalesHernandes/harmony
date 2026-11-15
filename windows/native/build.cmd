@echo off
rem Builds windows\native\build\harmony-w1.exe (the W1 proof).
rem Run once first: bash windows/scripts/fetch-webrtc.sh and fetch-nvenc.sh
rem (both live outside the repository, in %USERPROFILE%\harmony-deps).
rem With the GPU module present (windows\native\gpu\gpu.h) it is linked in and
rem the screen goes through NVENC; without it the program still talks and watches.
setlocal
set "HERE=%~dp0"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
if not exist "%W%\lib\webrtc.lib" (
  echo libwebrtc is missing: run bash windows/scripts/fetch-webrtc.sh
  exit /b 1
)
set "GPU="
set "GPUDEF=/DHARMONY_GPU=0"
if exist "%HERE%gpu\gpu.h" (
  set "GPU=%HERE%gpu\*.cpp"
  set "GPUDEF=/DHARMONY_GPU=1"
)
for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%HERE%build\obj" mkdir "%HERE%build\obj"
pushd "%HERE%build"
rem /wd4068: libwebrtc's headers carry clang pragmas MSVC does not know.
cl /nologo /std:c++20 /O2 /MT /EHsc /Zi /utf-8 /Zc:__cplusplus /W3 /wd4068 /wd4996 /wd4146 /D_CRT_SECURE_NO_WARNINGS /MP ^
  /DNDEBUG /DWEBRTC_WIN /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE %GPUDEF% ^
  /I "%W%\include" /I "%W%\include\third_party\abseil-cpp" /I "%W%\include\third_party\libyuv\include" ^
  /I "%USERPROFILE%\harmony-deps\nvidia\Video_Codec_SDK_13.0.37\Interface" /I "%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.26100.0\cppwinrt" ^
  /Fo"obj\\" "%HERE%src\camera.cpp" "%HERE%src\main.cpp" "%HERE%src\voice.cpp" "%HERE%src\net.cpp" "%HERE%src\json.cpp" ^
  "%HERE%src\log.cpp" "%HERE%src\perf.cpp" "%HERE%src\stats.cpp" "%HERE%src\loopback.cpp" "%HERE%src\direct.cpp" "%HERE%src\audio.cpp" %GPU% ^
  /Fe:harmony-w1.exe ^
  /link /DEBUG /OPT:REF /OPT:ICF /MAP /SUBSYSTEM:CONSOLE "%W%\lib\webrtc.lib" winhttp.lib winmm.lib ws2_32.lib secur32.lib iphlpapi.lib ^
  msdmo.lib dmoguids.lib wmcodecdspuuid.lib strmiids.lib d3d11.lib dxgi.lib d3dcompiler.lib crypt32.lib ^
  ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib shell32.lib psapi.lib mfreadwrite.lib mfplat.lib mfuuid.lib mf.lib ^
  windowsapp.lib runtimeobject.lib dwmapi.lib mmdevapi.lib
set ERR=%ERRORLEVEL%
popd
if %ERR% neq 0 exit /b %ERR%
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%..\app\dfn-stage.ps1" -Destination "%HERE%build"
if errorlevel 1 exit /b 1
echo built %HERE%build\harmony-w1.exe
