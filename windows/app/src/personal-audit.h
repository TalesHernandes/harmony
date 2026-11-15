#ifndef HARMONY_PUBLISH
// Runs off-screen. Invoke providers and RenderTargetBitmap only; no system input or capture.
winrt::Windows::Foundation::IAsyncAction PersonalAudit(std::string& stage) {
  auto lifetime = get_strong(); winrt::apartment_context ui;
  auto pause = [](int ms) { return winrt::resume_after(std::chrono::milliseconds(ms)); };
  auto press = [this](FrameworkElement const& element) {
    ParityRequire(bool(element));
    Automation::Peers::ButtonAutomationPeer(element.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
  };
  auto part = [this](std::wstring const& name) { return FindDesignPart(root, name.c_str()).try_as<FrameworkElement>(); };
#define PERSONAL_WAIT(cond) do { for (int i_ = 0; i_ < 150 && !(cond); ++i_) { co_await pause(100); co_await ui; } ParityRequire(cond); } while (0)
  stage = "personal-login";
  service->Restore(); PERSONAL_WAIT(!ReadState().busy);
  user.Text(net::Wide(app::Environment(L"HARMONY_TEST_USER"))); password.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD"))); Submit();
  PERSONAL_WAIT(ReadState().signedIn && !ReadState().needsEmail && PersonalState().personalReady && messageItems && messageItems.Size() > 0);
  co_await pause(500); co_await ui;
  std::string textChannel = chatActive;
  auto who = app::Environment(L"HARMONY_TEST_PEER");
  stage = "personal-profile"; OpenProfile(app::ChatPerson{who});
  co_await pause(300); co_await ui;
  ParityRequire(part(L"PersonalMute") && part(L"PersonalBlock"));
  co_await SaveDesignSurface(profileOverlay, chatOutput, "windows-perfil");
  press(part(L"PersonalMute")); PERSONAL_WAIT(!personalBusy && PersonalState().Silenced(who));
  stage = "personal-confirmation"; press(part(L"PersonalBlock")); PERSONAL_WAIT(bool(personalDialog));
  co_await pause(400); co_await ui;
  co_await SaveDesignSurface(personalDialog, chatOutput, "windows-confirmacao");
  auto primary = FindDesignPart(personalDialog, L"PrimaryButton").try_as<FrameworkElement>(); ParityRequire(bool(primary)); press(primary);
  PERSONAL_WAIT(!personalDialog && !personalBusy && PersonalState().Blocked(who));
  CloseProfile();
  stage = "personal-message";
  auto id = std::stoll(app::Environment(L"HARMONY_TEST_MESSAGE"));
  PERSONAL_WAIT(bool(part(L"PersonalReveal-" + std::to_wstring(id))));
  co_await SaveDesignSurface(root, chatOutput, "windows-mensagem-recolhida");
  press(part(L"PersonalReveal-" + std::to_wstring(id)));
  PERSONAL_WAIT(personalRevealed.contains(id) && !part(L"PersonalFold-" + std::to_wstring(id)));
  co_await SaveDesignSurface(root, chatOutput, "windows-mensagem-aberta");
  stage = "personal-private";
  OpenDirect(who); PERSONAL_WAIT(chatActive != textChannel && bool(part(L"PersonalConversation")) && part(L"PersonalConversation").Visibility() == Visibility::Visible);
  co_await SaveDesignSurface(root, chatOutput, "windows-privado-bloqueado");
  stage = "personal-settings"; OpenSettings("conta");
  co_await pause(400); co_await ui;
  personalPanel.StartBringIntoView(); co_await pause(300); co_await ui;
  co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "windows-ajustes");
  CloseSettings();
  // Render the real room view against a controlled state, without opening any media device.
  stage = "personal-seat-fixture";
  auto state = ReadState(); ParityRequire(!state.model.rooms.empty());
  auto& room = state.model.rooms.front();
  state.voice.room = room.id; state.voice.roomName = room.name; state.voice.self = PersonalState().owner;
  app::Person peer; peer.username = who; peer.name = who; room.people = {peer};
  showingCall = true; callSignature.clear(); DrawCall(state); root.UpdateLayout();
  co_await pause(400); co_await ui;
  co_await SaveDesignSurface(root, chatOutput, "windows-vaga-silenciada");
  showingCall = false; callSignature.clear(); Render();
  stage = "personal-other-session";
  std::ofstream(chatOutput / L"external-ready") << "ready";
  PERSONAL_WAIT(!PersonalState().Blocked(who) && !PersonalState().Silenced(who));
  OpenSettings("conta"); co_await pause(300); co_await ui;
  personalPanel.StartBringIntoView(); co_await pause(300); co_await ui;
  co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "windows-ajustes-vazio");
  stage = "personal-done";
#undef PERSONAL_WAIT
}
#endif
