// Shared control paths run without activation. Physical cursor checks are opt-in.
void ParityRequire(bool value,std::source_location at=std::source_location::current()) {
  if(!value) { logf("round5 assertion: %s:%u",at.file_name(),at.line()); winrt::throw_hresult(E_FAIL); }
}
static bool AuditRealPointer() { return app::Environment(L"HARMONY_AUDIT_REAL_POINTER")=="1"; }
static void AuditRequireRealPointer(std::source_location at=std::source_location::current()) {
  if(!AuditRealPointer()) {logf("audit real input blocked: %s:%u",at.file_name(),at.line());winrt::throw_hresult(E_ACCESSDENIED);}
}
static UINT AuditSendInput(UINT count,INPUT* input,int size,std::source_location at=std::source_location::current()) {
  AuditRequireRealPointer(at);return SendInput(count,input,size);
}
static BOOL AuditSetCursorPos(int x,int y,std::source_location at=std::source_location::current()) {
  AuditRequireRealPointer(at);return SetCursorPos(x,y);
}
// No documented nonactivating maximize command is available in this SDK.
// Never substitute an activating command in a background audit.
void AuditWindowState(bool maximize) {
  if(maximize) {AuditRequireRealPointer();ShowWindow(hwnd,SW_MAXIMIZE);}
  else ShowWindow(hwnd,SW_SHOWNOACTIVATE);
}
bool ParityCursor(wchar_t const* shape) {
  AuditRequireRealPointer();
  CURSORINFO cursor{sizeof(CURSORINFO)}; bool globalKnown=bool(GetCursorInfo(&cursor));
  POINT position{}; GetCursorPos(&position);
  logf("round5 cursor threads: UI %lu, input window %lu",GetCurrentThreadId(),GetWindowThreadProcessId(WindowFromPoint(position),nullptr));
  auto expected=LoadCursorW(nullptr,shape);
  // WinUI can duplicate the cursor handle. Compare the drawn system cursor,
  // rather than treating handle identity as proof of the visible shape.
  auto pixels=[](HCURSOR handle) {
    constexpr int size=32;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=size; info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* data{}; HDC dc=CreateCompatibleDC(nullptr); auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
    auto previous=SelectObject(dc,bitmap); memset(data,0,size*size*4); DrawIconEx(dc,0,0,handle,size,size,0,nullptr,DI_NORMAL); GdiFlush();
    std::vector<uint32_t> result(static_cast<uint32_t*>(data),static_cast<uint32_t*>(data)+size*size);
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); return result;
  };
  auto actual=pixels(GetCursor()); bool matches=actual==pixels(expected);
  logf("round5 cursor: window thread matches requested shape %d, global matches %d",matches,globalKnown ? int(pixels(cursor.hCursor)==pixels(expected)) : -1);
  logf("round5 global cursor: flags %u, arrow %d, resize %d",cursor.flags,
    globalKnown && pixels(cursor.hCursor)==pixels(LoadCursorW(nullptr,IDC_ARROW)),
    globalKnown && pixels(cursor.hCursor)==pixels(LoadCursorW(nullptr,IDC_SIZEWE)));
  return matches && globalKnown && pixels(cursor.hCursor)==pixels(expected);
}
void ParityMove(FrameworkElement element, double dx=0,std::source_location caller=std::source_location::current()) {
  AuditRequireRealPointer(caller);
  auto at=element.TransformToVisual(nullptr).TransformPoint({float(element.ActualWidth()/2+dx),float(element.ActualHeight()/2)});
  double scale=GetDpiForWindow(hwnd)/96.0; POINT p{LONG(at.X*scale),LONG(at.Y*scale)}; ParityRequire(ClientToScreen(hwnd,&p));
  ParityRequire(GetAncestor(WindowFromPoint(p),GA_ROOT)==hwnd);
  INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
  input.mi.dx=LONG((p.x-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
  input.mi.dy=LONG((p.y-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535.0/(GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
  ParityRequire(AuditSendInput(1,&input,sizeof(INPUT))==1);
  POINT actual{}; ParityRequire(GetCursorPos(&actual));
  logf("round5 pointer: requested %ld,%ld actual %ld,%ld scale %.2f",p.x,p.y,actual.x,actual.y,scale);
  ParityRequire(std::abs(actual.x-p.x)<=1 && std::abs(actual.y-p.y)<=1);
}
void ParityMouse(bool down,std::source_location at=std::source_location::current()) {
  AuditRequireRealPointer(at);
  INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=down?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP;
  ParityRequire(AuditSendInput(1,&input,sizeof(INPUT))==1);
}
winrt::Windows::Foundation::IAsyncAction Parity5Audit() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  ChatAuditPosition();
  bool firstEntryHand=false;
  auto settle=[&]() { return winrt::resume_after(std::chrono::milliseconds(180)); };
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round5"); co_await ui;
  logf("round5 tabs begin"); ParityMove(tabDirect); co_await settle(); co_await ui;
  firstEntryHand=ParityCursor(IDC_HAND);
  // Keep the direct-entry failure visible separately from continuous motion.
  // Include motion inside the surface, rather than only teleporting into it.
  ParityMove(tabDirect,1); co_await settle(); co_await ui;
  logf("round5 tabs background %06x",DesignColor(tabDirect.Background()));
  ParityRequire(DesignColor(tabDirect.Background())==Surface2);
  ParityRequire(ParityCursor(IDC_HAND));
  co_await SaveDesignSurface(root,chatOutput,"round5-tabs-hover");
  TextBlock author{nullptr}; FrameworkElement hovered{nullptr};
  for(uint32_t i=0;i<messageItems.Size();++i) if(auto item=messageList.ContainerFromIndex(i).try_as<FrameworkElement>()) {
    if(auto name=FindDesignPart(item,L"MessageAuthor").try_as<TextBlock>()) {
      auto p=name.TransformToVisual(root).TransformPoint({0,0});
      if(p.Y>200 && p.Y<600) { author=name; hovered=item; break; }
    }
  }
  ParityRequire(bool(author)); ParityMove(author); co_await settle(); co_await ui;
  ParityRequire(author.TextDecorations()==winrt::Windows::UI::Text::TextDecorations::Underline);
  ParityRequire(ParityCursor(IDC_HAND));
  co_await SaveDesignSurface(root,chatOutput,"round5-author-hover");
  ParityMove(headerTitle); co_await settle(); co_await ui;
  ParityRequire(ParityCursor(IDC_ARROW));
  ParityMove(composer); co_await settle(); co_await ui;
  ParityRequire(ParityCursor(IDC_IBEAM));
  ParityMove(hovered); co_await settle(); co_await ui;
  auto band=FindDesignPart(hovered,L"MessageHoverBand").as<Border>();
  auto top=band.TransformToVisual(hovered).TransformPoint({0,0}).Y;
  ParityRequire(top>=0 && top+band.ActualHeight()<=hovered.ActualHeight()+.1);
  co_await SaveDesignSurface(root,chatOutput,"round5-message-hover");
  } else logf("round5 cursor shapes and physical hover: skipped (HARMONY_AUDIT_REAL_POINTER)");
  TabHover(tabDirect,"dm",true);ParityRequire(DesignColor(tabDirect.Background())==Surface2);
  co_await SaveDesignSurface(root,chatOutput,"round5-tabs-hover-effect");TabHover(tabDirect,"dm",false);
  bool visualRow=false;
  for(uint32_t i=0;i<messageItems.Size();++i) if(auto row=messageList.ContainerFromIndex(i).try_as<FrameworkElement>()) {
    auto author=FindDesignPart(row,L"MessageAuthor").try_as<TextBlock>();
    auto band=FindDesignPart(row,L"MessageHoverBand").try_as<Border>();
    auto actions=FindDesignPart(row,L"Actions").try_as<Border>();
    if(!author || !band || !actions)continue;
    AuthorHover(author,true);ParityRequire(author.TextDecorations()==winrt::Windows::UI::Text::TextDecorations::Underline);
    MessageHoverVisual(band,actions,false,true);ParityRequire(actions.Opacity()==1 && actions.IsHitTestVisible());
    auto top=band.TransformToVisual(row).TransformPoint({0,0}).Y;
    ParityRequire(top>=0 && top+band.ActualHeight()<=row.ActualHeight()+.1);
    co_await SaveDesignSurface(root,chatOutput,"round5-message-hover-effect");
    AuthorHover(author,false);MessageHoverVisual(band,actions,false,false);visualRow=true;break;
  }
  ParityRequire(visualRow);
  auto drag=[&](double delta) -> winrt::Windows::Foundation::IAsyncAction {
    ChannelResizeBegin(); ChannelResizeTo(delta); ChannelResizeEnd();
    co_await settle(); co_await ui;
  };
  co_await drag(128); co_await ui; ParityRequire(channelWidth==384);
  co_await SaveDesignSurface(root,chatOutput,"round5-column-drag");
  co_await drag(300); co_await ui; ParityRequire(channelWidth==448);
  co_await SaveDesignSurface(root,chatOutput,"round5-column-max");
  co_await drag(-300); co_await ui; ParityRequire(channelWidth==208);
  co_await SaveDesignSurface(root,chatOutput,"round5-column-min");
  SetChannelWidth(256); root.UpdateLayout();
  OpenSearch(); co_await settle(); co_await ui;
  auto title=FindDesignPart(searchDialog,L"Title");
  auto content=FindDesignPart(searchDialog,L"Content");
  ParityRequire(std::abs(title.TransformToVisual(root).TransformPoint({0,0}).X-content.TransformToVisual(root).TransformPoint({0,0}).X)<.1);
  co_await SaveDesignSurface(searchDialog,chatOutput,"round5-search-empty");
  auto searchCard=searchDialog.Children().GetAt(0).as<FrameworkElement>();
  ParityRequire(!SearchPressOutside(searchCard,{4,4}) && searchOpen);
  ParityRequire(SearchPressOutside(searchCard,{-4,-4})); co_await settle(); co_await ui;
  for(int i=0;i<10 && searchOpen;++i) { co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui; }
  ParityRequire(!searchOpen); co_await SaveDesignSurface(root,chatOutput,"round5-search-dismissed");
  EnsureDirectLayer(); app::CallView incoming; incoming.phase=app::CallPhase::Ringing;
  incoming.peer.username="bruna"; incoming.peer.displayName="bruna";
  DrawIncoming(incoming); co_await settle(); co_await ui;
  auto card=FindDesignPart(directLayer,L"IncomingCall").as<Border>();
  ParityRequire(DesignColor(card.Background())==Surface2);
  co_await SaveDesignSurface(host,chatOutput,"round5-incoming");
  VisualStateManager::GoToState(FindDesignPart(directLayer,L"DirectReject").as<Button>(),L"PointerOver",false); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,chatOutput,"round5-reject-hover");
  directLayer.Children().Clear(); directSignature.clear();
  co_await AuditSystemFilePicker();co_await ui;
  Border controls; controls.Background(Brush(Surface)); controls.Padding(Thickness{16}); controls.Width(400);
  controls.HorizontalAlignment(HorizontalAlignment::Center); controls.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumnSpan(controls,3); Canvas::SetZIndex(controls,100);
  StackPanel samples; samples.Spacing(12);
  ComboBox choices; choices.HorizontalAlignment(HorizontalAlignment::Stretch); HarmonyPointer(choices);
  choices.Items().Append(box_value(L"Primeira opção")); choices.Items().Append(box_value(L"Segunda opção")); choices.SelectedIndex(0); samples.Children().Append(choices);
  CheckBox check; check.Content(box_value(L"Opção do harmony")); HarmonyPointer(check); check.IsChecked(true); samples.Children().Append(check);
  ToggleSwitch toggle; toggle.Header(box_value(L"Avisos")); toggle.IsOn(true); HarmonyPointer(toggle); samples.Children().Append(toggle);
  Slider slider; slider.Minimum(0); slider.Maximum(100); slider.Value(60); HarmonyPointer(slider); samples.Children().Append(slider);
  controls.Child(samples); root.Children().Append(controls); co_await settle(); co_await ui;
  choices.IsDropDownOpen(true); co_await settle(); co_await ui;
  for(auto const& popup:Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
    if(auto content=popup.Child().try_as<FrameworkElement>()) co_await SaveDesignSurface(content,chatOutput,"round5-dropdown");
  choices.IsDropDownOpen(false); co_await settle(); co_await ui;
  co_await SaveDesignSurface(controls,chatOutput,"round5-controls"); root.Children().RemoveAtEnd();
  std::ofstream(chatOutput/L"round5-result.json") << "{\"passed\":" << (!AuditRealPointer() || firstEntryHand ? "true" : "false") << ",\"first_entry_hand\":" << (firstEntryHand ? "true" : "false") << ",\"physical_cursor_skipped\":" << (AuditRealPointer()?"false":"true") << ",\"drag_min\":208,\"drag_max\":448,\"search_outside\":true}";
}
