@echo off
setlocal
call "%~dp0..\test.cmd" > "%~dp0..\test-output\optimization-test-final.log" 2>&1
set "RESULT=%ERRORLEVEL%"
> "%~dp0..\test-output\optimization-test-final.exit.txt" echo %RESULT%
exit /b %RESULT%
