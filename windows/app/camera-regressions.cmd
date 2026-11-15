@echo off
setlocal
cd /d "%~dp0"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
set "SOURCE=..\native\src\camera.cpp"
set "HARNESS=testes\camera-regressions.cpp"
set "DEST=test-output\camera"
if not "%~1"=="" set "SOURCE=%~1"
if not "%~2"=="" set "DEST=%~2"
if not "%~3"=="" set "HARNESS=%~3"
if not exist "%DEST%" mkdir "%DEST%"
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DWIN32_LEAN_AND_MEAN /DNDEBUG /DWEBRTC_WIN /DNOMINMAX /wd4068 /I"%~dp0..\native\src" /I"%W%\include" /I"%W%\include\third_party\abseil-cpp" /I"%W%\include\third_party\libyuv\include" "%HARNESS%" "%SOURCE%" ..\native\src\net.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\perf.cpp /Fo"%DEST%\\" /Fe"%DEST%\camera-tests.exe" /link "%W%\lib\webrtc.lib" mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib oleaut32.lib user32.lib advapi32.lib winhttp.lib psapi.lib winmm.lib ws2_32.lib secur32.lib iphlpapi.lib dmoguids.lib wmcodecdspuuid.lib msdmo.lib strmiids.lib crypt32.lib shell32.lib
if errorlevel 1 exit /b 1
"%DEST%\camera-tests.exe"
exit /b %errorlevel%
