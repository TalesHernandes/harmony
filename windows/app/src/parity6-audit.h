// Control effects run without activation; physical delivery and latency are opt-in.
winrt::Windows::Foundation::IAsyncAction Parity6Audit() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  ChatAuditPosition();
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction { co_await winrt::resume_after(std::chrono::milliseconds(150)); co_await ui; };
  std::vector<FrameworkElement> rows;
  for(uint32_t i=0;i<messageItems.Size();++i) {
    auto row=messageList.ContainerFromIndex(i).try_as<FrameworkElement>(); if(!row) continue;
    auto band=FindDesignPart(row,L"MessageHoverBand"); if(!band) continue;
    auto at=band.TransformToVisual(root).TransformPoint({0,0});
    if(at.Y>120 && at.Y+band.ActualHeight()<root.ActualHeight()-90) rows.push_back(row);
  }
  ParityRequire(rows.size()>=3);
  double worst=0,total=0;
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round6 messages"); co_await ui;
  for(auto const& row:rows) {
    ParityMove(headerTitle); co_await settle(); co_await ui;
    ParityRequire(GetForegroundWindow()==hwnd);
    auto bar=FindDesignPart(row,L"Actions");
    auto start=std::chrono::steady_clock::now();
    auto heard=std::make_shared<double>(0);
    auto token=bar.RegisterPropertyChangedCallback(UIElement::OpacityProperty(),[bar,start,heard](auto const&,auto const&) {
      if(bar.Opacity()==1) *heard=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    });
    ParityMove(row);
    for(int i=0;i<25 && bar.Opacity()<1;++i) { co_await winrt::resume_after(std::chrono::milliseconds(1)); co_await ui; }
    bar.UnregisterPropertyChangedCallback(UIElement::OpacityProperty(),token);
    auto ms=*heard;
    ParityRequire(ms>0);
    ParityRequire(bar.Opacity()==1); worst=std::max(worst,ms); total+=ms;
  }
  co_await SaveDesignSurface(host,chatOutput,"round6-message-hover");
  logf("round6 message hover: %zu/%zu rows, average %.2f ms, worst %.2f ms",rows.size(),rows.size(),total/rows.size(),worst);
  ParityRequire(worst<16);
  for(auto const& row:rows) {
    ParityMove(row); co_await winrt::resume_after(std::chrono::milliseconds(16)); co_await ui;
    ParityRequire(FindDesignPart(row,L"Actions").Opacity()==1);
  }
  } else logf("round6 message hover latency worst<16ms: skipped (HARMONY_AUDIT_REAL_POINTER)");
  for(auto const& row:rows) {
    auto band=FindDesignPart(row,L"MessageHoverBand").as<Border>();auto actions=FindDesignPart(row,L"Actions").as<Border>();
    MessageHoverVisual(band,actions,false,true);ParityRequire(actions.Opacity()==1 && actions.IsHitTestVisible());
    MessageHoverVisual(band,actions,false,false);ParityRequire(actions.Opacity()==0 && !actions.IsHitTestVisible());
  }
  composer.Focus(FocusState::Programmatic); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,chatOutput,"round6-composer-focus");
  auto inner=FindDesignPart(composer,L"BorderElement").as<Border>();
  ParityRequire(inner.BorderThickness().Left==0);
  auto ring=FindDesignPart(root,L"ComposerIsland").as<Border>().BorderThickness();
  ParityRequire(ring.Left==2 && ring.Top==2 && ring.Right==2 && ring.Bottom==2);
  auto borderColor=FindDesignPart(root,L"ComposerIsland").as<Border>().BorderBrush().as<SolidColorBrush>().Color();
  logf("round6 focus state=%d rgba=%d,%d,%d,%d",int(composer.FocusState()),borderColor.R,borderColor.G,borderColor.B,borderColor.A);
  ParityRequire(borderColor.R==4 && borderColor.G==133 && borderColor.B==247);
  auto account=FindDesignPart(root,L"SettingsIsland"); auto inputIsland=FindDesignPart(root,L"ComposerIsland");
  logf("round6 island heights account=%.2f composer=%.2f",account.ActualHeight(),inputIsland.ActualHeight());
  ParityRequire(std::abs(account.ActualHeight()-inputIsland.ActualHeight())<1);
  std::vector<app::ChatPerson> members; service->ChatModel().Inspect([&](app::ChatState const& state) {members=state.members;});
  for(auto const& person:members) {
    auto name=FindDesignPart(root,L"PersonName-"+net::Wide(person.username));
    auto face=FindDesignPart(root,L"PersonAvatar-"+net::Wide(person.username));
    if(!name || !face) continue;
    auto n=name.TransformToVisual(root).TransformPoint({0,float(name.ActualHeight()/2)});
    auto f=face.TransformToVisual(root).TransformPoint({0,float(face.ActualHeight()/2)});
    logf("round6 person centers name=%.2f avatar=%.2f",n.Y,f.Y);
    ParityRequire(std::abs(n.Y-f.Y)<.5);
    if(person.username=="carlos") {
      // PhotoFace uses the same static silhouette fill in all pointer states.
      co_await settle();co_await ui;
      co_await SaveDesignSurface(host,chatOutput,"round6-people-hover");
      auto photo=face.as<Grid>().Children().GetAt(0).as<Border>();
      auto fill=photo.Child().as<Grid>().Children().GetAt(0).as<Shapes::Path>().Fill().as<SolidColorBrush>().Color();
      ParityRequire(fill.R==0x32 && fill.G==0x32 && fill.B==0x35);
    }
  }
  auto composerAt=composer.TransformToVisual(inputIsland).TransformPoint({float(composer.Padding().Left),0});
  logf("round6 composer horizontal text inset %.2f",composerAt.X);
  ParityRequire(std::abs(composerAt.X-90)<.5);
  OpenEmojiPanel(); co_await settle(); co_await ui;
  auto query=FindDesignPart(picker.Child().as<FrameworkElement>(),L"EmojiSearch").as<TextBox>();
  query.Focus(FocusState::Programmatic); co_await settle(); co_await ui;
  co_await SaveDesignSurface(picker.Child().as<FrameworkElement>(),chatOutput,"round6-emoji-focus");
  ParityRequire(FindDesignPart(query,L"BorderElement").as<Border>().BorderThickness().Left==0);
  auto emojiRing=FindDesignPart(picker.Child(),L"EmojiSearchContainer").as<Border>().BorderThickness();
  ParityRequire(emojiRing.Left==2 && emojiRing.Top==2 && emojiRing.Right==2 && emojiRing.Bottom==2);
  auto emojiColor=FindDesignPart(picker.Child(),L"EmojiSearchContainer").as<Border>().BorderBrush().as<SolidColorBrush>().Color();
  ParityRequire(emojiColor.R==4 && emojiColor.G==133 && emojiColor.B==247);
  ClosePicker();co_await settle();co_await ui;
  ParityRequire(accountSettings.Focus(FocusState::Programmatic));co_await settle();co_await ui;
  ParityRequire(composer.FocusState()==FocusState::Unfocused);
  co_await SaveDesignSurface(host,chatOutput,"round6-composer-unfocused");
  auto neutral=std::make_shared<DesignPixels>();co_await ReadDesignPixels(inputIsland,neutral);co_await ui;
  for(int y:{1,neutral->height-2}) {
    auto at=size_t(y*neutral->width+neutral->width/2)*4;
    logf("round6 neutral surface y=%d bgra=%u,%u,%u,%u",y,neutral->bytes[at],neutral->bytes[at+1],neutral->bytes[at+2],neutral->bytes[at+3]);
    ParityRequire(neutral->bytes[at]==0x1b && neutral->bytes[at+1]==0x18 && neutral->bytes[at+2]==0x18 && neutral->bytes[at+3]==255);
  }
  co_await SaveDesignSurface(host,chatOutput,"round6-composer-unfocused");
  co_await AuditSystemFilePicker();co_await ui;
  co_await AuditSystemFilePicker(true);co_await ui;
  co_await Parity6ProfileAudit();co_await ui;
  EnsureDirectLayer(); app::CallView incoming; incoming.phase=app::CallPhase::Ringing;
  incoming.peer.username="bruna"; incoming.peer.displayName="bruna";
  DrawIncoming(incoming); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,chatOutput,"round6-incoming");
  VisualStateManager::GoToState(FindDesignPart(directLayer,L"DirectReject").as<Button>(),L"PointerOver",false); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,chatOutput,"round6-reject-hover");
  directLayer.Children().Clear(); directSignature.clear();
  CaptionRegions();
  using winrt::Microsoft::UI::Input::NonClientRegionKind;
  ParityRequire(captionInput.GetRegionRects(NonClientRegionKind::Maximize).size()==1);
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round6 caption delivery"); co_await ui;
  ParityMove(captionButtons[2]); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,chatOutput,"round6-caption-close-hover");
  } else logf("round6 real non-client close hover: skipped (HARMONY_AUDIT_REAL_POINTER)");
  // Optional physical delivery: maximize/restore caption button.
  if(AuditRealPointer()) {
  ParityMove(captionButtons[1]); co_await settle(); co_await ui;
  ParityMouse(true); ParityMouse(false); co_await settle(); co_await ui;
  ParityRequire(IsZoomed(hwnd));
  co_await SaveDesignSurface(host,chatOutput,"round6-caption-maximized");
  ParityMove(captionButtons[1]); ParityMouse(true); ParityMouse(false); co_await settle(); co_await ui;
  ParityRequire(!IsZoomed(hwnd));
  } else logf("round6 non-client maximize click: skipped (HARMONY_AUDIT_REAL_POINTER)");
  // Optional physical delivery: double-click on the title.
  if(AuditRealPointer()) {
  auto title=FindDesignPart(host,L"HarmonyTitleBar");
  auto doubleClick=[&]()->winrt::Windows::Foundation::IAsyncAction {
    ParityMove(title); ParityMouse(true); ParityMouse(false);
    co_await winrt::resume_after(std::chrono::milliseconds(60)); co_await ui;
    ParityMouse(true); ParityMouse(false); co_await settle(); co_await ui;
  };
  co_await doubleClick(); co_await ui; ParityRequire(IsZoomed(hwnd));
  co_await SaveDesignSurface(host,chatOutput,"round6-caption-double-click");
  co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
  co_await doubleClick(); co_await ui; ParityRequire(!IsZoomed(hwnd));
  } else logf("round6 title double-click: skipped (HARMONY_AUDIT_REAL_POINTER)");
  // Optional physical delivery: dragging the non-client title region.
  if(AuditRealPointer()) {
  auto title=FindDesignPart(host,L"HarmonyTitleBar");
  RECT before{}; GetWindowRect(hwnd,&before); ParityMove(title);
  POINT pointer{}; GetCursorPos(&pointer);
  INPUT drag[4]{}; for(auto& input:drag) input.type=INPUT_MOUSE;
  drag[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
  for(int i=1;i<=2;++i) {
    drag[i].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    drag[i].mi.dx=LONG((pointer.x+30*i-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
    drag[i].mi.dy=LONG((pointer.y+12*i-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
  }
  drag[3].mi.dwFlags=MOUSEEVENTF_LEFTUP; ParityRequire(AuditSendInput(4,drag,sizeof(INPUT))==4);
  co_await settle(); co_await ui;
  RECT after{}; GetWindowRect(hwnd,&after); ParityRequire(after.left!=before.left || after.top!=before.top);
  co_await SaveDesignSurface(host,chatOutput,"round6-caption-dragged");
  } else logf("round6 title drag: skipped (HARMONY_AUDIT_REAL_POINTER)");
  // Optional physical delivery: Windows snap shortcut.
  if(AuditRealPointer()) {
  auto placement=window.AppWindow().Position(); auto size=window.AppWindow().Size();
  INPUT snap[4]{}; for(auto& input:snap) input.type=INPUT_KEYBOARD;
  snap[0].ki.wVk=VK_LWIN; snap[1].ki.wVk=VK_LEFT; snap[2].ki.wVk=VK_LEFT; snap[2].ki.dwFlags=KEYEVENTF_KEYUP;
  snap[3].ki.wVk=VK_LWIN; snap[3].ki.dwFlags=KEYEVENTF_KEYUP;
  ParityRequire(AuditSendInput(4,snap,sizeof(INPUT))==4);
  co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
  auto snapped=window.AppWindow().Size(); ParityRequire(snapped.Width<size.Width);
  co_await SaveDesignSurface(host,chatOutput,"round6-caption-snapped");
  window.AppWindow().MoveAndResize({placement.X,placement.Y,size.Width,size.Height});
  } else logf("round6 Win+Arrow snap: skipped (HARMONY_AUDIT_REAL_POINTER)");
  // Optional physical delivery: minimize caption button.
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round6 caption restored"); co_await ui;
  ParityMove(captionButtons[0]); ParityMouse(true); ParityMouse(false); co_await settle(); co_await ui;
  ParityRequire(IsIconic(hwnd)); Open(); co_await settle(); co_await ui;
  } else logf("round6 non-client minimize click: skipped (HARMONY_AUDIT_REAL_POINTER)");
  if(AuditRealPointer()) {
    AuditWindowState(true);co_await settle();co_await ui;ParityRequire(IsZoomed(hwnd));
    co_await SaveDesignSurface(host,chatOutput,"round6-caption-maximized-effect");
  } else logf("round6 maximize effect: skipped (nonactivating maximize API not found)");
  AuditWindowState(false);co_await settle();co_await ui;ParityRequire(!IsZoomed(hwnd));
  // Coming back from minimized took the foreground in the measured runs, even with no-activate flags.
  if(AuditRealPointer()) {
    ShowWindow(hwnd,SW_SHOWMINNOACTIVE);co_await settle();co_await ui;ParityRequire(IsIconic(hwnd));
    AuditWindowState(false);co_await settle();co_await ui;ParityRequire(!IsIconic(hwnd));
  } else logf("round6 minimize effect: skipped (restoring took the foreground)");
  std::ofstream result(chatOutput/L"round6-result.json");
  result<<"{\"passed\":true,\"message_hover_skipped\":"<<(AuditRealPointer()?"false":"true")<<",\"message_hover_worst_ms\":"<<(AuditRealPointer()?std::to_string(worst):"null")<<",\"message_hover_rows\":"<<rows.size()<<"}";
}
