"""Compile the exact production Win32 callbacks with controlled OS edges."""
from pathlib import Path
import json
import subprocess
import sys

app = Path(__file__).resolve().parent.parent
root = app.parent / 'native'
out = app / 'test-output' / 'w4-platform'
out.mkdir(parents=True, exist_ok=True)
ui = (app / 'src/media-ui.h').read_text(encoding='utf-8')
audio = (root / 'src/loopback.cpp').read_text(encoding='utf-8')
inventory = (app/'testes/media-inventory.cpp').read_text(encoding='utf-8')

def block(text, anchor):
    start = text.index(anchor)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def run(name, ui_source, audio_source,inventory_source=inventory):
    directory = out / name
    directory.mkdir(exist_ok=True)
    callback = block(ui_source, 'cls.lpfnWndProc=[](HWND h,UINT m,WPARAM w,LPARAM l)->LRESULT')
    callback = callback.split('=', 1)[1]
    visibility = block(ui_source, 'void RefreshMediaVisibility')
    quality=block((app/'src/main.cpp').read_text(encoding='utf-8'),'void SyncQualityTimer')
    com = block(audio_source, 'struct ComScope') + ';'
    ancestry = block(audio_source, 'bool IncludesHarmony')
    inventory_process = block(inventory_source,'if(CreateProcessW(nullptr,command.data()')
    save = block(inventory_source,'static bool Save(')
    harness = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <map>
#include <string>
#include <functional>
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace json {struct Value{std::function<std::string()> serialize;std::string Dump()const{return serialize();}};}
SAVE
struct Service {int calls=0;void Watch(const char*,void*){++calls;}void WatchSize(double,double,bool){++calls;}};
struct App {Service* service=nullptr;bool exiting=false;HWND videoWindow=nullptr;std::string videoWho;
std::map<std::string,std::function<void(bool)>> mediaVisibility;
void FullscreenVideo(HWND){}
bool mediaShown=true;void RefreshCoveredTile(){}
bool settings=false,inCall=false;std::string settingsTab="diag";
struct History {bool active=false;void Active(bool v){active=v;}} diagnosticHistory;
struct Timer {bool enabled=false;explicit operator bool()const{return true;}bool IsEnabled()const{return enabled;}void Start(){enabled=true;}void Stop(){enabled=false;}} qualityTimer;
bool InCall(){return inCall;}
QUALITY
VISIBILITY
};
HRESULT initResult=S_OK;int initializes=0,uninitializes=0;
HRESULT FakeInitialize(void*,DWORD){++initializes;return initResult;}
void FakeUninitialize(){++uninitializes;}
#define CoInitializeEx FakeInitialize
#define CoUninitialize FakeUninitialize
COM
#undef CoInitializeEx
#undef CoUninitialize
int cursor=0;bool snapshotFailed=false;
HANDLE FakeSnapshot(DWORD,DWORD){cursor=0;return snapshotFailed?INVALID_HANDLE_VALUE:reinterpret_cast<HANDLE>(1);}
BOOL FakeNext(HANDLE,PROCESSENTRY32W* entry){DWORD ids[]{100,50,10,60};DWORD parents[]{50,10,0,0};if(cursor==4)return FALSE;entry->th32ProcessID=ids[cursor];entry->th32ParentProcessID=parents[cursor++];return TRUE;}
BOOL FakeClose(HANDLE){return TRUE;}
DWORD FakePid(){return 100;}
#define CreateToolhelp32Snapshot FakeSnapshot
#define Process32FirstW FakeNext
#define Process32NextW FakeNext
#define CloseHandle FakeClose
#define GetCurrentProcessId FakePid
ANCESTRY
#undef CloseHandle
DWORD waited=0;int waits=0,terminated=0;
BOOL FakeCreate(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION p){p->hProcess=reinterpret_cast<HANDLE>(1);p->hThread=reinterpret_cast<HANDLE>(2);return TRUE;}
DWORD FakeWait(HANDLE,DWORD milliseconds){if(++waits==1){waited=milliseconds;return WAIT_TIMEOUT;}return WAIT_OBJECT_0;}
BOOL FakeTerminate(HANDLE,UINT){++terminated;return TRUE;}
BOOL FakeExit(HANDLE,DWORD* code){*code=1;return TRUE;}
#define CreateProcessW FakeCreate
#define WaitForSingleObject FakeWait
#define TerminateProcess FakeTerminate
#define GetExitCodeProcess FakeExit
#define CloseHandle FakeClose
bool InventoryDeadline(){std::wstring command=L"fixture";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};bool timeout=false;DWORD code=1;
INVENTORY
return timeout&&waited==30000&&terminated==1;}
#undef CreateProcessW
#undef WaitForSingleObject
#undef TerminateProcess
#undef GetExitCodeProcess
#undef CloseHandle
int main(){
 wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);auto checkpoint=std::filesystem::path(executable).parent_path()/"checkpoint.json";
 std::ofstream(checkpoint)<<"old";bool oldSurvived=false;json::Value value{[&]{std::ifstream file(checkpoint);std::string bytes((std::istreambuf_iterator<char>(file)),{});oldSurvived=bytes=="old";return std::string("new");}};
 bool saved=Save(checkpoint,value);std::ifstream finalFile(checkpoint);std::string finalBytes((std::istreambuf_iterator<char>(finalFile)),{});
 if(!saved||!oldSurvived||finalBytes!="new"){puts("inventory checkpoint survives interrupted replacement");return 1;}
 if(!InventoryDeadline()){puts("inventory camera opening has external deadline");return 1;}
 if(!IncludesHarmony(100)||!IncludesHarmony(50)||!IncludesHarmony(10)||IncludesHarmony(60)||IncludesHarmony(0)){puts("ancestral game audio must reject harmony tree");return 1;}
 snapshotFailed=true;if(!IncludesHarmony(50)){puts("ancestral snapshot failure must fail closed");return 1;}
 for(HRESULT result:{S_OK,S_FALSE,E_FAIL}){initResult=result;int before=uninitializes;{ComScope scope;}if(uninitializes-before!=(SUCCEEDED(result)?1:0)){puts("COM success is balanced and failure is untouched");return 1;}}
 auto proc=CALLBACK;
 WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Harmony.W4.Callback.Regression";cls.lpfnWndProc=proc;RegisterClassW(&cls);
 App app;Service service;app.service=&service;
 HWND h=CreateWindowW(cls.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,40,40,nullptr,nullptr,cls.hInstance,&app);if(!h)return 2;
 service.calls=0;app.exiting=true;proc(h,WM_SIZE,0,0);
 if(service.calls){puts("video callbacks ignore exiting service");return 1;}
 app.service=nullptr;proc(h,WM_SIZE,0,0);proc(h,WM_CLOSE,0,0);
 bool visible=true;app.service=&service;app.exiting=false;app.mediaVisibility["tile"]=[&](bool value){visible=value;};app.RefreshMediaVisibility(false);
 if(visible){puts("tray visibility reaches media panels");return 1;}
 app.RefreshMediaVisibility(true);if(!visible)return 1;
 app.inCall=true;app.settings=true;app.RefreshMediaVisibility(true);
 if(!app.qualityTimer.enabled||!app.diagnosticHistory.active)return 1;
 app.RefreshMediaVisibility(false);if(app.qualityTimer.enabled||app.diagnosticHistory.active)return 1;
 app.inCall=false;app.RefreshMediaVisibility(true);if(app.qualityTimer.enabled)return 1;
 puts("Win32 shutdown, visibility, ancestral audio and COM passed");return 0;
}
'''
    harness = harness.replace('QUALITY',quality).replace('VISIBILITY', visibility).replace('COM\n', com+'\n').replace('ANCESTRY\n', ancestry+'\n').replace('CALLBACK;', callback+';').replace('INVENTORY\n',inventory_process+'\n').replace('SAVE\n',save+'\n')
    source = directory / 'test.cpp'
    source.write_text(harness, encoding='utf-8')
    exe = directory / 'test.exe'
    built = subprocess.run(['cmd','/c',str(root/'vc.cmd'),'cl','/nologo','/std:c++20','/EHsc','/MT','/utf-8',str(source),'/Fo'+str(directory/'test.obj'),'/Fe'+str(exe),'/link','user32.lib'], capture_output=True, text=True)
    (directory/'build.log').write_text(built.stdout+built.stderr, encoding='utf-8')
    if built.returncode:
        raise RuntimeError('Platform regression failed to compile: '+name)
    tested = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    (directory/'test.log').write_text(tested.stdout+tested.stderr, encoding='utf-8')
    return tested.returncode

assert run('baseline',ui,audio)==0
results=[]
if '--mutate' in sys.argv:
    for name,unit,before,after in [
        ('quit-video-guard','ui','self->service&&!self->exiting','self->service'),
        ('tray-panel-notification','ui','update(visible);','update(true);'),
        ('ancestral-audio-tree','audio','if(pid==target)return true;','if(false)return true;'),
        ('com-apartment-balance','audio','if(SUCCEEDED(hr))CoUninitialize();','/* omit COM cleanup */'),
        ('inventory-camera-deadline','inventory','WaitForSingleObject(process.hProcess,30000)','WaitForSingleObject(process.hProcess,INFINITE)'),
        ('inventory-checkpoint-atomic','inventory','pending+=L".pending";','/* write directly to visible checkpoint */'),
    ]:
        original=ui if unit=='ui' else inventory if unit=='inventory' else audio
        assert before in original
        mutated=original.replace(before,after) # Both close and size paths.
        code=run(name,mutated if unit=='ui' else ui,mutated if unit=='audio' else audio,mutated if unit=='inventory' else inventory)
        assert code==1,name
        results.append(dict(case=name,detected=True))
        print(name+': detected',flush=True)
(out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
