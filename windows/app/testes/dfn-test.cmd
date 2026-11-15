@echo off
setlocal
set "APP=%~dp0.."
cd /d "%APP%\"
set "SOURCE=%APP%\testes\dfn-tests.cpp"
set "DEST=%APP%\test-output\dfn"
if not "%~1"=="" set "SOURCE=%~1"
if not "%~2"=="" set "DEST=%~2"
if not exist "%DEST%" mkdir "%DEST%"
call "..\native\vc.cmd" cl /I"%APP%\src" /I"%APP%\testes" /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /I"%APP%\..\native\src" "%SOURCE%" /Fo"%DEST%\\" /Fe"%DEST%\dfn-tests.exe" /link advapi32.lib
if errorlevel 1 exit /b 1
python "%APP%\testes\dfn-speech-test.py" --exe "%DEST%\dfn-tests.exe"
exit /b %errorlevel%
