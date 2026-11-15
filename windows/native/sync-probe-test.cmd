@echo off
setlocal
set "HERE=%~dp0"
if not exist "%HERE%build\sync-test" mkdir "%HERE%build\sync-test"
call "%HERE%vc.cmd" cl /nologo /std:c++20 /EHsc /MT /utf-8 /DNOMINMAX "%HERE%src\sync-detectors-test.cpp" /Fo"%HERE%build\sync-test\\" /Fe"%HERE%build\sync-test\sync-detectors-test.exe"
if errorlevel 1 exit /b 1
"%HERE%build\sync-test\sync-detectors-test.exe"
exit /b %errorlevel%
