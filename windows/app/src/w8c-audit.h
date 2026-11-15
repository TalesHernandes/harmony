// Offline screenshots use the production view with synthetic, unpersisted state.
winrt::Windows::Foundation::IAsyncAction W8cAudit() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  try {
    co_await SaveDesignSurface(root,designOutput,"login");
    co_await SaveDesignWindow(hwnd,designOutput,L"login-window.png"); co_await ui;
    OpenSettings("diag");
    co_await winrt::resume_after(std::chrono::seconds(12)); co_await ui;
    co_await SaveDesignSurface(root,designOutput,"diagnostic");
    CloseSettings();
    designState.signedIn=true; designState.busy=false; designState.model.live=true;
    designState.model.rooms={{"fixture-room","geral",{{"fixture-self","Observador"},{"fixture-friend","Amigo"}}}};
    designState.voice.self="fixture-self";
    Render();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    co_await SaveDesignSurface(root,designOutput,"people");
    designState.voice.Join("fixture-room","geral"); designState.voice.connecting=false; showingCall=true;
    Render();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    co_await SaveDesignSurface(root,designOutput,"camera");
#ifdef HARMONY_W8C_CAMERA
    auto button=FindDesignPart(root,L"ShareCamera").as<Button>();
    co_await CameraMenu(button,false); co_await ui;
    co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"camera-menu.png");
    if(cameraMenu) cameraMenu.Hide();
#endif
    // The screen picker while already sharing: the "stop" strip on top.
    designState.voice.sharingScreen=true; ScreenPicker(false);
    co_await winrt::resume_after(std::chrono::milliseconds(2500)); co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"screen-picker.png"); co_await ui;
    for(auto popup:Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot())) if(auto shown=popup.Child().try_as<ContentDialog>()) shown.Hide();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    designState.voice.sharingScreen=false;
    // The remove confirmation and a profile, as the owner sees them.
    {app::ChatMessage message; message.id=1; message.body="LACRANO"; message.author.username="fixture-friend"; message.author.displayName="Amigo"; RemoveMessage(message);}
    co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
    co_await SaveDesignWindow(hwnd,designOutput,L"remove-message.png"); co_await ui;
    for(auto popup:Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot())) if(auto shown=popup.Child().try_as<ContentDialog>()) shown.Hide();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    {app::ChatPerson friendPerson; friendPerson.username="fixture-friend"; friendPerson.displayName="Amigo"; OpenProfile(friendPerson);}
    co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
    co_await SaveDesignSurface(root,designOutput,"profile"); CloseProfile();
    OpenSettings("net");
    co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
    co_await SaveDesignSurface(root,designOutput,"network");
    std::ofstream(designOutput/L"result.json")<<"{\"passed\":true,\"fixture_only\":true,\"monitor_x\":1952}";
  } catch(hresult_error const& e) {
    logf("w8c fixture failed HRESULT %ld",e.code().value);
    std::ofstream(designOutput/L"result.json")<<"{\"passed\":false}";
  }
  window.Close(); Exit();
}
