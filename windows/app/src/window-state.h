#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

namespace app {
// Normal bounds in DIPs relative to the monitor work area. Persist neither
// minimized coordinates nor absolute pixels from a possibly absent monitor.
struct WindowState {
  std::wstring monitor;
  double x=0,y=0,width=960,height=680;
  bool maximized=false;
};
inline bool ValidWindowState(WindowState const& s) {
  return !s.monitor.empty() && s.monitor.size()<CCHDEVICENAME &&
    std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.width) && std::isfinite(s.height) &&
    std::abs(s.x)<100000 && std::abs(s.y)<100000 && s.width>=240 && s.width<=32768 && s.height>=160 && s.height<=32768;
}
inline RECT FitWindowState(WindowState const& s,RECT work,UINT dpi,bool sameMonitor) {
  double scale=std::max(96u,dpi)/96.0;
  LONG width=std::clamp(LONG(std::lround(s.width*scale)),1L,work.right-work.left);
  LONG height=std::clamp(LONG(std::lround(s.height*scale)),1L,work.bottom-work.top);
  LONG x=sameMonitor?work.left+LONG(std::lround(s.x*scale)):work.left+(work.right-work.left-width)/2;
  LONG y=sameMonitor?work.top+LONG(std::lround(s.y*scale)):work.top+(work.bottom-work.top-height)/2;
  x=std::clamp(x,work.left,work.right-width);y=std::clamp(y,work.top,work.bottom-height);
  return {x,y,x+width,y+height};
}
inline bool WriteWindowState(std::filesystem::path const& path,WindowState const& s) {
  if(!ValidWindowState(s))return false;
  try {
    std::filesystem::create_directories(path.parent_path());
    auto pending=path;pending+=L".pending";
    std::wofstream out(pending,std::ios::trunc);
    out<<1<<' '<<std::quoted(s.monitor)<<' '<<s.x<<' '<<s.y<<' '<<s.width<<' '<<s.height<<' '<<s.maximized;
    out.close();
    if(out && MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    DeleteFileW(pending.c_str());
  } catch(...) {}
  return false;
}
inline bool ReadWindowState(std::filesystem::path const& path,WindowState& state) {
  try {
    if(std::filesystem::file_size(path)>2048)return false;
    std::wifstream in(path); int version=0; WindowState s;
    if(!(in>>version>>std::quoted(s.monitor)>>s.x>>s.y>>s.width>>s.height>>s.maximized) || version!=1 || !ValidWindowState(s))return false;
    in>>std::ws;if(!in.eof())return false;
    state=s;return true;
  } catch(...) {return false;}
}
inline WindowState CaptureWindowState(HWND window) {
  WINDOWPLACEMENT p{sizeof p};GetWindowPlacement(window,&p);
  MONITORINFOEXW m{};m.cbSize=sizeof m;
  GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&m);
  // WINDOWPLACEMENT uses workspace coordinates for a top-level app window.
  auto r=p.rcNormalPosition;double scale=GetDpiForWindow(window)/96.0;
  WindowState s;s.monitor=m.szDevice;
  s.x=(r.left-m.rcMonitor.left)/scale;s.y=(r.top-m.rcMonitor.top)/scale;
  s.width=(r.right-r.left)/scale;s.height=(r.bottom-r.top)/scale;
  s.maximized=p.showCmd==SW_SHOWMAXIMIZED || (p.showCmd==SW_SHOWMINIMIZED && (p.flags&WPF_RESTORETOMAXIMIZED));
  return s;
}
inline void RestoreWindowState(HWND window,WindowState const& state) {
  if(!ValidWindowState(state))return;
  struct Search {std::wstring name;HMONITOR found=nullptr;} search{state.monitor};
  EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
    auto& s=*reinterpret_cast<Search*>(data);MONITORINFOEXW m{};m.cbSize=sizeof m;
    if(GetMonitorInfoW(monitor,&m) && s.name==m.szDevice)s.found=monitor;
    return TRUE;
  },reinterpret_cast<LPARAM>(&search));
  bool same=search.found!=nullptr;
  MONITORINFO m{sizeof m};GetMonitorInfoW(same?search.found:MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&m);
  // Establish the target monitor before asking Windows for its current DPI.
  SetWindowPos(window,nullptr,m.rcWork.left,m.rcWork.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
  auto r=FitWindowState(state,m.rcWork,GetDpiForWindow(window),same);
  SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
  // SetWindowPlacement keeps a maximized launch hidden until App::Open.
  WINDOWPLACEMENT p{sizeof p};GetWindowPlacement(window,&p);
  p.rcNormalPosition={r.left-(m.rcWork.left-m.rcMonitor.left),r.top-(m.rcWork.top-m.rcMonitor.top),
    r.right-(m.rcWork.left-m.rcMonitor.left),r.bottom-(m.rcWork.top-m.rcMonitor.top)};
  p.showCmd=SW_HIDE;p.flags=state.maximized?WPF_RESTORETOMAXIMIZED:0;
  SetWindowPlacement(window,&p);
}
}
