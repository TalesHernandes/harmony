#include "media-sources.h"
#include <dwmapi.h>
#include <filesystem>
#include <vector>
namespace app {
namespace {
// The name a person knows a monitor by ("DELL U2419H"), where Windows only
// gives \\.\DISPLAY2: the display configuration maps one to the other.
std::wstring MonitorName(const wchar_t* device) {
  UINT32 paths=0,modes=0;
  if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes)!=ERROR_SUCCESS) return {};
  std::vector<DISPLAYCONFIG_PATH_INFO> path(paths); std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
  if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,path.data(),&modes,mode.data(),nullptr)!=ERROR_SUCCESS) return {};
  for(UINT32 i=0;i<paths;++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{}; source.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; source.header.size=sizeof(source);
    source.header.adapterId=path[i].sourceInfo.adapterId; source.header.id=path[i].sourceInfo.id;
    if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName,device)) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{}; target.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; target.header.size=sizeof(target);
    target.header.adapterId=path[i].targetInfo.adapterId; target.header.id=path[i].targetInfo.id;
    if(DisplayConfigGetDeviceInfo(&target.header)==ERROR_SUCCESS) return target.monitorFriendlyDeviceName;
  }
  return {};
}
}
std::vector<ScreenChoice> ScreenSources() {
  std::vector<ScreenChoice> out;
  EnumWindows([](HWND window,LPARAM data)->BOOL {
    auto& out=*reinterpret_cast<std::vector<ScreenChoice>*>(data);
    DWORD pid=0,affinity=0,cloaked=0; GetWindowThreadProcessId(window,&pid);
    DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)); GetWindowDisplayAffinity(window,&affinity);
    if(!media::Capturable({bool(IsWindowVisible(window)),bool(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW),bool(cloaked),pid==GetCurrentProcessId(),affinity!=WDA_NONE})) return TRUE;
    if(GetWindow(window,GW_OWNER)) return TRUE;
    wchar_t title[1024]{}; if(!GetWindowTextW(window,title,1024)) return TRUE;
    ScreenChoice c; c.window=window; c.pid=pid; c.title=title; c.minimized=IsIconic(window);
    RECT r{}; if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r)))) GetWindowRect(window,&r);
    c.width=int(r.right-r.left); c.height=int(r.bottom-r.top);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process) { wchar_t path[32768]{}; DWORD size=32768; if(QueryFullProcessImageNameW(process,0,path,&size)) c.executable=std::filesystem::path(path).filename().wstring(); CloseHandle(process); }
    if(c.executable.empty()) c.executable=L"não encontrado";
    out.push_back(std::move(c)); return TRUE;
  },reinterpret_cast<LPARAM>(&out));
  EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT r,LPARAM data)->BOOL {
    auto& out=*reinterpret_cast<std::vector<ScreenChoice>*>(data); MONITORINFOEXW info{}; info.cbSize=sizeof(info); GetMonitorInfoW(monitor,&info);
    ScreenChoice c; c.monitor=monitor; c.width=int(r->right-r->left); c.height=int(r->bottom-r->top); c.primary=info.dwFlags&MONITORINFOF_PRIMARY;
    // "Tela 2 · DELL U2419H": the number Windows shows in its own settings,
    // and the monitor's name when it gives one.
    std::wstring number; for(auto ch:std::wstring(info.szDevice)) if(ch>=L'0'&&ch<=L'9') number+=ch;
    auto name=MonitorName(info.szDevice);
    c.title=L"Tela"+(number.empty()?std::wstring():L" "+number)+(name.empty()?std::wstring():L" · "+name);
    out.push_back(std::move(c)); return TRUE;
  },reinterpret_cast<LPARAM>(&out));
  return out;
}
}
