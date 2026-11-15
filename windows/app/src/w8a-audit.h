// Offline effects run without activation; physical input latency is opt-in.
winrt::Windows::Foundation::IAsyncAction W8aAudit() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
#ifndef HARMONY_W8A_BASELINE
  if(PlacementAudit()) {co_await W8aPlacementAudit();co_return;}
#endif
  std::ofstream report(designOutput/L"w8a.json");
  report<<'{';
  std::string stage="position";
  try {
    Border neutral;neutral.Name(L"HeaderIsland");neutral.Width(24);neutral.Height(48);neutral.Background(Brush(0x060607));neutral.HorizontalAlignment(HorizontalAlignment::Right);neutral.VerticalAlignment(VerticalAlignment::Top);
    root.Children().Append(neutral);root.UpdateLayout();
    ChatAuditPosition();
    root.Children().RemoveAtEnd();
    submit.IsEnabled(true);
    auto move=[&](FrameworkElement const& element,double fraction=.5) {
      AuditRequireRealPointer();
      auto at=element.TransformToVisual(nullptr).TransformPoint({float(element.ActualWidth()*fraction),float(element.ActualHeight()/2)});
      double scale=GetDpiForWindow(hwnd)/96.0; POINT p{LONG(at.X*scale),LONG(at.Y*scale)}; ClientToScreen(hwnd,&p);
      INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
      input.mi.dx=LONG((p.x-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
      input.mi.dy=LONG((p.y-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
      ParityRequire(AuditSendInput(1,&input,sizeof input)==1);
    };
    if(AuditRealPointer()) {
    co_await ChatAuditForeground("w8a physical delivery");co_await ui;
    auto now=[] { LARGE_INTEGER t,f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return double(t.QuadPart)*1000/f.QuadPart; };
    { // Optional physical delivery: toolbar hover latency.
    auto heard=std::make_shared<double>(0);
    submit.PointerEntered([heard,now](auto const&,auto const&) {*heard=now();});
    double worst=0,total=0; int lit=0;
    for(int i=0;i<20;++i) {
      move(captionButtons[0]); co_await winrt::resume_after(std::chrono::milliseconds(40)); co_await ui;
      *heard=0; double start=now(); move(submit);
      for(int n=0;n<100 && !*heard;++n) {co_await winrt::resume_after(std::chrono::milliseconds(4));co_await ui;}
      if(*heard) {double ms=*heard-start;worst=std::max(worst,ms);total+=ms;++lit;}
    }
    report<<"\"foreground\":"<<(GetForegroundWindow()==hwnd?"true":"false")<<",\"hover_samples\":"<<lit<<",\"hover_mean_ms\":"<<(lit?total/lit:0)<<",\"hover_worst_ms\":"<<worst;
    ParityRequire(lit==20);
    }
    { // Optional physical delivery: non-client close hover.
    stage="caption";move(captionButtons[2]);
    co_await winrt::resume_after(std::chrono::milliseconds(50));co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"caption-50ms.png"); co_await ui;
    co_await winrt::resume_after(std::chrono::milliseconds(1500));co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"caption-1500ms.png");co_await ui;
#ifndef HARMONY_W8A_BASELINE
    logf("w8a caption after 1500ms: index %d background %lx",captionHot,static_cast<unsigned long>(DesignColor(captionButtons[2].Background())));
    ParityRequire(captionHot==2 && DesignColor(captionButtons[2].Background())==0xdb3b3e);
#endif
    }
    { // Optional physical delivery: channel-row hover latency.
    stage="channel-hover";
    root.Children().Clear();root.ColumnDefinitions().Clear();root.RowDefinitions().Clear();
    StackPanel channels;channels.Width(260);channels.Margin(Thickness{24,40,0,0});channels.HorizontalAlignment(HorizontalAlignment::Left);channels.VerticalAlignment(VerticalAlignment::Top);
    channels.Children().Append(Text(L"CANAIS",13));
    std::vector<FrameworkElement> rows;auto rowHeard=std::make_shared<std::vector<double>>(6,0);
    for(int k=0;k<6;++k) {
      auto row=ChannelRow(L"canal de teste "+std::to_wstring(k+1),"hashtag",false,k,0,false,[]{}).as<FrameworkElement>();
      row.PointerEntered([rowHeard,k,now](auto const&,auto const&){(*rowHeard)[k]=now();});
      channels.Children().Append(row);rows.push_back(row);
    }
    root.Children().Append(channels);root.UpdateLayout();
    co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    double rowWorst=0,rowTotal=0;int rowCount=0;
    for(int round=0;round<20;++round)for(int k=0;k<6;++k) {
      (*rowHeard)[k]=0;double start=now();move(rows[k]);
      for(int n=0;n<100 && !(*rowHeard)[k];++n){co_await winrt::resume_after(std::chrono::milliseconds(4));co_await ui;}
      if((*rowHeard)[k]){double ms=(*rowHeard)[k]-start;rowWorst=std::max(rowWorst,ms);rowTotal+=ms;++rowCount;}
    }
    std::fill(rowHeard->begin(),rowHeard->end(),0);
    for(auto const& row:rows){move(row);co_await winrt::resume_after(std::chrono::milliseconds(16));co_await ui;}
    co_await winrt::resume_after(std::chrono::milliseconds(100));co_await ui;
    int fastRows=0;for(auto time:*rowHeard)fastRows+=time>0;
    report<<",\"channel_samples\":"<<rowCount<<",\"channel_mean_ms\":"<<(rowCount?rowTotal/rowCount:0)<<",\"channel_worst_ms\":"<<rowWorst<<",\"channel_fast_rows\":"<<fastRows;
    ParityRequire(rowCount==120 && fastRows==6);
    co_await SaveDesignWindow(hwnd,designOutput,L"channel-hover.png");co_await ui;
    }
    } else {
      logf("w8a toolbar/row hover latency and real non-client close hover: skipped (HARMONY_AUDIT_REAL_POINTER)");
      report<<"\"physical_hover_skipped\":true";
    }
    stage="slider";
    root.Children().Clear();root.ColumnDefinitions().Clear();root.RowDefinitions().Clear();
    StackPanel fixture;fixture.HorizontalAlignment(HorizontalAlignment::Center);fixture.VerticalAlignment(VerticalAlignment::Center);fixture.Spacing(16);
    fixture.Children().Append(Text(L"Microfone",15));
    Slider slider;slider.Name(L"DfnSlider");slider.Width(400);slider.Minimum(0);slider.Maximum(100);slider.Value(50);fixture.Children().Append(slider);root.Children().Append(fixture);root.UpdateLayout();
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    slider.Value(70);
    co_await winrt::resume_after(std::chrono::milliseconds(100));co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"slider-value.png");co_await ui;
    report<<",\"slider_value\":"<<slider.Value()<<",\"slider_tooltip_enabled\":"<<(slider.IsThumbToolTipEnabled()?"true":"false");
    ParityRequire(slider.Value()>55);
#ifndef HARMONY_W8A_BASELINE
    ParityRequire(!slider.IsThumbToolTipEnabled());
#endif
    stage="tray";OpenTrayMenu();
    co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
    co_await SaveDesignWindow(trayPopup,designOutput,L"tray-idle.png");co_await ui;
    CloseTrayMenu();
#ifndef HARMONY_W8A_BASELINE
    designState.voice.Join("w8a-room","sala de teste");designState.voice.connecting=false;
    OpenTrayMenu();co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    ParityRequire(trayItems.size()==5 && trayItems[1].text==L"Mutar");
    SendMessageW(trayPopup,WM_KEYDOWN,VK_UP,0);ParityRequire(trayHot==4);
    SendMessageW(trayPopup,WM_KEYDOWN,VK_DOWN,0);ParityRequire(trayHot==0);
    SendMessageW(trayPopup,WM_KEYDOWN,VK_END,0);ParityRequire(trayHot==4);
    SendMessageW(trayPopup,WM_KEYDOWN,VK_HOME,0);ParityRequire(trayHot==0);
    trayHot=-1;InvalidateRect(trayPopup,nullptr,FALSE);
    co_await SaveDesignWindow(trayPopup,designOutput,L"tray-call.png");co_await ui;
    if(app::Environment(L"HARMONY_W8A_HOLD")=="1") {co_await winrt::resume_after(std::chrono::seconds(25));co_await ui;}
    designState.voice.muted=true;RefreshTrayMenu();
    co_await winrt::resume_after(std::chrono::milliseconds(150));co_await ui;
    ParityRequire(trayItems[1].text==L"Desmutar");
    co_await SaveDesignWindow(trayPopup,designOutput,L"tray-muted.png");co_await ui;
    designState.voice.deafened=true;RefreshTrayMenu();
    co_await winrt::resume_after(std::chrono::milliseconds(150));co_await ui;
    ParityRequire(!trayItems[1].enabled && trayItems[2].text==L"Desensurdecer");
    ChooseTrayItem(1);ParityRequire(IsWindowVisible(trayPopup));
    co_await SaveDesignWindow(trayPopup,designOutput,L"tray-deafened.png");co_await ui;
    designState.voice.room.clear();RefreshTrayMenu();ParityRequire(trayItems.size()==2);CloseTrayMenu();
    stage="placement";
    Open();
    bool maximize=AuditRealPointer();
    if(maximize)AuditWindowState(true);
    else logf("w8a maximized placement: skipped (nonactivating maximize API not found); normal placement remains tested");
    co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
    auto saved=app::CaptureWindowState(hwnd);ParityRequire(saved.maximized==maximize);
    ParityRequire(app::WriteWindowState(designOutput/L"window-state.txt",saved));
    ShowWindow(hwnd,SW_HIDE);Open();ParityRequire(bool(IsZoomed(hwnd))==maximize);
    co_await SaveDesignWindow(hwnd,designOutput,L"window-maximized.png");co_await ui;
    AuditWindowState(false);
    co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    app::WindowState disk;ParityRequire(app::ReadWindowState(designOutput/L"window-state.txt",disk));
    SetWindowPos(hwnd,nullptr,2140,500,600,400,SWP_NOZORDER|SWP_NOACTIVATE);
    app::RestoreWindowState(hwnd,disk);startupMaximized=disk.maximized;Open();
    co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
    ParityRequire(bool(IsZoomed(hwnd))==maximize);AuditWindowState(false);
    co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    auto normal=app::CaptureWindowState(hwnd);
    ParityRequire(std::abs(normal.width-saved.width)<2 && std::abs(normal.height-saved.height)<2 && std::abs(normal.x-saved.x)<2 && std::abs(normal.y-saved.y)<2);
    co_await SaveDesignWindow(hwnd,designOutput,L"window-restored.png");co_await ui;
    report<<",\"placement_roundtrip\":true,\"maximized_placement_skipped\":"<<(maximize?"false":"true")<<",\"tray_state_refresh\":true";
#endif
    report<<",\"passed\":true}";
  } catch(hresult_error const& e) {logf("w8a %s failed: %s",stage.c_str(),to_string(e.message()).c_str());report<<",\"passed\":false,\"stage\":\""<<stage<<"\",\"hresult\":"<<e.code().value<<"}";}
  co_await ui;
  report.close(); window.Close(); Exit();
}
#ifndef HARMONY_W8A_BASELINE
// Separate launches exercise the production SavePlacement/OnLaunched/Open
// path against the fixture's own DataDirectory, including a maximized start.
winrt::Windows::Foundation::IAsyncAction W8aPlacementAudit() {
  auto lifetime=get_strong();winrt::apartment_context ui;
  bool passed=false;std::string phase=app::Environment(L"HARMONY_W8A_PLACEMENT_PHASE");
  try {
    if(phase=="save") {
      AuditWindowState(false);MONITORINFO m{sizeof m};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&m);
      SetWindowPos(hwnd,nullptr,m.rcWork.left+130,m.rcWork.top+90,900,650,SWP_NOZORDER|SWP_NOACTIVATE);
      AuditWindowState(true);co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
      SavePlacement();
    }
    app::WindowState saved;ParityRequire(app::ReadWindowState(app::DataDirectory()/L"window-state.txt",saved));
    ParityRequire(IsZoomed(hwnd) && saved.maximized);
    co_await SaveDesignWindow(hwnd,designOutput,L"cold-maximized.png");co_await ui;
    if(phase=="restore") {
      AuditWindowState(false);co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
      auto normal=app::CaptureWindowState(hwnd);
      ParityRequire(std::abs(normal.x-saved.x)<2 && std::abs(normal.y-saved.y)<2 && std::abs(normal.width-saved.width)<2 && std::abs(normal.height-saved.height)<2);
      co_await SaveDesignWindow(hwnd,designOutput,L"cold-restored.png");co_await ui;
    }
    passed=true;
  }catch(hresult_error const& e) {logf("w8a cold placement failed HRESULT %ld",e.code().value);}
  co_await ui;
  std::ofstream(designOutput/L"w8a.json")<<"{\"passed\":"<<(passed?"true":"false")<<",\"pid\":"<<GetCurrentProcessId()<<",\"phase\":\""<<phase<<"\"}";
  exiting=true;window.Close();Exit();
}
#endif
