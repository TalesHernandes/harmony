// Simulate the Explorer double click that gives an installer foreground rights.
// A background Node spawn alone has no such right. Click only our test window.
#include "../src/installer-context.h"
#include <shellapi.h>
namespace {
std::wstring command;
HANDLE child{};
bool launched=false;
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
  if(message==WM_LBUTTONUP && !launched) {
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&startup,&process)) { PostQuitMessage(1);return 0; }
    AllowSetForegroundWindow(process.dwProcessId);
    ResumeThread(process.hThread);CloseHandle(process.hThread);child=process.hProcess;
    launched=true;ShowWindow(window,SW_HIDE);SetTimer(window,1,100,nullptr);return 0;
  }
  if(message==WM_TIMER && child && WaitForSingleObject(child,0)==WAIT_OBJECT_0) {
    DWORD code{};GetExitCodeProcess(child,&code);CloseHandle(child);child=nullptr;PostQuitMessage(int(code));return 0;
  }
  return DefWindowProcW(window,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
  try {
    auto root=app::installer::TestRoot();if(root.empty())return 2;
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc!=2)return 2;
    auto exe=std::filesystem::weakly_canonical(argv[1]);LocalFree(argv);
    auto relative=exe.lexically_relative(std::filesystem::weakly_canonical(root));
    if(relative.empty() || *relative.begin()==L".." || exe.extension()!=L".exe")return 2;
    command=L"\""+exe.wstring()+L"\"";
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=Proc;cls.lpszClassName=L"harmony.fixture.user-launch";RegisterClassW(&cls);
    auto window=CreateWindowExW(WS_EX_TOPMOST,cls.lpszClassName,L"harmony isolated launch fixture",WS_POPUP|WS_VISIBLE,40,40,240,100,nullptr,nullptr,instance,nullptr);
    if(!window)return 12;
    // Node hides the child's first show; override that only for this click target.
    ShowWindow(window,SW_SHOWNOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    POINT point{160,90},before{};GetCursorPos(&before);
    if(WindowFromPoint(point)!=window)return 13;
    SetCursorPos(point.x,point.y);
    INPUT input[2]{};input[0].type=input[1].type=INPUT_MOUSE;input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(2,input,sizeof(INPUT))!=2)return 14;
    MSG message{};auto deadline=GetTickCount64()+300000;
    while(GetTickCount64()<deadline) {
      if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
        if(message.message==WM_QUIT) {SetCursorPos(before.x,before.y);return int(message.wParam);}
        TranslateMessage(&message);DispatchMessageW(&message);
      } else Sleep(10);
    }
    return 1;
  } catch (...) { return 2; }
}
