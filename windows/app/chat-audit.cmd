@echo off
setlocal
cd /d "%~dp0..\.."
set "HARMONY_E2E_PORT=8099"
if not exist "windows\app\test-output\temp" mkdir "windows\app\test-output\temp"
set "TEMP=%CD%\windows\app\test-output\temp"
set "TMP=%TEMP%"
set "GOCACHE=%CD%\windows\app\test-output\go-cache"
set "GOFLAGS=-buildvcs=false"
if not defined PLAYWRIGHT_BROWSERS_PATH set "PLAYWRIGHT_BROWSERS_PATH=%USERPROFILE%\.pw-browsers"
node windows\app\testes\chat-audit.mjs %*
exit /b %errorlevel%
