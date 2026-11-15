// Production controls, offline fixture data and a separate installer identity.
bool liveAuditBusy=false;
std::string liveAuditPrevious;
winrt::fire_and_forget W8dLiveAudit(std::filesystem::path directory,std::string command) {
  auto lifetime=get_strong();winrt::apartment_context ui;liveAuditBusy=true;
  try {
    if(command=="join") {service->VoiceNoise(true,50);auto rooms=ReadState().model.rooms;if(!rooms.empty())service->JoinVoice(rooms.front().id);}
    else if(command=="leave")service->LeaveVoice();
    else if(command=="camera")service->Camera(true,"__harmony_pipe_perf_test__");
    else if(command=="camera-off")service->Camera(false,"");
    else if(command=="emoji-cache") {
      OpenEmojiPanel();
      co_await winrt::resume_after(std::chrono::milliseconds(500));co_await ui;
      if(pickerList){double end=pickerList.ScrollableHeight();for(double at=0;at<=end+400;at+=400){pickerList.ChangeView(nullptr,std::min(at,end),nullptr,true);co_await winrt::resume_after(std::chrono::milliseconds(120));co_await ui;}}
      co_await winrt::resume_after(std::chrono::seconds(3));co_await ui;ClosePicker();
    }
    else if(command=="snapshot" || command=="network") {
      OpenSettings(command=="network"?"net":"diag");
      co_await winrt::resume_after(std::chrono::milliseconds(500));co_await ui;
      if(command=="snapshot")DrawDiagnostics();else DrawNetwork();
      co_await SaveDesignSurface(root,directory,command);
      if(command=="snapshot"){
        auto scroll=FindDesignPart(root,L"SettingsScroll").as<ScrollViewer>();scroll.ChangeView(nullptr,850.,nullptr,true);
        co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;co_await SaveDesignSurface(root,directory,"resources-top");
        scroll.ChangeView(nullptr,1300.,nullptr,true);co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;
        co_await SaveDesignSurface(root,directory,"resources-bottom");co_await SaveDesignSurface(FindDesignPart(root,L"ResourceTable"),directory,"resource-table");
      }
      CloseSettings();
    }
    auto r=resources::Read();auto out=json::Value::Object();out["command"]=command;out["private_bytes"]=int64_t(resources::PrivateBytes());out["collector_samples"]=int64_t(diagnosticHistory.Collected());out["collector_storage_bytes"]=int64_t(diagnosticHistory.StorageBytes());out["collector_cpu_ms"]=double(diagnosticHistory.Cpu100ns())/10000.;out["camera_child_private"]=int64_t(r.cameraPrivate);
    for(size_t i=0;i<3;++i){auto p=json::Value::Object();p["loaded"]=int(r.parts[i].loaded);p["estimated_bytes"]=int64_t(r.parts[i].loaded?r.parts[i].estimate:0);out[i==0?"voice":i==1?"dfn":"camera"]=std::move(p);}
    out["encoded_count"]=int64_t(r.encodedCount);out["encoded_bytes"]=int64_t(r.encodedBytes);out["decoded_count"]=int64_t(r.decodedCount);out["decoded_raster_bytes"]=int64_t(r.decodedBytes);out["emoji_count"]=int64_t(r.emojiCount);
    std::ofstream(directory/L"live-result.json")<<out.Dump();
  }catch(hresult_error const& e){logf("w8d live audit failed HRESULT %ld",e.code().value);}
  liveAuditBusy=false;
}
void W8dLivePoll(){
  if(liveAuditBusy || app::Environment(L"HARMONY_W8D_LIVE")!="1" || app::installer::TestRoot().empty())return;
  auto directory=app::installer::TestRoot();std::ifstream file(directory/L"live.command");std::string command;std::getline(file,command);
  if(command.empty() || command==liveAuditPrevious)return;liveAuditPrevious=command;W8dLiveAudit(directory,command);
}
winrt::Windows::Foundation::IAsyncAction W8dAudit() {
  auto lifetime=get_strong();winrt::apartment_context ui;
  try {
    designState.signedIn=true;designState.busy=false;designState.model.live=true;
    designState.model.rooms={{"fixture-room","geral",{{"fixture-self","Observador"}}}};
    designState.voice.self="fixture-self";Render();
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignSurface(root,designOutput,"idle");
    OpenTrayMenu();co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    co_await SaveDesignWindow(trayPopup,designOutput,L"tray-idle.png");CloseTrayMenu();
    designState.voice.Join("fixture-room","geral");designState.voice.connecting=false;showingCall=true;Render();
    for(float scale:{1.f,1.25f,1.5f}) {
      OpenTrayMenu();
      trayAuditScale=scale;SetWindowPos(trayPopup,nullptr,2300,300,int(app::TrayWidth*scale),int(app::TrayHeight(trayItems)*scale),SWP_NOZORDER|SWP_NOACTIVATE);
      InvalidateRect(trayPopup,nullptr,FALSE);UpdateWindow(trayPopup);
      co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
      // Warm PrintWindow after a size/DPI change before recording its result.
      co_await SaveDesignWindow(trayPopup,designOutput,L"tray-sizing.png");co_await ui;
      InvalidateRect(trayPopup,nullptr,FALSE);UpdateWindow(trayPopup);
      co_await SaveDesignWindow(trayPopup,designOutput,L"tray-call-"+std::to_wstring(int(scale*100))+L".png");
      trayHot=3;InvalidateRect(trayPopup,nullptr,FALSE);UpdateWindow(trayPopup);
      co_await SaveDesignWindow(trayPopup,designOutput,L"tray-hover-"+std::to_wstring(int(scale*100))+L".png");CloseTrayMenu();
    }
    trayAuditScale=0;
    designState.voice.muted=true;OpenTrayMenu();co_await SaveDesignWindow(trayPopup,designOutput,L"tray-muted.png");CloseTrayMenu();
    auto button=FindDesignPart(root,L"ShareCamera").as<Button>();co_await CameraMenu(button,false);co_await ui;
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"camera-menu.png");if(cameraMenu)cameraMenu.Hide();
    // Enough samples already exist before either diagnostics tab is opened.
    co_await winrt::resume_after(std::chrono::seconds(8));co_await ui;
    OpenSettings("diag");co_await winrt::resume_after(std::chrono::milliseconds(400));co_await ui;DrawDiagnostics();
    co_await SaveDesignSurface(root,designOutput,"diagnostic");
    auto scroll=FindDesignPart(root,L"SettingsScroll").as<ScrollViewer>();scroll.ChangeView(nullptr,750.,nullptr,true);
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignSurface(root,designOutput,"resources-top");scroll.ChangeView(nullptr,1200.,nullptr,true);
    co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    co_await SaveDesignSurface(root,designOutput,"resources-bottom");
    co_await SaveDesignSurface(FindDesignPart(root,L"ResourceTable"),designOutput,"resource-table");
    CloseSettings();OpenSettings("net");co_await winrt::resume_after(std::chrono::milliseconds(400));co_await ui;DrawNetwork();
    co_await SaveDesignSurface(root,designOutput,"network");CloseSettings();
    std::ofstream(designOutput/L"result.json")<<"{\"passed\":true,\"fixture_only\":true,\"monitor_x\":1952,\"dpi_render_scales\":[100,125,150]}";
  } catch(hresult_error const& e) {
    logf("w8d visual fixture failed HRESULT %ld",e.code().value);std::ofstream(designOutput/L"result.json")<<"{\"passed\":false}";
  }
  window.Close();Exit();
}
