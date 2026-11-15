// The W6 round against a local harmony (call-audit.mjs): this program signs
// in as one person and goes to the tray; the web, as another, writes to it
// and calls it. Each step prints "call-audit <step>" on the log, which the
// script waits for before it acts, and the result goes to resultado.json.
//
// What it cannot do is press the Windows notice itself: the click is the
// notice's own arguments handed to NoticeAction, the path a real click takes.
// Included inside App, after call-ui.h.

std::vector<app::Notice> auditNotices;   // every notice shown, in order (W6 round only)

winrt::Windows::Foundation::IAsyncOperation<bool> Until(std::function<bool()> ready, int ms) {
  auto lifetime = get_strong(); winrt::apartment_context ui;
  for (int waited = 0; waited < ms; waited += 100) {
    if (ready()) co_return true;
    co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
  }
  co_return ready();
}

std::filesystem::path AuditShortcut() {
  PWSTR programs{}; check_hresult(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs));
  auto link = std::filesystem::path(programs) / L"Harmony Native Windows Audit.lnk"; CoTaskMemFree(programs);
  return link;
}

void AuditHide() {
  ShowWindow(hwnd, SW_HIDE); RefreshMediaVisibility(false); ReportPresence();
  service->WindowSeen(false);
}

winrt::Windows::Foundation::IAsyncAction CallAudit() {
  auto lifetime = get_strong(); winrt::apartment_context ui;
  std::string stage = "local-only"; bool passed = false;
  json::Value report = json::Value::Object(); report["notices"] = json::Value::Array();
  auto step = [](char const* name) { logf("call-audit %s", name); };
  // Photos of the window as it is (PrintWindow: the pictures in the panels
  // too), for looking at; a photo that fails fails nothing.
  auto shoot = [this](wchar_t const* name) -> winrt::Windows::Foundation::IAsyncAction {
    auto lifetime = get_strong();
    try { co_await SaveDesignWindow(hwnd, callOutput, std::wstring(L"chamada-") + name + L".png"); } catch (...) { logf("call-audit photo failed"); }
  };
  auto conversationNotices = [this] {
    std::vector<app::Notice> out;
    for (auto const& n : auditNotices) if (n.target == app::Notice::Target::Conversation) out.push_back(n);
    return out;
  };
  try {
    auto base = app::Environment(L"HARMONY_URL");
    check_bool(base.rfind("http://127.0.0.1:", 0) == 0 || base.rfind("http://localhost:", 0) == 0);
    // Windows shows a toast of a program without a package only under a
    // Start menu shortcut with its identity: the round's own, removed at the end.
    stage = "shortcut";
    check_bool(app::CreateShortcut(AuditShortcut(), app::Executable(), NoticeIdentity(), app::ToastAuditActivator));
    stage = "login";
    service->Restore();
    co_await Until([this] { return !ReadState().busy; }, 10000);
    service->Login(app::Environment(L"HARMONY_TEST_USER"), app::Environment(L"HARMONY_TEST_PASSWORD"));
    // The fixture's people have no confirmed e-mail and the test server does
    // not require one: "Agora não", as a person would (D10 came after W6).
    check_bool(co_await Until([this] { auto s = ReadState(); return (s.signedIn && !s.needsEmail) || (s.needsEmail && !s.emailRequired); }, 30000));
    if (ReadState().needsEmail) service->SkipEmail();
    check_bool(co_await Until([this] { return ReadState().signedIn && !chatActive.empty(); }, 30000));
    stage="cable-output";
    service->VoiceDevices();
    check_bool(co_await Until([this] { for(auto const& d:ReadState().voice.outputs) if(d.name.find("CABLE Input")!=std::string::npos) return true; return false; }, 5000));
    std::string cable;
    for(auto const& d:ReadState().voice.outputs) if(d.name.find("CABLE Input")!=std::string::npos) {cable=d.id;break;}
    check_bool(!cable.empty());
    service->VoiceDeviceChoice(false,cable);
    check_bool(co_await Until([this,cable] {return ReadState().voice.output==cable;},5000));
    report["modern_notices"] = modernNotices;
    // Which Windows notice carries them: the round counts only notices
    // Windows accepted (auditNotices fills after the history has them, or
    // after Shell_NotifyIcon). The toast is what the owner gets: demand it.
    report["notice_path"] = modernNotices ? "toast" : trayAdded ? "tray balloon" : "none";
    stage = "toasts";
    check_bool(modernNotices);
    std::string geral = chatActive, dm, bruna = app::Environment(L"HARMONY_TEST_PEER");
    check_bool(co_await Until([&] {
      service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.kind == "dm" && c.peer.username == bruna) dm = c.id; });
      return !dm.empty();
    }, 15000));
    step("ready");

    // 1. In the tray: a mention and a DM arrive, the silenced channel and
    // the switched-off notices say nothing, and no notice carries the text.
    stage = "notices";
    AuditHide(); step("hidden");
    check_bool(co_await Until([&] { return conversationNotices().size() >= 2; }, 20000));
    co_await winrt::resume_after(std::chrono::seconds(4)); co_await ui;  // the silenced channel's mention, if it wrongly came
    auto shown = conversationNotices();
    for (auto const& n : shown) {
      json::Value one = json::Value::Object(); one["id"] = n.id; one["title"] = n.title; one["body"] = n.body; one["tag"] = n.tag;
      report["notices"].Push(one);
      check_bool(n.title.find("SEGREDO") == std::string::npos && n.body.find("SEGREDO") == std::string::npos);
    }
    stage = "notices-count";
    check_bool(shown.size() == 2);
    stage = "notices-targets";
    check_bool((shown[0].id == geral && shown[1].id == dm) || (shown[0].id == dm && shown[1].id == geral));
    stage = "notices-off";
    service->SetNoticesOn(false); step("notices-off");
    co_await winrt::resume_after(std::chrono::seconds(5)); co_await ui;
    check_bool(conversationNotices().size() == 2);
    service->SetNoticesOn(true);

    // 2. A click on each opens its conversation, in its tab.
    stage = "click-mention";
    auto mention = shown[0].id == geral ? shown[0] : shown[1], direct = shown[0].id == dm ? shown[0] : shown[1];
    NoticeAction({{"target", "conversation"}, {"id", mention.id}});
    check_bool(co_await Until([&] { return IsWindowVisible(hwnd) && chatActive == geral && chatTab == "server"; }, 5000));
    stage = "click-dm";
    AuditHide();
    NoticeAction({{"target", "conversation"}, {"id", direct.id}});
    check_bool(co_await Until([&] { return IsWindowVisible(hwnd) && chatActive == dm && chatTab == "dm"; }, 5000));
    check_bool(headerCall && headerCall.Visibility() == Visibility::Visible);

    // 3. The web calls; it rings in the tray; "Atender" on the notice.
    stage = "incoming";
    AuditHide(); step("ring-me");
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Ringing; }, 20000));
    auto ringing = service->CallRead();
    stage = "incoming-notice";
    check_bool(co_await Until([&] {
      for (auto const& n : auditNotices) if (n.target == app::Notice::Target::Call && n.id == ringing.id) return true;
      return false;
    }, 5000));
    report["incoming_from"] = ringing.peer.username;
    check_bool(ringing.peer.username == bruna);
    {
      // The notice is in the notification centre, buttons and all.
      auto all = winrt::Windows::UI::Notifications::ToastNotificationManager::History().GetHistory(NoticeIdentity());
      std::string tag; bool ring = false;
      for (auto const& n : auditNotices) if (n.target == app::Notice::Target::Call && n.id == ringing.id) tag = n.tag;
      for (auto const& t : all) if (!tag.empty() && to_string(t.Tag()) == tag) ring = true;
      report["notification_centre"] = double(all.Size());
      check_bool(ring);
    }
    stage = "answer";
    NoticeAction({{"target", "call"}, {"id", ringing.id}, {"action", "accept"}});
    check_bool(co_await Until([this] { auto v = service->CallRead(); return v.phase == app::CallPhase::Live && v.mediaConnected; }, 30000));
    check_bool(IsWindowVisible(hwnd) && FindDesignPart(directLayer, L"DirectCallScreen"));
    step("live-1");
    // The script reads both directions from the web's statistics; meanwhile
    // the speaker changes to another device and back, as from Ajustes, and
    // the call keeps playing.
    stage = "switch-output";
    service->VoiceDevices();
    co_await Until([this] { return ReadState().voice.outputs.size() > 1; }, 5000);
    {
      auto cable = ReadState().voice.output;
      check_bool(!cable.empty());
      report["switched_output"] = true;
      service->VoiceDeviceChoice(false, cable);
      co_await winrt::resume_after(std::chrono::seconds(2)); co_await ui;
      service->VoiceDeviceChoice(false, cable);
    }
    co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
    step("switched");
    co_await shoot(L"viva");
    stage = "call-geometry";
    {
      // The room's own blocks since 25/10/2026 (main.cpp DrawStage): its 64
      // circle and its 24 chip, not the 96 and 28 the 1:1 used to draw.
      auto face = FindDesignPart(directLayer, L"CallAvatar");
      auto chip = FindDesignPart(directLayer, L"CallName");
      check_bool(face && face.Width() == 64 && face.Height() == 64 && chip && chip.Height() == 24);
      auto header = FindDesignPart(directLayer, L"DirectHeader");
      auto at = header.TransformToVisual(directLayer).TransformPoint({0, 0});
      check_bool(at.X == 16 && at.Y == 16 && header.ActualWidth() == directLayer.ActualWidth() - 32);
      co_await SaveDesignSurface(directLayer, callOutput, "nativo-chamada-superficie");
      directSmall = true; directSignature.clear(); Render();
      co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
      co_await shoot(L"minimizada");
      directSmall = false; directSignature.clear(); Render();
    }
    // "Surdo": the web's voice stops coming out here, and the web sees this
    // side's microphone off; hearing again gives both back.
    stage = "deafen";
    service->CallDeafen(true);
    check_bool(co_await Until([this] { auto v = service->CallRead(); return v.deafened && v.muted; }, 5000));
    step("deafened");
    co_await winrt::resume_after(std::chrono::seconds(3)); co_await ui;
    co_await shoot(L"surdo");
    service->CallDeafen(false);
    check_bool(co_await Until([this] { auto v = service->CallRead(); return !v.deafened && !v.muted; }, 5000));
    step("hearing");
    co_await winrt::resume_after(std::chrono::seconds(3)); co_await ui;
    // This side answered: its answer picks the codecs. Camera and screen go
    // out here too, where a camera in H.264 would send nothing.
    stage = "answered-camera";
    service->CallCamera(true);
    check_bool(co_await Until([this] { return service->CallRead().cam; }, 5000));
    step("camera-on-1");
    co_await winrt::resume_after(std::chrono::seconds(4)); co_await ui;
    co_await shoot(L"minha-camera");
    co_await winrt::resume_after(std::chrono::seconds(4)); co_await ui;
    service->CallCamera(false);
    check_bool(co_await Until([this] { return !service->CallRead().cam; }, 5000));
    stage = "answered-screen";
    service->CallScreen(service->CallRead().id, nullptr, MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), 480, 15, false);
    check_bool(co_await Until([this] { return service->CallRead().screen; }, 5000));
    step("sharing-1");
    co_await winrt::resume_after(std::chrono::seconds(4)); co_await ui;
    // The room's stage in the 1:1 (25/10/2026): the screen that started is
    // big, the strip folds with her camera floating over it, and full screen
    // puts the controls over the picture until Esc or the ✕.
    stage = "stage-folded";
    check_bool(directStage.focus.rfind("s:", 0) == 0 && FindDesignPart(directLayer, L"CallStripToggle"));
    directStage.strip = false; RedrawStage(directStage);
    co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
    check_bool(bool(FindDesignPart(directLayer, L"FloatingCamera")) == service->CallRead().peerCam);
    co_await shoot(L"faixa-escondida");
    stage = "stage-full";
    EnterCallFull(directStage, directStage.focus);
    co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
    check_bool(fullStage == &directStage && FindDesignPart(directLayer, L"FullscreenControls") && !FindDesignPart(directLayer, L"DirectHeader"));
    co_await shoot(L"tela-cheia");
    LeaveCallFull(); directStage.strip = true; RedrawStage(directStage);
    co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
    check_bool(!fullStage && FindDesignPart(directLayer, L"DirectHeader") && FindDesignPart(directLayer, L"CallStripToggle"));
    co_await winrt::resume_after(std::chrono::seconds(2)); co_await ui;
    service->CallStopScreen();
    check_bool(co_await Until([this] { return !service->CallRead().screen; }, 5000));
    step("stopped-1");
    stage = "mute";
    co_await winrt::resume_after(std::chrono::seconds(3)); co_await ui;
    service->CallMute(true);
    check_bool(co_await Until([this] { return service->CallRead().muted; }, 5000));
    step("muted");
    stage = "remote-hangup";
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Idle; }, 30000));
    step("ended-1");

    // 4. This program calls the web; it answers; this side hangs up.
    stage = "outgoing";
    co_await winrt::resume_after(std::chrono::seconds(2)); co_await ui;
    service->CallRing(app::CallPerson{bruna, bruna});
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Calling; }, 10000));
    step("calling");
    check_bool(co_await Until([this] { auto v = service->CallRead(); return v.phase == app::CallPhase::Live && v.mediaConnected; }, 30000));
    step("live-2");
    // The web turns its camera on: it shows big, in the call's own tile.
    stage = "peer-camera";
    check_bool(co_await Until([this] { return service->CallRead().peerCam; }, 30000));
    check_bool(co_await Until([this] { return bool(FindDesignPart(directLayer, L"DirectCameraVideo")); }, 5000));
    // Drawn, not only received: the tile's own renderer presents pictures.
    stage = "peer-camera-drawn";
    check_bool(co_await Until([this] { auto r = directRenderer.lock(); return r && r->presented > 10; }, 8000));
    { auto r = directRenderer.lock(); report["camera_presented"] = double(r ? r->presented.load() : 0); }
    step("seen-camera");
    co_await shoot(L"camera-dele");
    stage="pip-rounded-thumbnail";
    directStage.focus="p:"+ReadState().voice.self;
    // Focus the local participant so the received camera is in the strip.
    service->ChatModel().Inspect([&](app::ChatState const& state){directStage.focus="p:"+state.owner;});
    directSignature.clear();DrawDirectCall();
    co_await winrt::resume_after(std::chrono::seconds(1));co_await ui;
    auto camera=FindDesignPart(directLayer,L"DirectCameraVideo");check_bool(bool(camera));
    DirectTalking(false);
    // Find the actual ring again: stopping the meter releases its reference.
    directSignature.clear();DrawDirectCall();directTalk.Stop();
    directRingTheirs.Visibility(Visibility::Visible);
    camera=FindDesignPart(directLayer,L"DirectCameraVideo");
    auto visual=winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(camera);
    auto clip=visual.Clip();visual.Clip(nullptr);camera.Margin(Thickness{0,0,0,0});
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignWindow(hwnd,callOutput,L"miniatura-antes.png");
    camera.Margin(Thickness{2,2,2,2});visual.Clip(clip);
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignWindow(hwnd,callOutput,L"miniatura-depois.png");
    auto point=camera.TransformToVisual(host).TransformPoint({0,0});POINT client{};ClientToScreen(hwnd,&client);RECT wr{};GetWindowRect(hwnd,&wr);
    double dpi=camera.XamlRoot().RasterizationScale();
    report["corner_x"]=double(client.x-wr.left)+point.X*dpi;report["corner_y"]=double(client.y-wr.top)+point.Y*dpi;
    DirectTalking(true);
    auto cpu=[] {FILETIME created{},exit{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user);
      ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return double(k.QuadPart+u.QuadPart);};
    auto startCpu=cpu();auto startWall=GetTickCount64();
    co_await winrt::resume_after(std::chrono::seconds(5));co_await ui;
    report["cpu_main_one_core_percent"]=(cpu()-startCpu)/double(GetTickCount64()-startWall)/100.;
    stage="pip-minimize";
    auto mainRenderer=directRenderer.lock();check_bool(bool(mainRenderer));
    ShowWindow(hwnd,SW_MINIMIZE);
    // Minimised, nothing floats by itself (the owner, 02/11/2026): only its
    // button opens it, pressed here as a person would, from the call's header.
    co_await winrt::resume_after(std::chrono::milliseconds(1500));co_await ui;
    check_bool(!pipActive);report["pip_not_automatic"]=true;
    {auto open=FindDesignPart(directLayer,L"PipOpen").try_as<Button>();check_bool(bool(open));
     winrt::Microsoft::UI::Xaml::Automation::Peers::ButtonAutomationPeer(open).Invoke();}
    check_bool(co_await Until([this]{return pipActive && pipRenderer && pipRenderer->presented>10;},8000));
    co_await SaveDesignWindow(pipHwnd,callOutput,L"flutuante-camera.png");
    RECT pipBounds{};GetWindowRect(pipHwnd,&pipBounds);
    report["pip_width_px"]=double(pipBounds.right-pipBounds.left);report["pip_height_px"]=double(pipBounds.bottom-pipBounds.top);
    report["pip_monitor_left"]=double(pipBounds.left);
    auto mainCount=mainRenderer->presented.load();auto pipCount=pipRenderer->presented.load();
    startCpu=cpu();startWall=GetTickCount64();
    co_await winrt::resume_after(std::chrono::seconds(5));co_await ui;
    report["cpu_pip_one_core_percent"]=(cpu()-startCpu)/double(GetTickCount64()-startWall)/100.;
    report["hidden_main_presents"]=double(mainRenderer->presented.load()-mainCount);
    report["pip_presents"]=double(pipRenderer->presented.load()-pipCount);
    check_bool(mainRenderer->presented==mainCount && pipRenderer->presented>pipCount);
    logf("pip audit hidden main presents=%d floating presents=%d",mainRenderer->presented.load()-mainCount,pipRenderer->presented.load()-pipCount);
    // Its buttons wait for the pointer (the owner's PC: his mouse is
    // somewhere else, so they are hidden now).
    stage="pip-buttons-hidden";
    check_bool(pipHover.size()==2 && pipHover[0].Opacity()==0 && pipHover[1].Opacity()==0);
    for(auto& part:pipHover) part.Opacity(1);
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignWindow(pipHwnd,callOutput,L"flutuante-com-mouse.png");
    for(auto& part:pipHover) part.Opacity(0);
    // Bigger, the picture is drawn at the new size, not stretched.
    stage="pip-resize";
    {int scale=GetDpiForWindow(pipHwnd),w=MulDiv(560,scale,96),h=w*9/16;
     PlacePip(pipBounds.right-w,pipBounds.bottom-h,w,h);
     auto panel=FindDesignPart(pipRoot,L"PipPicture");
     bool grew=co_await Until([panel,w]{return panel && panel.ActualWidth()*panel.XamlRoot().RasterizationScale()>w-8;},4000);
     {RECT r{};GetWindowRect(pipHwnd,&r);logf("pip audit resize window=%dx%d panel=%.0f",int(r.right-r.left),int(r.bottom-r.top),panel?panel.ActualWidth():-1.0);}
     check_bool(grew);
     auto before=pipRenderer->presented.load();bool drew=co_await Until([this,before]{return pipRenderer->presented>before+5;},4000);
     logf("pip audit resize presents=%d error=%s",pipRenderer->presented.load()-before,pipRenderer->LastError().c_str());check_bool(drew);
     co_await SaveDesignWindow(pipHwnd,callOutput,L"flutuante-maior.png");
     report["pip_big_width_px"]=double(w);
     PlacePip(pipBounds.left,pipBounds.top,pipBounds.right-pipBounds.left,pipBounds.bottom-pipBounds.top);}
    // Its x takes it away for the rest of this call, the app still minimised.
    stage="pip-close";
    {auto close=FindDesignPart(pipRoot,L"PipClose").as<Button>();check_bool(bool(close));
     winrt::Microsoft::UI::Xaml::Automation::Peers::ButtonAutomationPeer(close).Invoke();}
    check_bool(co_await Until([this]{return !pipActive && !IsWindowVisible(pipHwnd);},3000));
    co_await winrt::resume_after(std::chrono::milliseconds(1200));co_await ui;
    check_bool(!pipActive && IsIconic(hwnd));
    // Closed, it opens again from the strip's button (the owner, 02/11/2026).
    stage="pip-reopen";
    {auto open=FindDesignPart(directLayer,L"PipOpen").try_as<Button>();check_bool(bool(open));
     winrt::Microsoft::UI::Xaml::Automation::Peers::ButtonAutomationPeer(open).Invoke();}
    check_bool(co_await Until([this]{return pipActive && pipRenderer && pipRenderer->presented>3;},8000));report["pip_reopened"]=true;
    // Off in Ajustes, it never opens.
    stage="pip-off";
    SetPipOn(false);co_await winrt::resume_after(std::chrono::milliseconds(1200));co_await ui;check_bool(!pipActive);
    SetPipOn(true);
    check_bool(co_await Until([this]{return pipActive && pipRenderer && pipRenderer->presented>3;},8000));
    stage="pip-click-return";
    // The click "por dentro", as the notices' NoticeAction: the owner uses
    // this PC while the round runs, so it never moves his real mouse.
    ReturnFromPip();
    check_bool(co_await Until([this]{return !pipActive && !IsIconic(hwnd) && !IsWindowVisible(pipHwnd);},5000));
    co_await SaveDesignWindow(hwnd,callOutput,L"chamada-restaurada.png");
    stage="pip-camera-ended";
    directSmall=true;pipWanted=true;directSignature.clear();DrawDirectCall();
    check_bool(co_await Until([this]{return pipActive;},5000));step("pip-stop-camera");
    check_bool(co_await Until([this]{return !service->CallRead().peerCam && pipActive && pipSlot==0;},10000));
    co_await SaveDesignWindow(pipHwnd,callOutput,L"flutuante-imagem-acabou.png");
    check_bool(co_await Until([this]{return !pipActive && !IsWindowVisible(pipHwnd);},6000));
    report["pip_end_notice_expired"]=true;ReturnFromPip();step("pip-tested");

    // This side's camera goes out too (VP8/VP9: the GPU's H.264 takes no
    // camera frames). On a PC without one, the model takes it back off.
    stage = "own-camera";
    service->CallCamera(true);
    check_bool(co_await Until([this] { return service->CallRead().cam; }, 5000));
    step("camera-on");
    co_await winrt::resume_after(std::chrono::seconds(12)); co_await ui;
    report["own_camera"] = service->CallRead().cam;
    service->CallCamera(false);
    check_bool(co_await Until([this] { return !service->CallRead().cam; }, 5000));
    step("camera-off");
    // This side shares the main screen, small; the web must receive it.
    stage = "share-screen";
    // A picker left from another call shares nothing (the id).
    service->CallScreen("not-this-call", nullptr, MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), 480, 15, false);
    co_await winrt::resume_after(std::chrono::seconds(1)); co_await ui;
    check_bool(!service->CallRead().screen);
    service->CallScreen(service->CallRead().id, nullptr, MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), 480, 15, false);
    check_bool(co_await Until([this] { return service->CallRead().screen; }, 5000));
    step("sharing");
    co_await winrt::resume_after(std::chrono::seconds(2)); co_await ui;
    co_await shoot(L"minha-tela");
    co_await winrt::resume_after(std::chrono::seconds(8)); co_await ui;
    // "Parar" on the floating notice, the way the owner stops it.
    stage = "stop-from-notice";
    // The floating "Parar" notice is gone (TransmissionNotice only removes
    // it since then): stop from the bar's button path instead.
    service->CallStopScreen();
    check_bool(co_await Until([this] { return !service->CallRead().screen; }, 5000));
    step("stopped-sharing");
    co_await winrt::resume_after(std::chrono::seconds(3)); co_await ui;
    service->CallHangup();
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Idle; }, 10000));
    step("ended-2");

    // 5. The web calls again; "Recusar" on the notice, window still away.
    stage = "decline";
    AuditHide(); step("ring-me-again");
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Ringing; }, 20000));
    auto again = service->CallRead();
    NoticeAction({{"target", "call"}, {"id", again.id}, {"action", "decline"}});
    check_bool(co_await Until([this] { return service->CallRead().phase == app::CallPhase::Idle; }, 10000));
    check_bool(!IsWindowVisible(hwnd));
    step("declined");
    stage = "done";
    passed = true;
  } catch (...) { logf("call-audit failed at %s", stage.c_str()); }
  co_await winrt::resume_after(std::chrono::seconds(2)); co_await ui;
  report["passed"] = passed; report["stage"] = stage;
  {
    std::ofstream out(callOutput / L"resultado.json", std::ios::binary); out << report.Dump();
  }
  logf("call-audit result %s", passed ? "passed" : "FAILED");
  {
    // The round's own identity (Harmony.Native.Windows.Audit) leaves nothing
    // behind: no notice, no COM class, no shortcut.
    UnregisterNotices();
    std::error_code gone; std::filesystem::remove(AuditShortcut(), gone);
    bool cleaned = app::ToastsUnregister(NoticeIdentity()) && !std::filesystem::exists(AuditShortcut());
    logf("call-audit cleanup %s", cleaned ? "ok" : "FAILED");
    if (!cleaned) { report["passed"] = false; report["stage"] = "cleanup"; std::ofstream out(callOutput / L"resultado.json", std::ios::binary); out << report.Dump(); }
  }
  Quit();
}
