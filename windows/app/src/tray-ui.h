// Included inside App. Own popup, with the same private font axes as WinUI.
DWORD TrayPopupStyle() {
#ifndef HARMONY_PUBLISH
  if(Measuring() && !AuditRealPointer()) return WS_EX_TOOLWINDOW;
#endif
  return WS_EX_TOOLWINDOW|WS_EX_TOPMOST;
}
HWND trayPopup=nullptr;
float trayAuditScale=0; // Only set by the unpublished offline rendering fixture.
int trayHot=-1;
std::vector<app::TrayItem> trayItems;
app::TrayCall TrayCallState() {
  auto v=ReadState().voice;auto direct=service->CallRead();
  // An outgoing call already has microphone controls and can be hung up;
  // an unanswered incoming ring has not joined a call yet.
  bool active=direct.phase==app::CallPhase::Calling || direct.phase==app::CallPhase::Connecting || direct.phase==app::CallPhase::Live;
  return active?app::TrayCall{true,true,direct.muted,direct.deafened}:app::TrayCall{!v.room.empty(),false,v.muted,v.deafened};
}
void CloseTrayMenu() {if(trayPopup)ShowWindow(trayPopup,SW_HIDE);trayHot=-1;}
void RefreshTrayMenu() {
  if(!trayPopup || !IsWindowVisible(trayPopup))return;
  auto next=app::TrayItems(TrayCallState());bool changed=next.size()!=trayItems.size();
  if(!changed)for(size_t i=0;i<next.size();++i)if(next[i].text!=trayItems[i].text || next[i].enabled!=trayItems[i].enabled)changed=true;
  if(!changed)return;
  trayItems=std::move(next);trayHot=-1;
  RECT r{};GetWindowRect(trayPopup,&r);double s=GetDpiForWindow(trayPopup)/96.0;
  MONITORINFO monitor{sizeof monitor};GetMonitorInfoW(MonitorFromWindow(trayPopup,MONITOR_DEFAULTTONEAREST),&monitor);
  int width=int(app::TrayWidth*s),height=int(app::TrayHeight(trayItems)*s);
  int x=std::clamp(int(r.left),int(monitor.rcWork.left),int(monitor.rcWork.right)-width);
  int y=std::clamp(int(r.bottom)-height,int(monitor.rcWork.top),int(monitor.rcWork.bottom)-height);
  SetWindowPos(trayPopup,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
  InvalidateRect(trayPopup,nullptr,FALSE);
}
void ChooseTrayItem(int index) {
  if(index<0 || index>=int(trayItems.size()) || !trayItems[index].enabled)return;
  auto action=trayItems[index].action;auto call=TrayCallState();CloseTrayMenu();
  if(action==app::TrayAction::Open)Open();else if(action==app::TrayAction::Quit)Quit();
  else if(call.active) {
    if(call.direct) {
      if(action==app::TrayAction::Mute && !call.deafened)service->CallMute(!call.muted);
      else if(action==app::TrayAction::Deafen)service->CallDeafen(!call.deafened);
      else if(action==app::TrayAction::Leave)service->CallHangup();
    } else {
      if(action==app::TrayAction::Mute && !call.deafened)service->MuteVoice(!call.muted,call.deafened);
      else if(action==app::TrayAction::Deafen)service->MuteVoice(call.muted,!call.deafened);
      else if(action==app::TrayAction::Leave)service->LeaveVoice();
    }
  }
}
void PaintTrayMenu(HWND popup,HDC supplied=nullptr) {
  PAINTSTRUCT paint{};HDC screen=supplied?supplied:BeginPaint(popup,&paint);RECT r{};GetClientRect(popup,&r);
  HDC dc=CreateCompatibleDC(screen);auto bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);auto old=SelectObject(dc,bitmap);
  try {
    using namespace installerui;ComPtr<ID2D1DCRenderTarget> target;
    auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96,D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    Check(drawing->CreateDCRenderTarget(&properties,&target));Check(target->BindDC(dc,&r));
    target->BeginDraw();float scale=trayAuditScale>0?trayAuditScale:GetDpiForWindow(popup)/96.f;
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    target->Clear(D2D1::ColorF(0x323235));ComPtr<ID2D1SolidColorBrush> brush;Check(target->CreateSolidColorBrush(D2D1::ColorF(0xfcfcfc),&brush));
    for(int i=0;i<int(trayItems.size());++i) {
      auto const& item=trayItems[i];bool hot=trayHot==i && item.enabled;
      float top=float(app::TrayTop(trayItems,i));
      // Red exits are solid buttons, as HarmonyDangerButton: #db3b3e, hover #e15451, white ink.
      bool solid=item.danger && item.enabled;float right=float(app::TrayWidth-4);
      brush->SetColor(solid?D2D1::ColorF(hot?0xe15451:0xdb3b3e):hot?D2D1::ColorF(0x27272a):D2D1::ColorF(0x323235));
      target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(4,top,right,top+36),8,8),brush.Get());
      brush->SetColor(D2D1::ColorF(!item.enabled?0x71717a:0xfcfcfc));
      auto glyph=TrayGeometry(app::TrayIcon(item));
      target->SetTransform(D2D1::Matrix3x2F::Translation(12,top+10)*D2D1::Matrix3x2F::Scale(scale,scale));
      target->FillGeometry(glyph.Get(),brush.Get());
      target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));
      auto layout=installerui::Layout(item.text,float(app::TrayWidth-44),22,13,500);
      target->DrawTextLayout(D2D1::Point2F(36,top+9),layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    Check(target->EndDraw());BitBlt(screen,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
  } catch(...) {logf("tray menu paint failed");}
  SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);if(!supplied)EndPaint(popup,&paint);
}
static LRESULT CALLBACK TrayPopupProc(HWND popup,UINT message,WPARAM w,LPARAM l) {
  auto self=reinterpret_cast<App*>(GetWindowLongPtrW(popup,GWLP_USERDATA));
  if(message==WM_NCCREATE) {self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(popup,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
  if(!self)return DefWindowProcW(popup,message,w,l);
  auto hit=[&] {double s=GetDpiForWindow(popup)/96.0;double x=SHORT(LOWORD(l))/s,y=SHORT(HIWORD(l))/s;
    if(x>=4 && x<app::TrayWidth-4)for(int i=0;i<int(self->trayItems.size());++i){int top=app::TrayTop(self->trayItems,i);if(y>=top && y<top+36)return i;}return -1;};
  switch(message) {
    case WM_NCCALCSIZE:return 0;
    case WM_NCHITTEST:return HTCLIENT;
    case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)self->CloseTrayMenu();return 0;
    case WM_DPICHANGED: {auto r=reinterpret_cast<RECT*>(l);SetWindowPos(popup,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_KEYDOWN:
      if(w==VK_ESCAPE)self->CloseTrayMenu();
      else if(w==VK_UP || w==VK_DOWN || w==VK_HOME || w==VK_END) {
        int count=int(self->trayItems.size());int step=w==VK_UP || w==VK_END?-1:1;
        if(w==VK_UP && self->trayHot<0)self->trayHot=0;
        if(w==VK_HOME)self->trayHot=-1;else if(w==VK_END)self->trayHot=0;
        for(int n=0;n<count;++n) {self->trayHot=(self->trayHot+step+count)%count;if(self->trayItems[self->trayHot].enabled)break;}
        InvalidateRect(popup,nullptr,FALSE);
      } else if(w==VK_RETURN || w==VK_SPACE)self->ChooseTrayItem(self->trayHot);
      return 0;
    case WM_MOUSEMOVE: {int row=hit();if(row!=self->trayHot) {self->trayHot=row;InvalidateRect(popup,nullptr,FALSE);}
      TRACKMOUSEEVENT track{sizeof track,TME_LEAVE,popup,0};TrackMouseEvent(&track);return 0;}
    case WM_MOUSELEAVE:self->trayHot=-1;InvalidateRect(popup,nullptr,FALSE);return 0;
    case WM_LBUTTONUP:self->ChooseTrayItem(hit());return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:self->PaintTrayMenu(popup);return 0;
    case WM_PRINTCLIENT:self->PaintTrayMenu(popup,reinterpret_cast<HDC>(w));return 0;
    case WM_CLOSE:self->CloseTrayMenu();return 0;
  }
  return DefWindowProcW(popup,message,w,l);
}
void OpenTrayMenu() {
  if(!trayPopup) {
    if(!installerui::writing)installerui::InitializeText((app::Executable().parent_path()/L"assets/Inter.ttf").c_str());
    WNDCLASSW cls{};cls.lpfnWndProc=TrayPopupProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Harmony.Native.TrayMenu";cls.hCursor=LoadCursorW(nullptr,IDC_HAND);RegisterClassW(&cls);
    trayPopup=CreateWindowExW(TrayPopupStyle(),cls.lpszClassName,L"harmony",WS_POPUP|WS_THICKFRAME,0,0,224,72,nullptr,nullptr,cls.hInstance,this);
    COLORREF noBorder=DWMWA_COLOR_NONE;DwmSetWindowAttribute(trayPopup,DWMWA_BORDER_COLOR,&noBorder,sizeof noBorder);
    BOOL dark=TRUE;DwmSetWindowAttribute(trayPopup,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof dark);
    DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;DwmSetWindowAttribute(trayPopup,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof corner);
    MARGINS shadow{1,1,1,1};DwmExtendFrameIntoClientArea(trayPopup,&shadow);
  }
  trayItems=app::TrayItems(TrayCallState());NOTIFYICONIDENTIFIER id{sizeof id};id.hWnd=control;id.uID=tray.uID;RECT anchor{};
  if(FAILED(Shell_NotifyIconGetRect(&id,&anchor))) {POINT p{};GetCursorPos(&p);anchor={p.x,p.y,p.x+1,p.y+1};}
  if(!designOutput.empty())anchor={2600,700,2601,701};
  MONITORINFO monitor{sizeof monitor};GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor);auto work=monitor.rcWork;
  // Establish the tray monitor before querying its DPI, independently of the main window.
  SetWindowPos(trayPopup,nullptr,work.left,work.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
  double scale=GetDpiForWindow(trayPopup)/96.0;int width=int(app::TrayWidth*scale),height=int(app::TrayHeight(trayItems)*scale);
  int x=(anchor.left+anchor.right-width)/2,y=anchor.top-height-4;
  if(anchor.bottom<=work.top)y=anchor.bottom+4;
  else if(anchor.right<=work.left) {x=anchor.right+4;y=anchor.top;}
  else if(anchor.left>=work.right) {x=anchor.left-width-4;y=anchor.top;}
  x=std::clamp(x,int(work.left),int(work.right)-width);y=std::clamp(y,int(work.top),int(work.bottom)-height);
  trayHot=-1;
#ifndef HARMONY_PUBLISH
  if(Measuring() && !AuditRealPointer()) {
    SetWindowPos(trayPopup,nullptr,x,y,width,height,SWP_SHOWWINDOW|SWP_NOZORDER|SWP_NOACTIVATE);
  } else
#endif
  {
    SetWindowPos(trayPopup,HWND_TOPMOST,x,y,width,height,SWP_SHOWWINDOW);
    SetForegroundWindow(trayPopup);SetFocus(trayPopup);
  }
  InvalidateRect(trayPopup,nullptr,FALSE);
}
