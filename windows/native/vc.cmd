@echo off
rem Runs a command with the Visual Studio x64 compiler on the PATH: vc.cmd cl ...
rem A prompt that already has the compiler skips vcvars64.bat, which hangs on
rem this machine for minutes at a time (17/10/2026, see docs/DEPLOY.md W5).
where cl.exe >nul 2>&1 && goto run
for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
:run
%*
