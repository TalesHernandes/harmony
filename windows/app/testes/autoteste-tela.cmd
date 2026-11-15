@echo off
setlocal
set "APP=%~dp0.."
cd /d "%APP%\..\.."
if not exist "windows\app\queixas\w4" mkdir "windows\app\queixas\w4"
if not exist "windows\app\test-output\temp" mkdir "windows\app\test-output\temp"
set "TEMP=%CD%\windows\app\test-output\temp"
set "TMP=%TEMP%"
set "LOCALAPPDATA=%CD%\windows\app\test-output\localappdata"
set "GOCACHE=%CD%\windows\app\test-output\go-cache"
set "GOFLAGS=-buildvcs=false"
set "PLAYWRIGHT_BROWSERS_PATH=%USERPROFILE%\.pw-browsers"
set "HARMONY_E2E_PORT=8107"
node windows\app\testes\autoteste-processes.mjs --preflight
if errorlevel 1 exit /b 1
call windows\native\vc.cmd cl /nologo /std:c++20 /EHsc /MT /utf-8 /DNOMINMAX windows\app\testes\media-game.cpp windows\native\src\json.cpp /Fowindows\app\queixas\w4\ /Fewindows\app\queixas\w4\media-game.exe /link ole32.lib propsys.lib user32.lib gdi32.lib winmm.lib
if errorlevel 1 exit /b 1
node windows\app\testes\autoteste-tela.mjs %*
exit /b %errorlevel%
