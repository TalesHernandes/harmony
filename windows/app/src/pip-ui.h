// Included inside App. The floating view consumes the existing call track;
// it never creates a PeerConnection, receiver, or decoder.
Window pipWindow{nullptr};
HWND pipHwnd{};
Grid pipRoot{nullptr};
std::shared_ptr<app::PanelRenderer> pipRenderer;
winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer pipTimer{nullptr};
bool pipActive=false, pipPositioned=false, pipDragging=false, pipMoved=false;
POINT pipPress{}, pipOrigin{};
int pipSlot=0;
ULONGLONG pipEndedAt=0;
std::string pipCall, pipControls;
bool pipWanted=false;                 // asked for by its button; never opens by itself
std::vector<UIElement> pipHover;      // what shows only under the pointer
struct PipGrip { bool on=false; std::string corner; POINT from{}; RECT was{}; } pipGrip;
int pipOn=-1;                         // Ajustes: -1 not read yet

// Ajustes → Aplicativo. On by default; the owner, 29/10/2026: "tem que ser
// opcional". Kept as a file, as the channel column's width (chat-ui.h).
// Since 02/11/2026 it only offers the button: the window opens when asked.
bool PipOn() {
  if(pipOn<0) { std::error_code e; pipOn=std::filesystem::exists(app::DataDirectory()/L"janelinha-desligada",e)?0:1; }
  return pipOn==1;
}
void SetPipOn(bool on) {
  pipOn=on?1:0;
  try { auto dir=app::DataDirectory(); std::filesystem::create_directories(dir);
    if(on) std::filesystem::remove(dir/L"janelinha-desligada"); else std::ofstream(dir/L"janelinha-desligada") << 1; } catch(...) {}
  logf("pip preference %s",on?"on":"off");
  if(!on) StopPip();
}
ToggleSwitch PipToggle() {
  ToggleSwitch toggle; HarmonyPointer(toggle); toggle.Name(L"PipToggle"); toggle.Header(box_value(L"Janelinha da chamada direta")); toggle.IsOn(PipOn());
  toggle.Toggled([this](auto const& sender, auto const&) { if(designOutput.empty()) SetPipOn(sender.template as<ToggleSwitch>().IsOn()); });
  return toggle;
}

// Every picture's renderer, so a block can ask its picture to fill it.
std::map<void*, std::weak_ptr<app::PanelRenderer>> pictureRenderers;
static void* PictureKey(IInspectable const& picture) { return winrt::get_abi(picture.as<winrt::Windows::Foundation::IUnknown>()); }
void KeepPicture(IInspectable const& picture, std::shared_ptr<app::PanelRenderer> const& r) {
  std::erase_if(pictureRenderers,[](auto const& e){return e.second.expired();});
  pictureRenderers[PictureKey(picture)]=r;
}
void CoverPicture(FrameworkElement const& picture) {
  auto found=pictureRenderers.find(PictureKey(picture));
  if(found!=pictureRenderers.end()) if(auto r=found->second.lock()) r->Cover(true);
}

static void RoundVideo(FrameworkElement const& panel, float radius) {
  using namespace winrt::Microsoft::UI::Xaml::Hosting;
  auto visual=ElementCompositionPreview::GetElementVisual(panel);
  auto geometry=visual.Compositor().CreateRoundedRectangleGeometry();
  geometry.CornerRadius({radius,radius});
  geometry.Size({float(panel.ActualWidth()),float(panel.ActualHeight())});
  visual.Clip(visual.Compositor().CreateGeometricClip(geometry));
  panel.SizeChanged([geometry](auto const&, auto const& e) {
    geometry.Size({float(e.NewSize().Width),float(e.NewSize().Height)});
  });
}

// There is something to float: their camera or their screen, in a live call.
bool PipAvailable(app::CallView const& v) { return PipOn() && v.phase == app::CallPhase::Live && (v.peerCam || v.peerScreen); }
Button PipButton(double box) {
  auto b = Square("pip", L"Abrir a janelinha", box, 16, Foreground, 0x2e2e31, [this] {
    if (!service) return; pipWanted = true; directSmall = true; directSignature.clear(); logf("pip opened by hand"); Render(); SyncPip(true);
  });
  b.Name(L"PipOpen"); b.Background(Brush(Default)); return b;
}

void StopPip() {
  if (!pipActive) return;
  pipActive=false;
  if(pipRenderer) pipRenderer->Size(0,0);
  if(service && pipSlot) service->CallVideo(pipSlot, {});
  pipSlot=0; pipRenderer.reset();
  if(pipRoot) pipRoot.Children().Clear();
  pipHover.clear(); pipGrip.on=false;
  if(pipHwnd) ShowWindow(pipHwnd,SW_HIDE);
  // Restore the original sink only after the floating sink is detached.
  if(service) for(auto const& [slot,weak]:directRenderers) if(auto r=weak.lock())
    service->CallVideo(slot,[r](auto track){r->Attach(track);});
  RefreshMediaVisibility(mediaShown);
  logf("pip hidden; main visibility=%d",mediaShown && !directSmall);
}

void ReturnFromPip() {
  directSmall=false; pipWanted=false; StopPip(); Open(); directSignature.clear(); DrawDirectCall();
}

void EnsurePip() {
  if(pipWindow) return;
  pipWindow=Window(); pipWindow.Title(L"harmony · chamada");
  check_hresult(pipWindow.as<IWindowNative>()->get_WindowHandle(&pipHwnd));
  auto presenter=pipWindow.AppWindow().Presenter().as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
  presenter.SetBorderAndTitleBar(false,false); presenter.IsResizable(false);
  presenter.IsMaximizable(false); presenter.IsMinimizable(false); presenter.IsAlwaysOnTop(true);
  SetWindowLongPtrW(pipHwnd,GWL_STYLE,(GetWindowLongPtrW(pipHwnd,GWL_STYLE)&~WS_OVERLAPPEDWINDOW)|WS_POPUP);
  DWORD border=0xfffffffe;DwmSetWindowAttribute(pipHwnd,DWMWA_BORDER_COLOR,&border,sizeof(border));
  SetWindowPos(pipHwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
  SetWindowLongPtrW(pipHwnd,GWL_EXSTYLE,(GetWindowLongPtrW(pipHwnd,GWL_EXSTYLE)|WS_EX_TOOLWINDOW)&~WS_EX_APPWINDOW);
  pipRoot=Grid(); pipRoot.Background(Brush(Background)); RoundVideo(pipRoot,8);
  pipRoot.PointerEntered([this](auto const&,auto const&){for(auto& part:pipHover) part.Opacity(1);});
  pipRoot.PointerExited([this](auto const&,auto const&){if(!pipGrip.on) for(auto& part:pipHover) part.Opacity(0);});
  pipWindow.Content(pipRoot);
  pipWindow.AppWindow().Closing([this](auto const&,auto const& e){if(!exiting){e.Cancel(true);ReturnFromPip();}});
  pipWindow.Closed([this](auto const&, auto const&){pipHwnd=nullptr;});
}

void DrawPip(app::CallView const& v, int slot) {
  if(pipRenderer) pipRenderer->Size(0,0);
  if(service && pipSlot) service->CallVideo(pipSlot,{});
  pipRoot.Children().Clear(); pipHover.clear(); pipRenderer.reset(); pipSlot=slot;
  Grid picture; picture.Background(Brush(Background));
  Automation::AutomationProperties::SetName(picture,L"Voltar à chamada");
  picture.PointerPressed([this](auto const& sender,auto const& e){
    auto picture=sender.template as<Grid>();
    if(!e.GetCurrentPoint(picture).Properties().IsLeftButtonPressed()) return;
    GetCursorPos(&pipPress); RECT r{};GetWindowRect(pipHwnd,&r);pipOrigin={r.left,r.top};
    pipDragging=true;pipMoved=false;picture.CapturePointer(e.Pointer());e.Handled(true);
  });
  picture.PointerMoved([this](auto const&,auto const&){
    if(!pipDragging) return;POINT p{};GetCursorPos(&p);
    if(abs(p.x-pipPress.x)+abs(p.y-pipPress.y)>4) pipMoved=true;
    if(pipMoved) SetWindowPos(pipHwnd,nullptr,pipOrigin.x+p.x-pipPress.x,pipOrigin.y+p.y-pipPress.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
  });
  picture.PointerReleased([this](auto const& sender,auto const& e){
    auto picture=sender.template as<Grid>();
    bool open=pipDragging&&!pipMoved;pipDragging=false;picture.ReleasePointerCapture(e.Pointer());e.Handled(true);
    if(open) ReturnFromPip();
  });
  picture.PointerCaptureLost([this](auto const&,auto const&){pipDragging=false;});
  if(slot) {
    SwapChainPanel panel; panel.Name(L"PipPicture"); panel.IsHitTestVisible(false); RoundVideo(panel,8);
    auto r=std::make_shared<app::PanelRenderer>(winrt::get_unknown(panel));pipRenderer=r;
    auto measure=[this,r](SwapChainPanel const& p){double dpi=p.XamlRoot()?p.XamlRoot().RasterizationScale():1;
      r->Size(pipActive?int(p.ActualWidth()*dpi):0,pipActive?int(p.ActualHeight()*dpi):0);};
    panel.SizeChanged([measure](auto const& s,auto const&){measure(s.template as<SwapChainPanel>());});
    panel.CompositionScaleChanged([measure](auto const& s,auto const&){measure(s.template as<SwapChainPanel>());});
    panel.Unloaded([r](auto const&,auto const&){r->Size(0,0);r->Unload();});
    service->CallVideo(slot,[r](auto track){r->Attach(track);});picture.Children().Append(panel);
  } else {
    auto text=Line(L"A imagem de "+PersonName(CallFace(v.peer))+L" acabou · A chamada continua",14,Foreground,500);
    text.TextWrapping(TextWrapping::Wrap);text.TextAlignment(TextAlignment::Center);text.Margin(Thickness{16,16,16,60});text.VerticalAlignment(VerticalAlignment::Center);
    text.IsHitTestVisible(false);picture.Children().Append(text);
  }
  pipRoot.Children().Append(picture);
  StackPanel controls;controls.Orientation(Orientation::Horizontal);controls.Spacing(8);
  controls.HorizontalAlignment(HorizontalAlignment::Center);controls.VerticalAlignment(VerticalAlignment::Bottom);controls.Margin(Thickness{8,8,8,8});
  controls.Children().Append(DirectMute(v,36));
  // 320 points wide: the bar's "Desligar" with its word would cover the
  // picture, so the floating view keeps only its red square, as Android's.
  auto hangup=HangupButton(L"Desligar");hangup.Width(36);hangup.Height(36);hangup.Padding(Thickness{0,0,0,0});
  hangup.Content(VoiceIcon("handset-arrow-out",Foreground,16));controls.Children().Append(hangup);
  if(slot==2) {
    auto volume=CallSquare("volume",Foreground,Default,0x2e2e31,L"Som da tela",[]{});
    volume.Width(36);volume.Height(36);volume.Content(VoiceIcon("volume",Foreground,16));
    Flyout fly;fly.Content(LiveVolume(v.peer.username,PersonName(CallFace(v.peer))));volume.Flyout(fly);controls.Children().Append(volume);
  }
  pipRoot.Children().Append(controls);
  // Its x, top right as on the floating cameras (main.cpp FloatingCameras).
  auto close=Square("xmark",L"Fechar a janelinha",28,14,Foreground,Default,[this]{
    pipWanted=false; StopPip(); directSignature.clear(); Render(); logf("pip closed");});
  close.Name(L"PipClose"); close.Background(Brush(Background)); close.HorizontalAlignment(HorizontalAlignment::Right); close.VerticalAlignment(VerticalAlignment::Top); close.Margin(Thickness{12,12,12,12});
  pipRoot.Children().Append(close);
  // The buttons only under the pointer (the owner, 29/10/2026: "devem
  // aparecer somente quando passar o mouse"); SyncPip hides them once it left.
  pipHover={controls,close};
  for(auto& part:pipHover) part.Opacity(0);
  // The four corners resize it, keeping its shape, the corner across still;
  // the picture is drawn again at its new size, not stretched.
  using Shape=winrt::Microsoft::UI::Input::InputSystemCursorShape;
  struct Corner { char const* name; Shape shape; HorizontalAlignment h; VerticalAlignment v; };
  for(auto const& c:{Corner{"nw",Shape::SizeNorthwestSoutheast,HorizontalAlignment::Left,VerticalAlignment::Top},Corner{"se",Shape::SizeNorthwestSoutheast,HorizontalAlignment::Right,VerticalAlignment::Bottom},
                     Corner{"ne",Shape::SizeNortheastSouthwest,HorizontalAlignment::Right,VerticalAlignment::Top},Corner{"sw",Shape::SizeNortheastSouthwest,HorizontalAlignment::Left,VerticalAlignment::Bottom}}) {
    Border grip; grip.Name(L"PipCorner"); grip.Width(16); grip.Height(16); grip.Background(SolidColorBrush(winrt::Windows::UI::Color{0,0,0,0}));
    grip.HorizontalAlignment(c.h); grip.VerticalAlignment(c.v); HarmonyPointer(grip,c.shape);
    std::string corner=c.name;
    grip.PointerPressed([this,corner](auto const& sender,auto const& e){
      pipGrip.on=true;pipGrip.corner=corner;GetCursorPos(&pipGrip.from);GetWindowRect(pipHwnd,&pipGrip.was);
      sender.template as<UIElement>().CapturePointer(e.Pointer());e.Handled(true);});
    grip.PointerMoved([this](auto const&,auto const& e){
      if(!pipGrip.on) return; POINT p{};GetCursorPos(&p);
      bool west=pipGrip.corner.find('w')!=std::string::npos,north=pipGrip.corner.find('n')!=std::string::npos;
      int scale=GetDpiForWindow(pipHwnd);MONITORINFO m{sizeof(m)};GetMonitorInfoW(MonitorFromWindow(pipHwnd,MONITOR_DEFAULTTONEAREST),&m);
      int was=pipGrip.was.right-pipGrip.was.left,dx=p.x-pipGrip.from.x,dy=p.y-pipGrip.from.y;
      // The axis pulled furthest decides, as on the floating cameras.
      int byW=west?was-dx:was+dx,byH=(north?(pipGrip.was.bottom-pipGrip.was.top)-dy:(pipGrip.was.bottom-pipGrip.was.top)+dy)*16/9;
      int w=std::abs(byW-was)>=std::abs(byH-was)?byW:byH;
      int most=(std::min)(int((m.rcWork.right-m.rcWork.left)*0.6),int((m.rcWork.bottom-m.rcWork.top)*0.6)*16/9);
      w=std::clamp(w,MulDiv(240,scale,96),(std::max)(MulDiv(240,scale,96),most));int h=w*9/16;
      int x=west?pipGrip.was.right-w:pipGrip.was.left,y=north?pipGrip.was.bottom-h:pipGrip.was.top;
      PlacePip(x,y,w,h);e.Handled(true);});
    auto release=[this](auto const& sender,auto const& e){if(!pipGrip.on) return;pipGrip.on=false;sender.template as<UIElement>().ReleasePointerCaptures();e.Handled(true);
      RECT r{};GetWindowRect(pipHwnd,&r);logf("pip resized to %dx%d",int(r.right-r.left),int(r.bottom-r.top));};
    grip.PointerReleased(release);grip.PointerCaptureLost([this](auto const&,auto const&){pipGrip.on=false;});
    grip.Tapped([](auto const&,auto const& e){e.Handled(true);});
    pipRoot.Children().Append(grip);
  }
}

// Through its AppWindow: the presenter is not resizable by the user's frame,
// and a size set behind its back is not what its content lays out to.
void PlacePip(int x,int y,int w,int h) {
  if(!pipWindow) return;
  pipWindow.AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{x,y,w,h});ShapePip();
}
// The window's own rounded edge follows its size.
void ShapePip() {
  if(!pipHwnd) return;
  RECT bounds{};GetWindowRect(pipHwnd,&bounds);int radius=MulDiv(16,GetDpiForWindow(pipHwnd),96);
  SetWindowRgn(pipHwnd,CreateRoundRectRgn(0,0,bounds.right-bounds.left+1,bounds.bottom-bounds.top+1,radius,radius),TRUE);
}

void SyncPip(bool rebind=false) {
  if(!service || exiting) return;
  auto v=service->CallRead();
  if(v.phase==app::CallPhase::Live) {
    if(!pipTimer) {pipTimer=window.DispatcherQueue().CreateTimer();pipTimer.Interval(std::chrono::milliseconds(250));
      pipTimer.Tick([this](auto const&,auto const&){SyncPip();});}
    if(!pipTimer.IsRunning()) pipTimer.Start();
  } else if(pipTimer) pipTimer.Stop();
  bool away=directSmall || !IsWindowVisible(hwnd) || IsIconic(hwnd);
  int slot=v.peerScreen?2:v.peerCam?1:0;
  if(v.phase!=app::CallPhase::Live) pipWanted=false;
  if(pipCall!=v.id) {StopPip();pipEndedAt=0;pipCall=v.id;pipWanted=false;}
  if(v.phase!=app::CallPhase::Live || !away || !PipOn() || !pipWanted) {StopPip();pipEndedAt=0;return;}
  if(pipActive && !pipGrip.on && !pipHover.empty() && pipHover[0].Opacity()>0) {
    POINT p{};RECT r{};GetCursorPos(&p);GetWindowRect(pipHwnd,&r);
    if(!PtInRect(&r,p)) for(auto& part:pipHover) part.Opacity(0);
  }
  if(slot) pipEndedAt=0;
  if(!slot && !pipActive) return;
  if(!slot && !pipEndedAt) pipEndedAt=GetTickCount64();
  // Their picture ended and the note was read: closed, and it waits for the
  // button again rather than coming back by itself with the next picture.
  if(pipEndedAt && GetTickCount64()-pipEndedAt>=4000) {pipWanted=false;StopPip();directSignature.clear();Render();return;}
  EnsurePip();
  bool opening=!pipActive;pipActive=true;
  // All main panels stop, including local previews and the unused peer slot.
  for(auto const& [key,weak]:directRenderers) if(auto r=weak.lock()) r->Size(0,0);
  auto controls=std::to_string(slot)+(v.muted?"m":"_")+(v.deafened?"d":"_");
  if(opening || pipSlot!=slot || pipControls!=controls) {pipControls=controls;DrawPip(v,slot);}
  else if(rebind && pipRenderer) {auto r=pipRenderer;service->CallVideo(slot,[r](auto track){r->Attach(track);});}
  if(opening) {
    if(!pipPositioned) {MONITORINFO m{sizeof(m)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&m);
      int scale=GetDpiForWindow(hwnd);int w=MulDiv(320,scale,96),h=MulDiv(180,scale,96);
      SetWindowPos(pipHwnd,HWND_TOPMOST,m.rcWork.right-w-16,m.rcWork.bottom-h-16,w,h,SWP_NOACTIVATE);pipPositioned=true;}
    ShapePip();
    ShowWindow(pipHwnd,SW_SHOWNOACTIVATE);
    logf("pip shown slot=%d; main renderers sized 0x0",slot);
  }
}
