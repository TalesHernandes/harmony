// Isolated layout fixtures; no account data or capture is sent to a server.
winrt::Windows::Foundation::IAsyncAction Parity7cDesign() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction { co_await winrt::resume_after(std::chrono::milliseconds(350)); co_await ui; root.UpdateLayout(); };
  designState.signedIn=true; designState.busy=false; designState.model.live=true;
  designState.model.rooms={{"voz-geral","geral",{{"design-me","Observador"}}}};
  auto& peer=designState.model.rooms[0].people[0]; peer.muted=true; peer.screen=true; peer.camera=true;
  designState.voice.self="design-me"; designState.voice.Join("voz-geral","geral"); designState.voice.connecting=false;
  showingCall=true; Render(); co_await settle(); co_await ui;
  auto room=FindDesignPart(root,L"VoiceRoomRow").as<Button>();
  auto person=FindDesignPart(root,L"VoiceSelf").as<Button>();
  ParityRequire(room.ActualWidth()>180 && std::abs(room.ActualWidth()-person.ActualWidth())<1);
  auto textRow=ChannelRow(L"geral", "hashtag", true,0,0,false,[]{}).as<FrameworkElement>();
  channelList.Children().Append(textRow); root.UpdateLayout();
  ParityRequire(textRow.ActualWidth()==room.ActualWidth());
  VisualStateManager::GoToState(person,L"PointerOver",false);
  auto marks=FindDesignPart(person,L"VoiceStatusMarks"); auto name=voiceNames.at("design-me");
  auto center=[](FrameworkElement e,FrameworkElement relative) {return e.TransformToVisual(relative).TransformPoint({0,float(e.ActualHeight()/2)}).Y;};
  ParityRequire(std::abs(center(marks,person)-center(name,person))<1);
  auto island=FindDesignPart(root,L"VoiceShareScreen").as<Button>(); auto call=FindDesignPart(root,L"ShareScreen").as<Button>();
  ParityRequire(island.ActualHeight()==32 && call.ActualHeight()==44);
  co_await SaveDesignSurface(host,designOutput,"round7c-voice-idle");
  designState.voice.sharingScreen=true; Render(); co_await settle(); co_await ui;
  co_await SaveDesignSurface(host,designOutput,"round7c-voice-sharing");
  OpenSettings("conta"); co_await settle(); co_await ui;
  auto pick=FindDesignPart(settingsOverlay,L"ChangePhoto"); auto remove=FindDesignPart(settingsOverlay,L"RemovePhoto");
  ParityRequire(pick.ActualWidth()==remove.ActualWidth() && pick.ActualHeight()==44 && remove.ActualHeight()==44);
  ParityRequire(FindDesignPart(settingsOverlay,L"PhotoStatus").Visibility()==Visibility::Collapsed);
  co_await SaveDesignSurface(host,designOutput,"round7c-account");
  auto others=FindDesignPart(settingsOverlay,L"RevokeOtherSessions"); auto logout=FindDesignPart(settingsOverlay,L"AskLogout");
  ParityRequire(others.ActualHeight()==44 && logout.ActualHeight()==44);
  DependencyObject parent=others;
  while(parent && !parent.try_as<ScrollViewer>()) parent=Media::VisualTreeHelper::GetParent(parent);
  if(auto list=parent.try_as<ScrollViewer>()) { list.ChangeView(nullptr,list.ScrollableHeight(),nullptr,true); co_await settle(); co_await ui; }
  co_await SaveDesignSurface(host,designOutput,"round7c-account-actions"); CloseSettings();
  island=FindDesignPart(root,L"VoiceShareScreen").as<Button>();
  Automation::Peers::ButtonAutomationPeer(island).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
  for(int i=0;i<80 && screenPickerPending;++i) { co_await settle(); co_await ui; }
  ParityRequire(!screenPickerPending && std::filesystem::exists(designOutput/L"round7c-screen-picker.png"));
  std::ofstream out(designOutput/L"round7c-measures.json");
  out << "{\"voice_row_width\":" << room.ActualWidth() << ",\"person_width\":" << person.ActualWidth()
      << ",\"photo_width\":" << pick.ActualWidth() << ",\"action_height\":44,\"compact_height\":32,\"checkbox_gap\":8}";
}
