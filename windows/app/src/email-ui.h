// Included inside App. The address belongs only to this account form.
struct EmailView {
  ContentControl region{nullptr};
  StackPanel host{nullptr}; PasswordBox password{nullptr}; TextBox address{nullptr}, code{nullptr};
  json::Value user; bool gate=false, open=false, changing=false, sent=false; uint64_t epoch=0;
  std::wstring info;
  void Clear() {
    ++epoch;
    if (password) password.Password(L""); if (address) address.Text(L""); if (code) code.Text(L"");
    password=nullptr; address=nullptr; code=nullptr;
  }
};
std::shared_ptr<EmailView> emailView;
bool emailGateShowing=false;
void ClearEmail() {
  if (emailView) { emailView->Clear(); emailView->user={}; emailView->host.Children().Clear(); emailView->host=nullptr; }
  emailView.reset();
}
void EmailAction(std::shared_ptr<EmailView> view, app::Service::AccountAction action) {
  if (!view->region.IsEnabled()) return;
  auto body=json::Value::Object();
  if (action==app::Service::AccountAction::ChangeEmail) {
    body["password"]=to_string(view->password.Password()); body["email"]=to_string(view->address.Text());
  } else if (action==app::Service::AccountAction::VerifyEmail) body["code"]=to_string(view->code.Text());
  auto epoch=view->epoch; view->region.IsEnabled(false);
  AccountUI(action,std::move(body),"",[this,view,epoch,action](net::Response result) {
    if (emailView!=view || view->epoch!=epoch) return;
    view->region.IsEnabled(true);
    if (result.status<200 || result.status>=300) {
      FindDesignPart(view->host,L"EmailStatus").as<TextBlock>().Text(net::Wide(app::ErrorMessage(result))); return;
    }
    if (action==app::Service::AccountAction::ChangeEmail) {
      view->user["email"]=to_string(view->address.Text()); view->user["email_verified"]=false;
      view->Clear(); view->sent=true; view->changing=false;
      view->info=view->gate ? L"Código enviado." : L"Enviamos um código para o novo e-mail.";
    } else if (action==app::Service::AccountAction::VerifyEmail) {
      view->user=result.Json()["user"]; view->Clear(); view->open=false; view->sent=false; view->info=L"E-mail trocado.";
      if (securityView) { securityView->Clear(); securityView->open=false; securityView->user=view->user; DrawSecurity(securityView); }
    } else { view->info=L"Código reenviado."; FindDesignPart(view->host,L"EmailStatus").as<TextBlock>().Text(view->info); return; }
    DrawEmail(view);
  });
}
void DrawEmail(std::shared_ptr<EmailView> view) {
  auto host=view->host; host.Children().Clear();
  if (!view->gate) {
    auto toggle=Action(view->open ? L"Cancelar" : L"Trocar",[this,view] {
      view->Clear(); view->open=!view->open; view->sent=false; view->info=L"";
      if (securityView) { securityView->Clear(); securityView->open=false; DrawSecurity(securityView); }
      DrawEmail(view);
    }); toggle.Name(L"EmailToggle"); host.Children().Append(SecurityRow(L"E-mail",view->user["email"].Str().empty()?L"não cadastrado":net::Wide(view->user["email"].Str()),toggle,view->user["email_verified"].Bool()?L"":L"não confirmado"));
  }
  if (view->gate || view->open) {
    bool addressForm=!view->sent || view->changing;
    if (view->gate) host.Children().Append(Text(addressForm ? L"Cadastre seu e-mail para recuperar a conta e receber avisos de segurança." :
      L"Digite o código que enviamos para " + net::Wide(view->user["email"].Str()) + L". Ele vale por 10 minutos; confira também o spam.",13,Muted));
    if (addressForm) {
      host.Children().Append(Text(view->gate ? L"E-mail" : L"Novo e-mail",13,Muted));
      view->address=HarmonyTextBox(); view->address.Name(L"EmailAddress"); Automation::AutomationProperties::SetName(view->address,view->gate ? L"E-mail" : L"Novo e-mail"); host.Children().Append(view->address);
      view->password=HarmonyPasswordBox(); view->password.Name(L"EmailPassword"); host.Children().Append(Text(L"Senha atual",13,Muted)); host.Children().Append(view->password);
      if (!view->gate) { view->address.Background(Brush(Surface3)); view->password.Background(Brush(Surface3)); }
      auto send=AccountPrimary(L"Enviar código",[this,view] { EmailAction(view,app::Service::AccountAction::ChangeEmail); }); send.Name(L"EmailSend"); send.IsEnabled(false);
      auto update=[address=view->address,password=view->password,send](auto const&,auto const&) { send.IsEnabled(!address.Text().empty() && !password.Password().empty()); };
      view->address.TextChanged(update); view->password.PasswordChanged(update); send.HorizontalAlignment(HorizontalAlignment::Stretch); host.Children().Append(send);
    } else {
      host.Children().Append(Text(view->gate ? L"Código" : L"Código recebido",13,Muted));
      view->code=HarmonyTextBox(); view->code.Name(L"EmailCode"); Automation::AutomationProperties::SetName(view->code,view->gate ? L"Código" : L"Código recebido"); host.Children().Append(view->code);
      if (!view->gate) view->code.Background(Brush(Surface3));
      auto confirm=AccountPrimary(view->gate ? L"Confirmar e-mail" : L"Confirmar",[this,view] { EmailAction(view,app::Service::AccountAction::VerifyEmail); }); confirm.Name(L"EmailConfirm"); confirm.IsEnabled(false);
      view->code.TextChanged([code=view->code,confirm](auto const&,auto const&) { confirm.IsEnabled(!code.Text().empty()); }); confirm.HorizontalAlignment(HorizontalAlignment::Stretch); host.Children().Append(confirm);
      auto resend=Action(view->gate ? L"Reenviar" : L"Reenviar código",[this,view] { EmailAction(view,app::Service::AccountAction::ResendEmail); }); resend.Name(L"EmailResend"); resend.HorizontalAlignment(HorizontalAlignment::Stretch);
      if (view->gate) {
        // The two second choices side by side, one width each, under the main one.
        auto other=Action(L"Usar outro e-mail",[this,view] { view->Clear(); view->changing=true; view->info=L""; DrawEmail(view); }); other.Name(L"EmailOther"); other.HorizontalAlignment(HorizontalAlignment::Stretch);
        Grid pair; pair.ColumnSpacing(8); for(int i=0;i<2;++i) { ColumnDefinition column; column.Width(GridLength{1,GridUnitType::Star}); pair.ColumnDefinitions().Append(column); }
        pair.Children().Append(resend); Grid::SetColumn(other,1); pair.Children().Append(other); host.Children().Append(pair);
      } else host.Children().Append(resend);
    }
  }
  auto status=Text(view->info,13,Muted); status.Name(L"EmailStatus"); CollapseEmpty(status); host.Children().Append(status);
  if (view->gate) {
    Grid footer; for(int i=0;i<2;++i) { ColumnDefinition column; column.Width(GridLength{1,GridUnitType::Star}); footer.ColumnDefinitions().Append(column); }
    auto link=[](Button button) { button.Background(Tint(0,0)); button.Foreground(Brush(Muted)); button.Height(32); button.MinHeight(32); button.Padding(Thickness{0}); };
    if (!ReadState().emailRequired) { auto skip=Action(L"Agora não",[this] { service->SkipEmail(); }); skip.Name(L"EmailSkip"); link(skip); skip.HorizontalAlignment(HorizontalAlignment::Left); footer.Children().Append(skip); }
    auto logout=Action(L"Sair da conta",[this] { service->Logout(); }); NegativeButton(logout); logout.HorizontalAlignment(HorizontalAlignment::Right); Grid::SetColumn(logout,1); footer.Children().Append(logout); host.Children().Append(footer);
  }
}
void LoadEmail(std::shared_ptr<EmailView> view) {
  AccountUI(app::Service::AccountAction::Me,{},"",[this,view](net::Response result) {
    if (emailView!=view) return;
    if (result.status!=200) {
      view->host.Children().Clear(); view->host.Children().Append(Text(net::Wide(app::ErrorMessage(result)),13,Muted));
      view->host.Children().Append(Action(L"Tentar de novo",[this,view] { LoadEmail(view); })); return;
    }
    view->user=result.Json()["user"]; view->sent=view->gate && !view->user["email"].Str().empty(); DrawEmail(view);
  });
}
void EmailControls(StackPanel body) {
  auto view=std::make_shared<EmailView>(); emailView=view; view->host=StackPanel(); view->host.Spacing(8);
  Border card; card.Background(Brush(Surface2)); card.Padding(Thickness{12,12,12,12}); card.CornerRadius(CornerRadius{8,8,8,8}); card.Child(view->host);
  view->region=ContentControl(); view->region.HorizontalContentAlignment(HorizontalAlignment::Stretch); view->region.Content(card); body.Children().Append(view->region);
  window.DispatcherQueue().TryEnqueue([this,view,panel=settingsOverlay] { if (emailView==view && settingsOverlay==panel) LoadEmail(view); });
}
void CloseEmailEditor() {
  if (emailView && !emailView->gate) { emailView->Clear(); emailView->open=false; emailView->info=L""; DrawEmail(emailView); }
}
void EmailGate() {
  if (emailGateShowing) return;
  emailGateShowing=true; showingRooms=false; page=nullptr; ClearEmail();
  root.Children().Clear(); root.ColumnDefinitions().Clear();
  StackPanel content; content.Spacing(16); content.MaxWidth(384); content.Margin(Thickness{24,24,24,24});
  content.HorizontalAlignment(HorizontalAlignment::Stretch); content.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(Text(L"harmony",20)); content.Children().Append(Text(L"Confirme seu e-mail",13,Muted));
  auto view=std::make_shared<EmailView>(); emailView=view; view->gate=true; view->host=StackPanel(); view->host.Spacing(16); view->region=ContentControl(); view->region.HorizontalContentAlignment(HorizontalAlignment::Stretch); view->region.Content(view->host); content.Children().Append(view->region);
  ScrollViewer scroll; scroll.Content(content); ImmediateMouseWheel(scroll); root.Children().Append(scroll); LoadEmail(view);
}
