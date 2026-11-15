#pragma once
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")
#include <string>
#include "media-policy.h"
namespace media {
inline std::wstring FollowExecutable(HWND window) {
  DWORD pid=0;if(window)GetWindowThreadProcessId(window,&pid);else pid=GetCurrentProcessId();
  HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!process)return {};
  wchar_t path[32768]{};DWORD length=32768;
  bool ok=QueryFullProcessImageNameW(process,0,path,&length);CloseHandle(process);
  return ok?std::wstring(path,length):std::wstring();
}
inline HWND FollowWindow(const std::wstring& original,HWND previous) {
  struct Search {
    std::wstring original,own,windows;HWND foreground=nullptr,best=nullptr,previous=nullptr;FollowChoice choice;
  } search;
  wchar_t windows[32768]{};GetWindowsDirectoryW(windows,32768);
  search.original=original;search.own=FollowExecutable(nullptr);search.windows=windows;search.previous=previous;
  search.foreground=GetAncestor(GetForegroundWindow(),GA_ROOT);
  EnumWindows([](HWND window,LPARAM data)->BOOL {
    auto& s=*reinterpret_cast<Search*>(data);
    if(window==s.previous)return TRUE;
    DWORD pid=0,cloaked=0,affinity=0;GetWindowThreadProcessId(window,&pid);
    DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof cloaked);GetWindowDisplayAffinity(window,&affinity);
    if(!Capturable({bool(IsWindowVisible(window)),bool(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW),bool(cloaked),pid==GetCurrentProcessId(),affinity!=WDA_NONE})||
        GetWindow(window,GW_OWNER)||!GetWindowTextLengthW(window))return TRUE;
    if(!FollowFolder(s.original,FollowExecutable(window),s.own,s.windows))return TRUE;
    RECT r{};if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof r)))GetWindowRect(window,&r);
    if(r.right<=r.left||r.bottom<=r.top)return TRUE;
    FollowChoice choice{window==s.foreground,uint64_t(r.right-r.left)*uint64_t(r.bottom-r.top)};
    if(!s.best||FollowBetter(choice,s.choice)){s.best=window;s.choice=choice;}
    return TRUE;
  },reinterpret_cast<LPARAM>(&search));
  return search.best;
}
}
