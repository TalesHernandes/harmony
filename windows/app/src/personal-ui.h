// Included inside App. The account's own preferences never enter member cards.
StackPanel personalPanel{nullptr}, personalComposer{nullptr};
ContentDialog personalDialog{nullptr};
uint64_t personalShown = 0;
std::set<int64_t> personalRevealed;
bool personalBusy = false;
std::wstring personalProblem;
app::ChatState PersonalState() {
  app::ChatState state;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    state.owner = s.owner; state.personal = s.personal; state.personalReady = s.personalReady;
    state.personalVersion = s.personalVersion; state.conversations = s.conversations;
  });
  return state;
}
void PersonalRun(std::string who, bool block, bool enabled) {
  if (personalBusy) return;
  personalBusy = true; personalProblem.clear();
  auto weak = get_weak(); auto dispatcher = window.DispatcherQueue();
  service->ChatWork([weak, dispatcher, who, block, enabled](app::Chat& chat) {
    auto error = chat.Personal(who, block, enabled);
    dispatcher.TryEnqueue([weak, error] { if (auto self = weak.get(); self && !self->exiting) {
      self->personalBusy = false; self->personalProblem = net::Wide(error);
      if (self->profileOverlay) self->OpenProfile(app::ChatPerson{self->profileFor});
      self->DrawPersonal();
    } });
  });
}
void AskPersonalBlock(std::string who) {
  ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark);
  dialog.Name(L"PersonalBlockDialog"); ConfirmLook(dialog);
  dialog.Title(Line(L"Bloquear esta pessoa?", 20, Foreground, 600));
  auto explanation = Text(L"Você deixa de ouvir a voz e a tela dela, as mensagens ficam recolhidas e não haverá novas mensagens privadas nem chamadas entre vocês. Ela não será avisada.", 14, Muted);
  explanation.TextWrapping(TextWrapping::Wrap); dialog.Content(explanation);
  dialog.PrimaryButtonText(L"Bloquear"); dialog.CloseButtonText(L"Cancelar"); dialog.DefaultButton(ContentDialogButton::Close);
  personalDialog = dialog;
  auto weak = get_weak(); auto dispatcher = window.DispatcherQueue();
  dialog.ShowAsync().Completed([weak, dispatcher, who](auto const& result, auto) {
    auto yes = result.GetResults() == ContentDialogResult::Primary;
    dispatcher.TryEnqueue([weak, who, yes] { if (auto self = weak.get(); self && !self->exiting) {
      self->personalDialog = nullptr; if (yes) self->PersonalRun(who, true, true);
    } });
  });
}
void PersonalButtons(StackPanel const& target, std::string const& who, bool profile = false) {
  auto state = PersonalState(); auto at = state.personal.find(who);
  bool muted = at != state.personal.end() && at->second.first, blocked = state.Blocked(who);
  // The rows of the profile's actions (person-ui.h): icon, then words, at the left.
  auto rowOf = [this](std::string const& icon, std::wstring const& label, std::function<void()> pick) {
    Button b; HarmonyPointer(b); b.Style(Resources().Lookup(box_value(L"HarmonyButton")).as<Style>());
    b.MinHeight(0); b.Padding(Thickness{12, 0, 12, 0}); b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0, 0, 0, 0});
    b.HorizontalContentAlignment(HorizontalAlignment::Stretch); b.Background(Brush(Default));
    Grid row; row.ColumnSpacing(12); ColumnDefinition mark; mark.Width(GridLength{20, GridUnitType::Pixel}); row.ColumnDefinitions().Append(mark);
    ColumnDefinition words; words.Width(GridLength{1, GridUnitType::Star}); row.ColumnDefinitions().Append(words);
    auto glyph = VoiceIcon(icon, Foreground, 18).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph);
    auto text = Line(label, 14, Foreground, 500); Grid::SetColumn(text, 1); text.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(text);
    b.Content(row); Automation::AutomationProperties::SetName(b, label);
    b.Click([this, pick](auto const&, auto const&) { if (!exiting) pick(); });
    return b;
  };
  auto mute = rowOf("microphone-slash", muted ? L"Tirar o silêncio" : L"Silenciar", [this, who, muted] { PersonalRun(who, false, !muted); });
  auto block = rowOf("ban", blocked ? L"Desbloquear" : L"Bloquear", [this, who, blocked] { if (blocked) PersonalRun(who, true, false); else AskPersonalBlock(who); });
  if (profile) { mute.Name(L"PersonalMute"); block.Name(L"PersonalBlock"); }
  for (auto button : {mute, block}) {
    button.Height(40); button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.IsEnabled(!personalBusy && state.personalReady); target.Children().Append(button);
  }
  if (!personalProblem.empty()) target.Children().Append(Text(personalProblem, 13, DangerSoftFg));
}
void DrawPersonal() {
  if (!personalPanel || !settings || settingsTab != "conta") return;
  personalPanel.Children().Clear();
  personalPanel.Children().Append(Line(L"PESSOAS SILENCIADAS E BLOQUEADAS", 13, Muted, 600));
  auto state = PersonalState();
  if (!state.personalReady) { personalPanel.Children().Append(Text(L"Não foi possível carregar a lista. Tente novamente.", 13, DangerSoftFg)); return; }
  bool any = false;
  for (auto const& [who, flags] : state.personal) if (flags.first || flags.second) {
    any = true; StackPanel card; card.Spacing(8); card.Children().Append(Line(L"@" + net::Wide(who), 14, Foreground, 500));
    PersonalButtons(card, who); personalPanel.Children().Append(card);
  }
  if (!any) personalPanel.Children().Append(Text(L"Você não silenciou nem bloqueou ninguém.", 13, Muted));
}
void PersonalControls(StackPanel const& body) {
  personalPanel = StackPanel(); personalPanel.Name(L"PersonalPanel"); personalPanel.Spacing(12);
  body.Children().Append(personalPanel); DrawPersonal();
}
void RefreshPersonalUI() {
  auto state = PersonalState();
  if (personalShown != state.personalVersion) {
    personalShown = state.personalVersion; personalRevealed.clear();
    if (searchOpen) CloseSearch();
    callSignature.clear();
    if (profileOverlay && !profileFor.empty()) OpenProfile(app::ChatPerson{profileFor});
    DrawPersonal();
  }
  if (!personalComposer || !composer) return;
  std::string peer;
  for (auto const& c : state.conversations) if (c.id == chatActive && c.kind == "dm" && state.Blocked(c.peer.username)) peer = c.peer.username;
  personalComposer.Children().Clear();
  personalComposer.Visibility(peer.empty() ? Visibility::Collapsed : Visibility::Visible);
  auto island = FindDesignPart(root, L"ComposerIsland").try_as<FrameworkElement>();
  if (island) island.IsHitTestVisible(peer.empty());
  composer.IsReadOnly(!peer.empty());
  UpdateSend();
  if (!peer.empty()) {
    auto text = Text(L"Você bloqueou esta pessoa. Desbloqueie para conversar.", 14, Muted); text.TextWrapping(TextWrapping::Wrap);
    personalComposer.Children().Append(text);
    personalComposer.Children().Append(RoleButton("ban", L"Desbloquear", false, [this, peer] { PersonalRun(peer, true, false); }));
  }
}
