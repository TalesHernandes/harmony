// Compact account rows share the web's label / value / action grouping.
Grid AccountRow(std::wstring title,std::wstring value,Button action=nullptr,std::wstring note=L"") {
  Grid row; row.ColumnSpacing(12);
  for(auto width:{GridLength{0,GridUnitType::Auto},GridLength{1,GridUnitType::Star},GridLength{0,GridUnitType::Auto}}) { ColumnDefinition c; c.Width(width); row.ColumnDefinitions().Append(c); }
  auto label=Text(title,13,Muted); label.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(label);
  StackPanel words; words.VerticalAlignment(VerticalAlignment::Center); words.HorizontalAlignment(HorizontalAlignment::Right);
  auto text=Line(value,15,Foreground,500); words.Children().Append(text);
  if(!note.empty()) words.Children().Append(Text(note,13,Muted));
  Grid::SetColumn(words,1); row.Children().Append(words);
  if(action) { action.Height(44); action.MinHeight(44); action.MinWidth(104); action.FontSize(13); action.Padding(Thickness{12,0,12,0}); action.HorizontalContentAlignment(HorizontalAlignment::Center); action.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(action,2); row.Children().Append(action); }
  return row;
}
Border AccountCard(UIElement child) {
  Border card; card.Background(Brush(Surface2)); card.Padding(Thickness{12,12,12,12}); card.CornerRadius(CornerRadius{8,8,8,8}); card.Child(child); return card;
}
Grid SecurityRow(std::wstring title,std::wstring value,Button action,std::wstring note=L"") {
  Grid row; row.ColumnSpacing(12);
  ColumnDefinition wordsColumn; wordsColumn.Width(GridLength{1,GridUnitType::Star}); row.ColumnDefinitions().Append(wordsColumn);
  ColumnDefinition actionColumn; actionColumn.Width(GridLength{0,GridUnitType::Auto}); row.ColumnDefinitions().Append(actionColumn);
  StackPanel words; words.Children().Append(Text(title,13,Muted)); words.Children().Append(Text(value,15,Foreground));
  if(!note.empty()) words.Children().Append(Text(note,13,Muted));
  row.Children().Append(words); action.Height(44); action.MinHeight(44); action.MinWidth(104); action.FontSize(13); action.Padding(Thickness{12,0,12,0}); action.HorizontalContentAlignment(HorizontalAlignment::Center); action.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(action,1); row.Children().Append(action); return row;
}
void CollapseEmpty(TextBlock const& text) {
  auto update=[weak=make_weak(text)] { if(auto t=weak.get()) t.Visibility(t.Text().empty()?Visibility::Collapsed:Visibility::Visible); };
  update(); text.RegisterPropertyChangedCallback(TextBlock::TextProperty(),[update](auto const&,auto const&) { update(); });
}
// Included inside App. Photos use the web's authenticated avatar endpoints.
Button AccountPrimary(std::wstring title, std::function<void()> clicked) {
  auto button=Action(title,std::move(clicked)); button.Style(Resources().Lookup(box_value(L"HarmonyPrimaryButton")).as<Style>());
  button.Resources().Insert(box_value(L"ButtonBackgroundDisabled"),Brush(Default));
  button.Resources().Insert(box_value(L"ButtonForegroundDisabled"),Brush(Muted));
  button.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0x3592f9));
  button.Resources().Insert(box_value(L"ButtonBackgroundPressed"),Brush(0x3592f9));
  return button;
}
void ProfileControls(StackPanel body, app::ChatPerson self) {
  Border profile; profile.Name(L"EditProfile");
  StackPanel fields; fields.Spacing(16);
  auto face=StackPanel(); face.Name(L"OwnPhoto"); face.Children().Append(PhotoFace(self,96));
  auto statusLine=Text(L"",13,Muted); statusLine.Name(L"PhotoStatus"); CollapseEmpty(statusLine);
  auto pick=Action(L"Trocar foto",[]{}); pick.Name(L"ChangePhoto");
  auto remove=Action(L"Remover foto",[]{}); remove.Name(L"RemovePhoto"); remove.IsEnabled(!self.avatar.empty());
  auto change=[this,face,self,statusLine,pick,remove](std::string path, bool deleting,std::function<void(bool)> complete={}) {
    bool hadPhoto=remove.IsEnabled();
    pick.IsEnabled(false); remove.IsEnabled(false); statusLine.Text(L"Salvando…");
    AccountUI(deleting ? app::Service::AccountAction::RemoveAvatar : app::Service::AccountAction::Avatar,{},path,
      [this,face,self,statusLine,pick,remove,deleting,hadPhoto,complete](net::Response result) mutable {
        pick.IsEnabled(true);
        if (result.status>=200 && result.status<300) {
          self.avatar=deleting ? "" : result.Json()["avatar"].Str();
          face.Children().Clear(); face.Children().Append(PhotoFace(self,96));
          auto event=json::Value::Object(); event["username"]=self.username; event["avatar"]=self.avatar;
          service->ChatModel().Event("profile",event);
          if (auto header=FindDesignPart(settingsOverlay,L"AccountProfileFace").try_as<Border>()) header.Child(PhotoFace(self,48));
          DrawAccount(ReadState()); statusLine.Text(L"Foto atualizada.");
          remove.IsEnabled(!self.avatar.empty());
        } else { statusLine.Text(net::Wide(app::ErrorMessage(result))); remove.IsEnabled(hadPhoto); }
        if(complete)complete(result.status>=200&&result.status<300);
      });
  };
  pick.Click([this,change](auto const&,auto const&) {
    if (!chatOutput.empty()) { change(app::Environment(L"HARMONY_TEST_PICTURE"),false); return; }
    PickFiles(L"Trocar foto",true,[this,change](auto files) {
      if(!files.empty()) EditPhoto(files.front(),[change](std::string path,std::function<void(bool)> complete){change(path,false,complete);});
    });
  });
  remove.Click([change](auto const&,auto const&) { change("",true); });
  Grid photoRow; photoRow.ColumnSpacing(16);
  ColumnDefinition avatarColumn; avatarColumn.Width(GridLength{96,GridUnitType::Pixel}); photoRow.ColumnDefinitions().Append(avatarColumn);
  ColumnDefinition actionsColumn; actionsColumn.Width(GridLength{1,GridUnitType::Star}); photoRow.ColumnDefinitions().Append(actionsColumn);
  face.VerticalAlignment(VerticalAlignment::Center); photoRow.Children().Append(face);
  StackPanel actions; actions.Spacing(12); actions.VerticalAlignment(VerticalAlignment::Center);
  for(auto button:{pick,remove}) { button.Height(44); button.MinHeight(44); button.Width(144); button.HorizontalAlignment(HorizontalAlignment::Left); actions.Children().Append(button); }
  Grid::SetColumn(actions,1); photoRow.Children().Append(actions); fields.Children().Append(photoRow);
  fields.Children().Append(Text(L"JPEG, PNG ou GIF; até 5 MiB, 8000 pixels por lado e 40 megapixels.",13,Muted));
  fields.Children().Append(statusLine); profile.Child(fields); body.Children().Append(profile);
}
// Included inside App. Private account data stays in the open settings card.
PasswordBox accountCurrent{nullptr}, accountNext{nullptr};
void ClearAccountSecrets() {
  ClearSecurity();
  ClearEmail();
  if (accountCurrent) accountCurrent.Password(L"");
  if (accountNext) accountNext.Password(L"");
  accountCurrent=nullptr; accountNext=nullptr;
}
void AccountUI(app::Service::AccountAction action, json::Value body, std::string id,
    std::function<void(net::Response)> complete) {
  auto panel=settingsOverlay; auto weak=get_weak(); auto dispatcher=window.DispatcherQueue();
  service->Account(action,std::move(body),std::move(id),[weak,dispatcher,panel,complete=std::move(complete)](net::Response result) {
    dispatcher.TryEnqueue([weak,panel,complete,result=std::move(result)] {
      if (auto self=weak.get(); self && !self->exiting && self->settingsOverlay==panel && self->ReadState().signedIn) complete(result);
    });
  });
}
void LoadAccountSessions(StackPanel list, TextBlock statusLine, Button refresh) {
  refresh.IsEnabled(false); statusLine.Text(L"Carregando aparelhos…");
  AccountUI(app::Service::AccountAction::Sessions,{},"",[this,list,statusLine,refresh](net::Response result) {
    refresh.IsEnabled(true); list.Children().Clear();
    if (result.status!=200) { statusLine.Text(net::Wide(app::ErrorMessage(result))); return; }
    statusLine.Text(L"");
    auto response=result.Json();
    for (auto const& session:response["sessions"].Elements()) {
      Grid device; device.ColumnSpacing(12);
      ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); device.ColumnDefinitions().Append(words);
      ColumnDefinition action; action.Width(GridLength{0,GridUnitType::Auto}); device.ColumnDefinitions().Append(action);
      StackPanel row; row.VerticalAlignment(VerticalAlignment::Center); device.Children().Append(row);
      bool current=session["current"].Bool();
      auto agent=net::Wide(session["user_agent"].Str());
      row.Children().Append(Line(current ? L"Este aparelho" : agent.empty() ? L"Aparelho" : agent,15,Foreground,500));
      auto stamp=[](std::string const& iso) {
        auto time=app::ChatLocal(app::ChatSeconds(iso)); wchar_t value[32];
        swprintf_s(value,L"%02d/%02d, %02d:%02d",time.tm_mday,time.tm_mon+1,time.tm_hour,time.tm_min); return std::wstring(value);
      };
      row.Children().Append(Text(L"Entrou " + stamp(session["created_at"].Str()) + L" · visto " + stamp(session["last_seen_at"].Str()),13,Muted));
      if (!current) {
        auto revoke=Action(L"Desconectar",[]{}); revoke.Name(L"RevokeSession");
        revoke.Height(44); revoke.MinHeight(44); revoke.Padding(Thickness{12,0,12,0}); revoke.FontSize(13); revoke.VerticalAlignment(VerticalAlignment::Center);
        NegativeButton(revoke);
        revoke.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0xdb3b3e));
        revoke.PointerEntered([revoke](auto const&,auto const&) { revoke.Foreground(Brush(Foreground)); });

        revoke.Click([this,id=session["id"].Str(),revoke,list,statusLine,refresh](auto const&,auto const&) {
          revoke.IsEnabled(false); refresh.IsEnabled(false);
          AccountUI(app::Service::AccountAction::RevokeSession,{},id,[this,revoke,list,statusLine,refresh](net::Response reply) {
            refresh.IsEnabled(true); revoke.IsEnabled(true);
            if (reply.status<200 || reply.status>=300) { statusLine.Text(L"Não desconectou: " + net::Wide(app::ErrorMessage(reply))); return; }
            LoadAccountSessions(list,statusLine,refresh);
          });
        }); Grid::SetColumn(revoke,1); device.Children().Append(revoke);
      }
      Border card; card.Background(Brush(Surface2)); card.Padding(Thickness{12,12,12,12}); card.CornerRadius(CornerRadius{8,8,8,8}); card.Child(device); list.Children().Append(card);
    }
  });
}
void AccountControls(StackPanel body) {
  StackPanel section; section.Spacing(8); section.Children().Append(Text(L"CONTA E SEGURANÇA",13,Muted));
  StackPanel securityRows; securityRows.Spacing(2); section.Children().Append(securityRows); body.Children().Append(section);
  EmailControls(securityRows);
  Expander change; change.Name(L"ChangePassword");
  auto passwordToggle=Action(L"Trocar",[change] { change.IsExpanded(!change.IsExpanded()); });
  change.Header(SecurityRow(L"Senha",L"••••••••••",passwordToggle));
  change.Expanding([passwordToggle](auto const&,auto const&) { passwordToggle.Content(box_value(L"Cancelar")); });
  change.Collapsed([passwordToggle](auto const&,auto const&) { passwordToggle.Content(box_value(L"Trocar")); });
  change.HorizontalAlignment(HorizontalAlignment::Stretch); change.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  StackPanel fields; fields.Spacing(8);
  fields.Children().Append(Text(L"Senha atual",13,Muted)); accountCurrent=HarmonyPasswordBox(); auto current=accountCurrent; current.Name(L"CurrentPassword"); fields.Children().Append(current);
  fields.Children().Append(Text(L"Nova senha (pelo menos 10 caracteres)",13,Muted)); accountNext=HarmonyPasswordBox(); auto next=accountNext; next.Name(L"NewPassword"); fields.Children().Append(next);
  auto statusLine=Text(L"",13,Muted); statusLine.Name(L"PasswordStatus"); CollapseEmpty(statusLine);
  auto save=AccountPrimary(L"Salvar nova senha",[]{}); save.Name(L"SavePassword"); save.IsEnabled(false);
  auto update=[current,next,save](auto const&,auto const&) { save.IsEnabled(current.IsEnabled() && !current.Password().empty() && next.Password().size()>=10); };
  current.PasswordChanged(update); next.PasswordChanged(update);
  save.Click([this,current,next,save,statusLine,change](auto const&,auto const&) {
    if (!current.IsEnabled() || current.Password().empty() || next.Password().size()<10) return;
    auto request=json::Value::Object(); request["current"]=to_string(current.Password()); request["new"]=to_string(next.Password());
    current.IsEnabled(false); next.IsEnabled(false); save.IsEnabled(false); statusLine.Text(L"Salvando…");
    AccountUI(app::Service::AccountAction::Password,std::move(request),"",[current,next,save,statusLine,change](net::Response result) {
      current.IsEnabled(true); next.IsEnabled(true);
      if (result.status>=200 && result.status<300) {
        current.Password(L""); next.Password(L""); change.IsExpanded(false);
        statusLine.Text(L"Senha trocada. Os outros aparelhos saíram da conta.");
      } else { statusLine.Text(net::Wide(app::ErrorMessage(result))); save.IsEnabled(!current.Password().empty() && next.Password().size()>=10); }
    });
  });
  change.Collapsed([current,next](auto const&,auto const&) { current.Password(L""); next.Password(L""); });
  fields.Children().Append(save); change.Content(fields); securityRows.Children().Append(change); section.Children().Append(statusLine);
  SecurityControls(securityRows);
  StackPanel devices; devices.Spacing(8); body.Children().Append(devices); body=devices;
  body.Children().Append(Text(L"APARELHOS CONECTADOS",13,Muted));
  StackPanel list; list.Spacing(2); list.Name(L"AccountSessions"); body.Children().Append(list);
  auto sessionStatus=Text(L"",13,Muted); sessionStatus.Name(L"SessionsStatus"); CollapseEmpty(sessionStatus); body.Children().Append(sessionStatus);
  auto refresh=Action(L"Atualizar aparelhos",[]{}); refresh.Name(L"RefreshSessions");
  refresh.Click([this,list,sessionStatus,refresh](auto const&,auto const&) { LoadAccountSessions(list,sessionStatus,refresh); });
  auto others=Action(L"Desconectar todos os outros",[]{}); others.Name(L"RevokeOtherSessions");
  others.Height(44); others.MinHeight(44); others.Padding(Thickness{12,0,12,0}); others.FontSize(13); others.HorizontalAlignment(HorizontalAlignment::Left);
  others.Click([this,list,sessionStatus,refresh,others](auto const&,auto const&) {
    others.IsEnabled(false); refresh.IsEnabled(false);
    AccountUI(app::Service::AccountAction::RevokeOthers,{},"",[this,list,sessionStatus,refresh,others](net::Response result) {
      others.IsEnabled(true); refresh.IsEnabled(true);
      if (result.status<200 || result.status>=300) { sessionStatus.Text(L"Não desconectou os outros: " + net::Wide(app::ErrorMessage(result))); return; }
      LoadAccountSessions(list,sessionStatus,refresh);
    });
  }); body.Children().Append(others);
  // Defer until OpenSettings has attached the card, so the callback's identity
  // check cannot mistake the former tab for the new one.
  window.DispatcherQueue().TryEnqueue([this,list,sessionStatus,refresh,panel=settingsOverlay] {
    if (!exiting && settingsOverlay==panel && ReadState().signedIn) LoadAccountSessions(list,sessionStatus,refresh);
  });
}
