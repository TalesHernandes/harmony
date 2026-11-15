@echo off
setlocal
set "HARMONY_TEST_MONITOR_X=1920"
cd /d "%~dp0"
if not exist "test-output" mkdir "test-output"
if not exist "test-output\temp" mkdir "test-output\temp"
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX testes\w8d-resource-tests.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-w8d-resource-tests.exe /link psapi.lib user32.lib
if errorlevel 1 exit /b 1
test-output\harmony-w8d-resource-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX testes\panel-policy-tests.cpp /Fotest-output\ /Fetest-output\harmony-panel-policy-tests.exe
if errorlevel 1 exit /b 1
test-output\harmony-panel-policy-tests.exe
if errorlevel 1 exit /b 1
set "TEMP=%~dp0test-output\temp"
set "TMP=%TEMP%"
set "LOCALAPPDATA=%~dp0test-output"
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE testes\w8a-tests.cpp /Fotest-output\ /Fetest-output\harmony-w8a-tests.exe /link user32.lib
if errorlevel 1 exit /b 1
test-output\harmony-w8a-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX testes\photo-crop-tests.cpp /Fotest-output\ /Fetest-output\harmony-photo-crop-tests.exe
if errorlevel 1 exit /b 1
test-output\harmony-photo-crop-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX testes\sound-rules-tests.cpp /Fotest-output\ /Fetest-output\harmony-sound-rules-tests.exe
if errorlevel 1 exit /b 1
test-output\harmony-sound-rules-tests.exe
if errorlevel 1 exit /b 1
python testes\emoji-from-web.py --check
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX testes\emoji-tests.cpp src\emoji.cpp /Fotest-output\ /Fetest-output\harmony-emoji-tests.exe
if errorlevel 1 exit /b 1
test-output\harmony-emoji-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS testes\chat-tests.cpp src\chat.cpp src\upload.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\net.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-chat-tests.exe /link user32.lib winhttp.lib psapi.lib bcrypt.lib
if errorlevel 1 exit /b 1
test-output\harmony-chat-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS testes\tests.cpp testes\voice-tests.cpp testes\voice-test-engine.cpp src\service.cpp src\rooms.cpp src\chat.cpp src\upload.cpp src\platform.cpp src\direct-call.cpp src\notices.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\net.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-app-tests.exe /link shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib ws2_32.lib bcrypt.lib
if errorlevel 1 exit /b 1
test-output\harmony-app-tests.exe %*
if errorlevel 1 exit /b 1
call "%~dp0testes\dfn-test.cmd"
if errorlevel 1 exit /b 1
call "%~dp0testes\engine-test.cmd"
if errorlevel 1 exit /b 1
call "%~dp0testes\engine-test.cmd" "%~dp0testes\w8c-tests.cpp" "%~dp0test-output\w8c"
if errorlevel 1 exit /b 1
call "%~dp0testes\engine-test.cmd" "%~dp0testes\gpu-fallback-tests.cpp" "%~dp0test-output\gpu-fallback"
if errorlevel 1 exit /b 1
python testes\w4-regressions.py
if errorlevel 1 exit /b 1
python testes\w4-round5.py
if errorlevel 1 exit /b 1
python testes\w4-round4.py
if errorlevel 1 exit /b 1
python testes\w4-platform-regressions.py
if errorlevel 1 exit /b 1
call "%~dp0camera-regressions.cmd"
if errorlevel 1 exit /b 1
"%~dp0..\native\build\gpu_selftest.exe" --keyframes-retry
if errorlevel 1 exit /b 1
"%~dp0..\native\build\gpu_selftest.exe" --vbv-probe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS testes\w6-tests.cpp src\direct-call.cpp src\notices.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\net.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-w6-tests.exe /link user32.lib winhttp.lib psapi.lib
if errorlevel 1 exit /b 1
test-output\harmony-w6-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE testes\toast-tests.cpp src\toast.cpp src\platform.cpp ..\native\src\net.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-toast-tests.exe /link windowsapp.lib runtimeobject.lib shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if errorlevel 1 exit /b 1
test-output\harmony-toast-tests.exe
if errorlevel 1 exit /b 1
if not exist "test-output\speaking" mkdir "test-output\speaking"
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DHARMONY_SPEAKING_TEST testes\voice-tests.cpp /Fotest-output\speaking\ /Fetest-output\speaking\test.exe
if errorlevel 1 exit /b 1
test-output\speaking\test.exe
if errorlevel 1 exit /b 1
python -c "from pathlib import Path; a=Path.cwd(); o=a/'test-output/speaking'; h=(a/'src/voice-state.h').read_text(encoding='utf-8'); assert 'kSpeakingFloor = 0.001;' in h; h=h.replace('kSpeakingFloor = 0.001;', 'kSpeakingFloor = 2.5 / 128.0;').replace('../../native/src/json.h', (a.parent/'native/src/json.h').as_posix()); (o/'voice-state.h').write_text(h,encoding='utf-8'); (o/'mutant.cpp').write_text((a/'testes/voice-tests.cpp').read_text(encoding='utf-8').replace('../src/voice-state.h', 'voice-state.h').replace('../../native/src/speaking-level.h', (a.parent/'native/src/speaking-level.h').as_posix()),encoding='utf-8')"
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DHARMONY_SPEAKING_TEST test-output\speaking\mutant.cpp /Fotest-output\speaking\ /Fetest-output\speaking\mutant.exe
if errorlevel 1 exit /b 1
test-output\speaking\mutant.exe > test-output\speaking\mutation.log 2>&1
if not errorlevel 1 exit /b 1
findstr /c:"FAIL: speaking lights quiet audio above -60 dBFS" test-output\speaking\mutation.log
if errorlevel 1 exit /b 1
echo PASS: speaking regression kills the old 2.5 byte-RMS threshold
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX /DUNICODE /D_UNICODE testes\updater-tests.cpp src\platform.cpp ..\native\src\net.cpp ..\native\src\json.cpp ..\native\src\log.cpp ..\native\src\perf.cpp /Fotest-output\ /Fetest-output\harmony-updater-tests.exe /link shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if errorlevel 1 exit /b 1
test-output\harmony-updater-tests.exe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE instalar\verified-launch-tests.cpp /Fotest-output\ /Fetest-output\harmony-verified-launch-tests.exe /link test-output\platform.obj test-output\net.obj test-output\json.obj test-output\log.obj test-output\perf.obj shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if errorlevel 1 exit /b 1
test-output\harmony-verified-launch-tests.exe "test-output\verified-launch-%RANDOM%"
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE instalar\fast-launch-tests.cpp /Fotest-output\ /Fetest-output\harmony-fast-launch-tests.exe /link test-output\platform.obj test-output\net.obj test-output\json.obj test-output\log.obj test-output\perf.obj shell32.lib crypt32.lib advapi32.lib user32.lib winhttp.lib psapi.lib ole32.lib propsys.lib
if errorlevel 1 exit /b 1
test-output\harmony-fast-launch-tests.exe "test-output\fast-launch-%RANDOM%"
if errorlevel 1 exit /b 1
node testes\update-transport-tests.mjs
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File testes\installer-tests.ps1
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File testes\installer-state-tests.ps1
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File instalar\cabinet-tests.ps1 -Observe
if errorlevel 1 exit /b 1
call "..\native\vc.cmd" cl /nologo /std:c++20 /EHsc /MT /utf-8 /I"%~dp0src" /I"%~dp0testes" /DNOMINMAX instalar\installer-text-tests.cpp /Fotest-output\installer-text-tests.obj /Fetest-output\installer-text-tests.exe /link dwrite.lib d2d1.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1
test-output\installer-text-tests.exe "%~dp0assets\Inter.ttf"
exit /b %errorlevel%
