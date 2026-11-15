void OwnPhotoHover(Border const& overlay,bool hovered) {overlay.Opacity(hovered?1:0);}
void ProfileAvatarAction(app::ChatPerson const& p,std::string const& me) {
    auto view=[this,p] { app::ChatAttachment photo; photo.url="/api/people/"+app::ChatEscape(p.username)+"/avatar?v="+app::ChatEscape(p.avatar); photo.name=p.displayName; app::ChatMessage message; message.author=p; OpenLightbox(photo,message); };
    if(p.username!=me) { if(!p.avatar.empty()) view(); return; }
    ownPhotoMenu=MenuFlyout(); auto menu=ownPhotoMenu;
    auto add=[&](wchar_t const* label,std::function<void()> action,bool danger=false) { MenuFlyoutItem item; item.Text(label); if(danger) {
      item.Style(Resources().Lookup(box_value(L"HarmonyDangerMenuItem")).as<Style>());
    } item.Click([action](auto const&,auto const&) {action();}); menu.Items().Append(item); };
    add(L"Alterar foto",[this,p] { PickFiles(L"Alterar foto",true,[this,p](auto files) { if(!files.empty()) EditPhoto(files.front(),[this,p](std::string path,std::function<void(bool)> complete){UpdateOwnPhoto(p,path,false,complete);}); }); });
    if(!p.avatar.empty()) { add(L"Ver foto",view); add(L"Remover foto",[this,p] { UpdateOwnPhoto(p,"",true); },true); }
    menu.ShowAt(FindDesignPart(profileOverlay,L"ProfileAvatar"));

}
// Update only the profile that initiated the request; private account replies never enter logs.
void UpdateOwnPhoto(app::ChatPerson person,std::string path,bool remove,std::function<void(bool)> complete={}) {
  auto weak=get_weak(); auto dispatcher=window.DispatcherQueue(); auto panel=profileOverlay;
  service->Account(remove?app::Service::AccountAction::RemoveAvatar:app::Service::AccountAction::Avatar,{},path,
    [weak,dispatcher,person,panel,complete](net::Response result) mutable {
      dispatcher.TryEnqueue([weak,person,panel,result,complete]() mutable { if(auto self=weak.get();self && !self->exiting) {
        if(result.status>=200 && result.status<300) {
          person.avatar=result.Json()["avatar"].Str(); auto event=json::Value::Object(); event["username"]=person.username; event["avatar"]=person.avatar;
          self->service->ChatModel().Event("profile",event); self->DrawAccount(self->ReadState());
          if(self->profileOverlay==panel) self->OpenProfile(person);
        } else if(self->profileOverlay==panel) FindDesignPart(panel,L"ProfileStatus").as<TextBlock>().Text(net::Wide(app::ErrorMessage(result)));
        if(complete) complete(result.status>=200 && result.status<300);
      }});
    });
}
// Included inside App. Person actions share one source across all entry points.
Grid profileOverlay{nullptr};
MenuFlyout ownPhotoMenu{nullptr};
void CloseProfile() {
  if(ownPhotoMenu) ownPhotoMenu.Hide();
  if (!profileOverlay) return;
  uint32_t at = 0;
  if (root.Children().IndexOf(profileOverlay, at)) root.Children().RemoveAt(at);
  profileOverlay = nullptr;
}
app::ChatPerson ResolvePerson(app::ChatPerson p) {
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    for (auto const& member : s.members) if (member.username == p.username) p = member;
  });
  return p;
}
void RingPerson(std::string who) {
  auto room = ReadState().voice.room;
  if (room.empty()) return;
  if (!profileOverlay) OpenProfile(app::ChatPerson{who});
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  auto panel=profileOverlay;
  service->ChatWork([room, who, dispatcher, weak, panel](app::Chat& chat) {
    auto result = chat.RingRoom(room, who);
    dispatcher.TryEnqueue([weak, result, panel] { if (auto self = weak.get(); self && !self->exiting && self->profileOverlay == panel) {
      auto status = FindDesignPart(self->profileOverlay, L"ProfileStatus").as<TextBlock>();
      status.Text(result.status >= 200 && result.status < 300 ? L"Chamado enviado." : net::Wide(app::ChatError(result)));
    }});
  });
}
struct PersonAction { std::wstring label; std::function<void()> pick; };
std::vector<PersonAction> PersonActions(app::ChatPerson const& p) {
  auto state = ReadState(); std::string me;
  service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; });
  std::vector<PersonAction> actions{{L"Ver perfil", [this, p] { OpenProfile(p); }}};
  if (p.username != me) actions.push_back({L"Ligar", [this,p] { CloseProfile(); directSmall=false; service->CallRing(app::CallPerson{p.username,net::Narrow(PersonName(p))}); }});
  if (p.username != me) actions.push_back({L"Conversar no privado", [this, who=p.username] { CloseProfile(); OpenDirect(who); }});
  std::string room, roomName;
  for (auto const& r : state.model.rooms) for (auto const& peer : r.people) if (peer.username == p.username) { room = r.id; roomName = r.name; }
  if (p.username != me && !state.voice.room.empty() && room != state.voice.room)
    actions.push_back({L"Chamar para a sala", [this, who=p.username] { RingPerson(who); }});
  if (!room.empty() && room != state.voice.room)
    actions.push_back({L"Entrar em " + net::Wide(roomName), [this, room] { CloseProfile(); showingCall=true; callPeople=false; callSignature.clear(); service->JoinVoice(room); }});
  return actions;
}
MenuFlyout PersonMenu(app::ChatPerson p) {
  MenuFlyout menu;
  for (auto const& action : PersonActions(p)) {
    MenuFlyoutItem item; HarmonyPointer(item); item.Text(action.label);
    item.Click([this, pick=action.pick](auto const&, auto const&) { if (!exiting) pick(); });
    menu.Items().Append(item);
  }
  return menu;
}
void BindPerson(FrameworkElement element, app::ChatPerson p) {
  HarmonyPointer(element); element.Tapped([this, p](auto const&, Input::TappedRoutedEventArgs const& e) { e.Handled(true); OpenProfile(p); });
  MenuAtPointer(element, PersonMenu(p));
}
void OpenProfile(app::ChatPerson p) {
  CloseProfile(); p = ResolvePerson(p);
  auto state = ReadState(); std::string me; std::vector<app::ChatRole> roles;
  service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; roles = app::ChatRolesOf(p.username, s.roles); });
  Grid veil; veil.Name(L"ProfileOverlay"); veil.Background(Tint(0, .65));
  Grid::SetColumnSpan(veil, (std::max)(1, int(root.ColumnDefinitions().Size())));
  Grid::SetRowSpan(veil, (std::max)(1, int(root.RowDefinitions().Size())));
  veil.Tapped([this](auto const&, auto const&) { CloseProfile(); });
  // The owner, 22/10/2026: empty gaps, an X "só jogado ali", and buttons of
  // every size that "não se conversam". One width (400), one gap (16), every
  // action a full-width row of the same height, field and button of one height.
  Border card; card.Name(L"PersonProfile"); card.MaxWidth(400); card.Margin(Thickness{16,16,16,16});
  card.HorizontalAlignment(HorizontalAlignment::Stretch); card.VerticalAlignment(VerticalAlignment::Center);
  card.Background(Brush(Surface)); card.CornerRadius(CornerRadius{16,16,16,16}); card.Padding(Thickness{20,20,20,20});
  card.Tapped([](auto const&, Input::TappedRoutedEventArgs const& e) { e.Handled(true); });
  StackPanel content; content.Spacing(16);
  Grid heading; heading.ColumnSpacing(16);
  for (auto w : {GridLength{72,GridUnitType::Pixel}, GridLength{1,GridUnitType::Star}, GridLength{32,GridUnitType::Pixel}}) { ColumnDefinition c; c.Width(w); heading.ColumnDefinitions().Append(c); }
  auto avatar=PhotoFace(p,72);
  if (auto initial=avatar.Child().try_as<TextBlock>()) { initial.FontSize(24); initial.LineHeight(32); }
  HarmonyPointer(avatar);
  avatar.Tapped([this,p,me](auto const&,Input::TappedRoutedEventArgs const& e) {e.Handled(true);ProfileAvatarAction(p,me);});
  avatar.Name(L"ProfileAvatar");
  if(p.username==me) {
    Grid photo; photo.Children().Append(avatar);
    auto pencil=VoiceIcon("pencil",Foreground,24).as<FrameworkElement>(); pencil.HorizontalAlignment(HorizontalAlignment::Center);pencil.VerticalAlignment(VerticalAlignment::Center);
    Border edit;edit.Name(L"OwnPhotoEditOverlay");edit.Background(Tint(0,.6));edit.CornerRadius(CornerRadius{36,36,36,36});edit.Child(pencil);edit.IsHitTestVisible(false);edit.Opacity(0);photo.Children().Append(edit);
    photo.PointerEntered([this,edit](auto const&,auto const&) {OwnPhotoHover(edit,true);});photo.PointerExited([this,edit](auto const&,auto const&) {OwnPhotoHover(edit,false);});
    heading.Children().Append(photo);
  } else heading.Children().Append(avatar);
  std::string room, roomName; bool screen = false;
  for (auto const& r : state.model.rooms) for (auto const& peer : r.people) if (peer.username == p.username) { room = r.id; roomName = r.name; screen = peer.screen; }
  // Name, handle, and one line for where the person is (presence and room
  // together, instead of a paragraph of its own under the heading).
  StackPanel names; names.VerticalAlignment(VerticalAlignment::Center); names.Spacing(2);
  std::optional<uint32_t> tint; for (auto const& r : roles) if ((tint = app::ChatColorValue(r.color))) break;
  auto title=Line(PersonName(p),20,tint.value_or(Foreground),600); title.Name(L"ProfileName"); title.TextTrimming(TextTrimming::CharacterEllipsis); names.Children().Append(title);
  names.Children().Append(Line(L"@" + net::Wide(p.username) + (p.nickname.empty() ? L"" : L" · " + net::Wide(p.displayName)),13,Muted,500));
  auto presence = state.model.StateOf(p.username);
  std::wstring where = presence == "in_call" ? L"Em chamada" : presence == "away" ? L"Ausente" : presence == "offline" ? L"Offline" : L"Online";
  if (!room.empty()) where += L" · em " + net::Wide(roomName) + (screen ? L", transmitindo a tela" : L"");
  StackPanel whereRow; whereRow.Orientation(Orientation::Horizontal); whereRow.Spacing(6);
  Shapes::Ellipse dot; dot.Width(8); dot.Height(8); dot.VerticalAlignment(VerticalAlignment::Center);
  dot.Fill(Brush(presence == "offline" ? 0x71717a : presence == "away" ? 0xf5a524 : Success)); whereRow.Children().Append(dot);
  auto whereText=Line(where,13,Muted,500); whereText.TextTrimming(TextTrimming::CharacterEllipsis); whereRow.Children().Append(whereText);
  names.Children().Append(whereRow);
  Grid::SetColumn(names,1); heading.Children().Append(names);
  // The red X of every panel (owner, 19/10), level with the name line.
  auto close = Square("xmark", L"Fechar perfil",32,16,Foreground,Surface2,[this] { CloseProfile(); });
  close.VerticalAlignment(VerticalAlignment::Top); close.Name(L"CloseProfile"); Grid::SetColumn(close,2); heading.Children().Append(close); content.Children().Append(heading);
  ProfileRolesSection(content,p);  // roles-ui.h
  // Every action one row: same width, same 40 height, icon then words.
  StackPanel rows; rows.Spacing(4);
  auto addRow=[&](std::wstring const& label,std::string const& icon,std::function<void(FrameworkElement const&)> pick,wchar_t const* name=nullptr) {
    Button b; HarmonyPointer(b); b.Style(Resources().Lookup(box_value(L"HarmonyButton")).as<Style>());
    b.Height(40); b.MinHeight(0); b.Padding(Thickness{12,0,12,0}); b.CornerRadius(CornerRadius{8,8,8,8}); b.BorderThickness(Thickness{0,0,0,0});
    b.HorizontalAlignment(HorizontalAlignment::Stretch); b.HorizontalContentAlignment(HorizontalAlignment::Stretch); b.Background(Brush(Default));
    if (name) b.Name(name);
    Grid row; row.ColumnSpacing(12); ColumnDefinition mark; mark.Width(GridLength{20,GridUnitType::Pixel}); row.ColumnDefinitions().Append(mark);
    ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); row.ColumnDefinitions().Append(words);
    auto glyph=VoiceIcon(icon,Foreground,18).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph);
    auto text=Line(label,14,Foreground,500); Grid::SetColumn(text,1); text.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(text);
    b.Content(row); Automation::AutomationProperties::SetName(b,label);
    b.Click([this,pick](auto const& sender,auto const&){ if(!exiting) pick(sender.template as<FrameworkElement>()); });
    rows.Children().Append(b);
  };
  auto actions = PersonActions(p);
  for (size_t i=1; i<actions.size(); ++i) {
    auto const& label=actions[i].label;
    auto icon = label==L"Ligar" ? "handset" : label.starts_with(L"Conversar") ? "comment" : label.starts_with(L"Chamar") ? "bell" : "headphones";
    addRow(label,icon,[pick=actions[i].pick](FrameworkElement const&){pick();},label==L"Ligar"?L"ProfileCall":nullptr);
  }
  if (screen && p.username != me && room == state.voice.room) addRow(L"Assistir à tela","display",[this,who=p.username](FrameworkElement const&){ CloseProfile(); showingCall=true; WatchScreen(who); });
  if (rows.Children().Size()) content.Children().Append(rows);
  if (p.username != me) {
    StackPanel personal; personal.Spacing(4); PersonalButtons(personal, p.username, true); content.Children().Append(personal);
  }
  // The volume right here in the profile (the owner, 25/10/2026: the menu it
  // opened spilled out of the panel): the voice, and the live's sound apart.
  if (p.username != me && !room.empty() && room == state.voice.room) {
    StackPanel volumes; volumes.Spacing(8); volumes.Name(L"ProfileVolume");
    volumes.Children().Append(Line(L"VOLUME — SÓ PARA VOCÊ",12,Muted,600));
    auto volumeRow = [&](std::wstring const& label, std::string const& key, wchar_t const* name) {
      Border control; control.Height(44); control.Padding(Thickness{12,0,12,0}); control.Background(Brush(Surface2)); control.CornerRadius(CornerRadius{8,8,8,8});
      Grid row; row.ColumnSpacing(12);
      for (auto w : {GridLength{84,GridUnitType::Pixel}, GridLength{1,GridUnitType::Star}, GridLength{44,GridUnitType::Pixel}}) { ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c); }
      auto text = Line(label,13,Foreground,500); text.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(text);
      double now = (designOutput.empty() ? ReadState() : designState).voice.Volume(key) * 100;
      Slider slider; HarmonyPointer(slider); slider.Name(name); slider.Minimum(0); slider.Maximum(200); slider.StepFrequency(1); slider.Value(now); slider.VerticalAlignment(VerticalAlignment::Center);
      Grid::SetColumn(slider,1); row.Children().Append(slider); Automation::AutomationProperties::SetName(slider, label + L" de " + PersonName(p));
      auto percent = Action(std::to_wstring(int(now)) + L"%", [weak = make_weak(slider)] { if (auto value = weak.get()) value.Value(100); });
      percent.Height(18); percent.MinHeight(18); percent.MinWidth(0); percent.Padding(Thickness{0}); percent.FontSize(13); percent.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
      percent.Foreground(Brush(Muted)); percent.Background(nullptr); percent.VerticalAlignment(VerticalAlignment::Center); percent.HorizontalAlignment(HorizontalAlignment::Right);
      ToolTipService::SetToolTip(percent, box_value(L"Voltar a 100%")); Grid::SetColumn(percent,2); row.Children().Append(percent);
      slider.ValueChanged([this, key, percent](auto const&, auto const& args) {
        percent.Content(box_value(std::to_wstring(int(args.NewValue())) + L"%"));
        if (designOutput.empty()) service->VoiceVolume(key, args.NewValue() / 100); else designState.voice.Volume(key, args.NewValue() / 100);
      });
      control.Child(row); volumes.Children().Append(control);
    };
    volumeRow(L"Voz", p.username, L"ProfileVoiceVolume");
    if (screen) volumeRow(L"Som da live", p.username + "/screen_audio", L"ProfileLiveVolume");
    content.Children().Append(volumes);
  }
  if (p.username != me) {
    StackPanel section; section.Spacing(8);
    section.Children().Append(Line(L"COMO VOCÊ CHAMA ESSA PESSOA",12,Muted,600));
    TextBox nickname=HarmonyTextBox(); nickname.Name(L"ProfileNickname"); nickname.MaxLength(32); nickname.Text(net::Wide(p.nickname)); nickname.PlaceholderText(net::Wide(p.displayName));
    nickname.Height(40); nickname.MinHeight(40); nickname.FontSize(15); nickname.Background(Brush(Surface2)); nickname.CornerRadius(CornerRadius{8,8,8,8});
    Grid entry; entry.ColumnSpacing(8); ColumnDefinition field; field.Width(GridLength{1,GridUnitType::Star}); entry.ColumnDefinitions().Append(field);
    ColumnDefinition button; button.Width(GridLength{0,GridUnitType::Auto}); entry.ColumnDefinitions().Append(button); entry.Children().Append(nickname);
    auto save=Action(L"Salvar", [] {}); save.Name(L"SaveNickname");
    save.Style(Resources().Lookup(box_value(L"HarmonyPrimaryButton")).as<Style>());
    save.Click([this,p,nickname,save](auto const&,auto const&) {
      save.IsEnabled(false); nickname.IsEnabled(false);
      auto dispatcher=window.DispatcherQueue(); auto weak=get_weak(); auto panel=profileOverlay;
      service->ChatWork([p,name=net::Narrow(nickname.Text().c_str()),dispatcher,weak,panel,save,nickname](app::Chat& chat) {
        auto result=chat.Nickname(p.username,name);
        dispatcher.TryEnqueue([weak,p,result,panel,save,nickname] { if (auto self=weak.get(); self && !self->exiting && self->profileOverlay == panel) {
          if (result.status >= 200 && result.status < 300) self->OpenProfile(p);
          else { save.IsEnabled(true); nickname.IsEnabled(true); FindDesignPart(panel,L"ProfileStatus").as<TextBlock>().Text(net::Wide(app::ChatError(result))); }
        }});
      });
    });
    save.Height(40); save.MinHeight(40); save.Padding(Thickness{16,0,16,0}); Grid::SetColumn(save,1); entry.Children().Append(save);
    section.Children().Append(entry); section.Children().Append(Line(L"Só você vê esse nome.",13,Muted,500)); content.Children().Append(section);
  }
  auto status=Text(L"",13,Muted); status.Name(L"ProfileStatus"); status.Visibility(Visibility::Collapsed);
  status.RegisterPropertyChangedCallback(TextBlock::TextProperty(),[](auto const& sender,auto const&){auto t=sender.template as<TextBlock>();t.Visibility(t.Text().empty()?Visibility::Collapsed:Visibility::Visible);});
  content.Children().Append(status);
  ScrollViewer scroll; scroll.Content(content); scroll.HorizontalContentAlignment(HorizontalAlignment::Stretch); ImmediateMouseWheel(scroll); ScrollLaneWhenNeeded(scroll); card.Child(scroll); // equal sides unless it scrolls
  Input::KeyboardAccelerator escape; escape.Key(winrt::Windows::System::VirtualKey::Escape);
  escape.Invoked([this](auto const&,Input::KeyboardAcceleratorInvokedEventArgs const& e) { e.Handled(true); CloseProfile(); }); card.KeyboardAccelerators().Append(escape); card.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
  veil.Children().Append(card); profileOverlay=veil; root.Children().Append(veil);
  if (app::Environment(L"HARMONY_AUDIT_PERSONAL").empty()) close.Focus(FocusState::Programmatic);
}

#ifndef HARMONY_PUBLISH
winrt::Windows::Foundation::IAsyncAction Parity6ProfileAudit() {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction {co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;};
  OpenOwnProfile(); co_await settle(); co_await ui;
  ParityRequire(bool(profileOverlay) && !FindDesignPart(profileOverlay,L"ProfileCall"));
  app::ChatPerson own;
  service->ChatModel().Inspect([&](app::ChatState const& state) {for(auto const& person:state.members) if(person.username==state.owner) own=person;});
  UpdateOwnPhoto(own,app::Environment(L"HARMONY_TEST_PICTURE"),false);
  for(int i=0;i<100;++i) {
    bool ready=false; service->ChatModel().Inspect([&](app::ChatState const& state) {for(auto const& person:state.members) if(person.username==state.owner) ready=!person.avatar.empty();});
    if(ready) break; co_await settle();co_await ui;
  }
  co_await settle();co_await ui;
  auto face=FindDesignPart(profileOverlay,L"ProfileAvatar"); OwnPhotoHover(FindDesignPart(profileOverlay,L"OwnPhotoEditOverlay").as<Border>(),true); co_await settle();co_await ui;
  co_await SaveDesignSurface(root,chatOutput,"round6-own-profile-photo");
  ProfileAvatarAction(ResolvePerson(own),own.username);co_await settle();co_await ui;
  ParityRequire(ownPhotoMenu && ownPhotoMenu.IsOpen() && ownPhotoMenu.Items().Size()==3);
  auto removeItem=ownPhotoMenu.Items().GetAt(2).as<MenuFlyoutItem>();
  ParityRequire(DesignColor(FindDesignPart(removeItem,L"DangerItem").as<Border>().Background())==0xdb3b3e);
  VisualStateManager::GoToState(removeItem,L"PointerOver",false);
  ParityRequire(DesignColor(FindDesignPart(removeItem,L"DangerItem").as<Border>().Background())==0xe15451 &&
                DesignColor(FindDesignPart(removeItem,L"DangerText").as<TextBlock>().Foreground())==Foreground);
  co_await SaveDesignSurface(removeItem,chatOutput,"round6-remove-photo-hover");
  VisualStateManager::GoToState(removeItem,L"Normal",false);
  for(auto popup:Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
    if(auto surface=popup.Child().try_as<FrameworkElement>();surface && surface.Name()!=L"SmokeLayerBackground")
      co_await SaveDesignSurface(surface,chatOutput,"round6-own-photo-menu");
  auto invoke=[](MenuFlyoutItem const& item) {Automation::Peers::MenuFlyoutItemAutomationPeer(item).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();};
  invoke(ownPhotoMenu.Items().GetAt(1).as<MenuFlyoutItem>());co_await settle();co_await ui;
  ParityRequire(bool(lightbox));co_await SaveDesignSurface(root,chatOutput,"round6-profile-photo-viewer");
  CloseLightbox();
  ProfileAvatarAction(ResolvePerson(own),own.username);co_await settle();co_await ui;
  invoke(ownPhotoMenu.Items().GetAt(2).as<MenuFlyoutItem>());
  for(int i=0;i<100;++i) {
    bool removed=false; service->ChatModel().Inspect([&](app::ChatState const& state) {for(auto const& person:state.members) if(person.username==state.owner) removed=person.avatar.empty();});
    if(removed) break;co_await settle();co_await ui;
  }
  co_await settle();co_await ui;
  ProfileAvatarAction(ResolvePerson(own),own.username);co_await settle();co_await ui;
  ParityRequire(ownPhotoMenu.Items().Size()==1);ownPhotoMenu.Hide();CloseProfile();
  app::ChatPerson peer;peer.username="bruna";OpenProfile(peer);co_await settle();co_await ui;
  ParityRequire(bool(FindDesignPart(profileOverlay,L"ProfileCall")));
  co_await SaveDesignSurface(root,chatOutput,"round6-other-profile-call");CloseProfile();
}

#endif
