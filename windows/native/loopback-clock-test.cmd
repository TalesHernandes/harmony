@echo off
setlocal
set "HERE=%~dp0"
set "W=%USERPROFILE%\harmony-deps\webrtc-m155.8059.4.1\webrtc"
if not exist "%HERE%build\clock-test" mkdir "%HERE%build\clock-test"
call "%HERE%vc.cmd" cl /nologo /std:c++20 /EHsc /MT /utf-8 /DWEBRTC_WIN /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%W%\include" /I "%W%\include\third_party\abseil-cpp" "%HERE%src\loopback-clock-test.cpp" /Fo"%HERE%build\clock-test\\" /Fe"%HERE%build\clock-test\loopback-clock-test.exe"
if errorlevel 1 exit /b 1
"%HERE%build\clock-test\loopback-clock-test.exe"
