#pragma once
#include <windows.h>
namespace app {
inline bool ExcludeTransmissionNotice(HWND window) {
  DWORD affinity=0;
  return window&&SetWindowDisplayAffinity(window,WDA_EXCLUDEFROMCAPTURE)&&GetWindowDisplayAffinity(window,&affinity)&&affinity==WDA_EXCLUDEFROMCAPTURE;
}
inline HWND TestTransmissionNotice(HWND target) {
  WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Harmony.Transmission.TestNotice";
  cls.lpfnWndProc=[](HWND h,UINT message,WPARAM w,LPARAM l)->LRESULT {
    if(message==WM_PAINT){PAINTSTRUCT paint;auto dc=BeginPaint(h,&paint);RECT r;GetClientRect(h,&r);auto brush=CreateSolidBrush(RGB(255,0,255));FillRect(dc,&r,brush);DeleteObject(brush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));DrawTextW(dc,L"Você está transmitindo — teste W4",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);EndPaint(h,&paint);return 0;}
    return DefWindowProcW(h,message,w,l);
  };RegisterClassW(&cls);
  RECT r{};GetClientRect(target,&r);POINT point{r.right/2-160,int(r.bottom*0.2)-18};ClientToScreen(target,&point);
  auto window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Você está transmitindo — teste W4",WS_POPUP,point.x,point.y,320,36,nullptr,nullptr,cls.hInstance,nullptr);
  if(!ExcludeTransmissionNotice(window)){if(window)DestroyWindow(window);return nullptr;}
  ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);return window;
}
}
