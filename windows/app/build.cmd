@echo off
setlocal
cd /d "%~dp0"
set "NUGET_PACKAGES=%~dp0.nuget-packages"
set "TEMP=%~dp0temp"
set "TMP=%~dp0temp"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%~dp0assets" mkdir "%~dp0assets"
call "%~dp0..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /utf-8 "%~dp0testes\unpack-font.cpp" /Fe"%~dp0temp\unpack-font.exe" /Fo"%~dp0temp\unpack-font.obj" /link dwrite.lib
if errorlevel 1 exit /b 1
"%~dp0temp\unpack-font.exe" "%~dp0..\..\web\harmony\src\fonts\inter-latin.woff2" "%~dp0assets\Inter.ttf"
if errorlevel 1 (
  echo DirectWrite could not unpack the existing Inter font. No new dependency allowed.
  exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found.
  exit /b 1
)
set "PROOF_MSBUILD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do if not defined PROOF_MSBUILD set "PROOF_MSBUILD=%%i"
if not defined PROOF_MSBUILD (
  echo MSBuild with C++ tools not found.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0testes\build.ps1" %*
exit /b %errorlevel%
