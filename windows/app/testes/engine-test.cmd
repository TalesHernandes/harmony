@echo off
setlocal
set "APP=%~dp0.."
cd /d "%APP%\"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
set "SOURCE=%APP%\testes\engine-regressions.cpp"
set "DEST=%APP%\test-output\engine"
if not "%~1"=="" set "SOURCE=%~1"
if not "%~2"=="" set "DEST=%~2"
if not exist "%DEST%" mkdir "%DEST%"
set "SUPPORT=..\native\src\loopback.cpp ..\native\src\net.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\perf.cpp"
if "%~3"=="reuse" set "SUPPORT=%APP%\test-output\engine\loopback.obj %APP%\test-output\engine\net.obj %APP%\test-output\engine\json.obj %APP%\test-output\engine\log.obj %APP%\test-output\engine\perf.obj"
call "..\native\vc.cmd" cl /I"%APP%\src" /I"%APP%\testes" /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /Zc:__cplusplus /DNDEBUG /DWEBRTC_WIN /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /wd4068 /wd4996 /wd4146 /I"%APP%\." /I"%APP%\..\native\src" /I"%APP%\..\native\gpu" /I"%W%\include" /I"%W%\include\third_party\abseil-cpp" /I"%W%\include\third_party\libyuv\include" "%SOURCE%" %SUPPORT% /Fo"%DEST%\\" /Fe"%DEST%\engine-tests.exe" /link "%W%\lib\webrtc.lib" winhttp.lib winmm.lib ws2_32.lib secur32.lib iphlpapi.lib msdmo.lib dmoguids.lib wmcodecdspuuid.lib strmiids.lib d3d11.lib dxgi.lib d3dcompiler.lib crypt32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib shell32.lib psapi.lib mfplat.lib mfuuid.lib mf.lib windowsapp.lib runtimeobject.lib dwmapi.lib mmdevapi.lib
if errorlevel 1 exit /b 1
"%DEST%\engine-tests.exe"
exit /b %errorlevel%

