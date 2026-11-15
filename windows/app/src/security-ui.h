// Included inside App. This private view never enters chat or voice state.
struct SecurityView {
  ContentControl region{nullptr};
  StackPanel host{nullptr};
  PasswordBox password{nullptr}; TextBox code{nullptr};
  TextBlock proof{nullptr};
  json::Value user, setup;
  std::vector<std::string> recovery;
  bool open=false, sent=false; uint64_t epoch=0;
  std::wstring info;
  void Clear() {
    ++epoch;
    if (password) password.Password(L"");
    if (code) code.Text(L"");
    if (proof) proof.Text(L"");
    password=nullptr; code=nullptr; proof=nullptr; setup={}; recovery.clear(); sent=false;
  }
};
std::shared_ptr<SecurityView> securityView;
void ClearSecurity() {
  if (securityView) { securityView->Clear(); securityView->user={}; securityView->host.Children().Clear(); securityView->host=nullptr; }
  securityView.reset();
}
void FactorAction(std::shared_ptr<SecurityView> view, app::Service::AccountAction action) {
  if (!view->region.IsEnabled()) return;
  auto body=json::Value::Object(); body["password"]=to_string(view->password.Password());
  if (view->code) body["code"]=to_string(view->code.Text());
  auto epoch=view->epoch; view->region.IsEnabled(false);
  AccountUI(action,std::move(body),"",[this,view,epoch,action](net::Response result) {
    if (securityView!=view || view->epoch!=epoch) return;
    view->region.IsEnabled(true);
    if (result.status<200 || result.status>=300) {
      view->info=net::Wide(app::ErrorMessage(result));
      FindDesignPart(view->host,L"FactorStatus").as<TextBlock>().Text(view->info); return;
    }
    auto answer=result.Json();
    if (!answer["secret"].Str().empty()) {
      view->setup=answer; view->code.Text(L""); view->info=L"";
    } else if (answer["code_required"].Bool()) {
      view->sent=true; view->info=L"Enviamos um código para o seu e-mail.";
    } else {
      view->Clear(); view->open=false;
      if (answer["user"].IsObject()) view->user=answer["user"];
      for (auto const& code:answer["recovery_codes"].Elements()) view->recovery.push_back(code.Str());
      view->info=action==app::Service::AccountAction::DisableFactor ? L"Verificação em duas etapas desligada." : L"Verificação em duas etapas atualizada.";
    }
    DrawSecurity(view);
  });
}
void DrawSecurity(std::shared_ptr<SecurityView> view) {
  auto previous=view->password ? view->password.Password() : hstring{};
  if (view->password) view->password.Password(L"");
  if (view->code) view->code.Text(L"");
  if (view->proof) view->proof.Text(L"");
  auto host=view->host; host.Children().Clear();
  auto method=view->user["twofa"].Str();
  bool verified=view->user["email_verified"].Bool();
  auto toggle=Action(view->open ? L"Cancelar" : method.empty() ? L"Ligar" : L"Mudar",[this,view] {
    CloseEmailEditor();
    view->Clear(); view->open=!view->open; view->info=L""; DrawSecurity(view);
  }); toggle.Name(L"FactorToggle"); toggle.IsEnabled(verified); host.Children().Append(SecurityRow(L"Verificação em duas etapas",method=="totp"?L"Por app autenticador":method=="email"?L"Por e-mail":L"Desligada",toggle,verified?L"":L"confirme o e-mail antes"));
  if (view->open) {
    view->password=HarmonyPasswordBox(); view->password.Background(Brush(Surface3)); view->password.Password(previous); view->password.Name(L"FactorPassword");
    host.Children().Append(Text(L"Senha atual",13,Muted)); host.Children().Append(view->password);
    view->code=HarmonyTextBox(); view->code.Background(Brush(Surface3)); view->code.Name(L"FactorCode");
    auto add=[this,view,host](std::wstring title, std::wstring name, app::Service::AccountAction action, bool needsCode) {
      auto button=Action(title,[this,view,action] { FactorAction(view,action); }); button.Name(name);
      if(action==app::Service::AccountAction::DisableFactor) {
        button.Style(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
        NegativeButton(button);
      } else if (action!=app::Service::AccountAction::EmailEnable && action!=app::Service::AccountAction::Recovery) {
        button.Background(Brush(Accent)); button.Resources().Insert(box_value(L"ButtonBackgroundDisabled"),Brush(Default));
        button.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0x3592f9));
      }
      auto update=[password=view->password,code=view->code,button,needsCode](auto const&,auto const&) { button.IsEnabled(!password.Password().empty() && (!needsCode || !code.Text().empty())); };
      view->password.PasswordChanged(update); view->code.TextChanged(update);
      button.IsEnabled(!view->password.Password().empty() && (!needsCode || !view->code.Text().empty()));
      button.HorizontalAlignment(HorizontalAlignment::Stretch); button.Height(44); host.Children().Append(button); // the choices one width, as a list
    };
    if (!view->setup["secret"].Str().empty()) {
      host.Children().Append(Text(L"Sem câmera? Digite esta chave no app:",13,Muted));
      view->proof=Text(net::Wide(view->setup["secret"].Str()),16); view->proof.IsTextSelectionEnabled(true); HarmonySelection(view->proof); host.Children().Append(view->proof);
      host.Children().Append(Text(L"Vale por 10 minutos.",13,Muted));
      host.Children().Append(Action(L"Abrir no app autenticador",[this,view] {
        auto uri=view->setup["uri"].Str();
        if (uri.starts_with("otpauth://totp/")) ShellExecuteW(hwnd,L"open",net::Wide(uri).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
      }));
      host.Children().Append(Text(L"Código que o app mostra",13,Muted)); host.Children().Append(view->code);
      add(L"Confirmar e ligar",L"FactorEnableTotp",app::Service::AccountAction::TotpEnable,true);
    } else if (method.empty()) {
      host.Children().Append(Text(L"Além da senha, entrar vai pedir um código. Escolha de onde ele vem.",13,Muted));
      add(L"Por app autenticador (recomendado)",L"FactorSetup",app::Service::AccountAction::TotpSetup,false);
      add(L"Por e-mail",L"FactorEnableEmail",app::Service::AccountAction::EmailEnable,false);
    } else {
      if (method=="totp" || view->sent) {
        host.Children().Append(Text(method=="totp" ? L"Código do app ou de recuperação" : L"Código recebido por e-mail",13,Muted)); host.Children().Append(view->code);
      }
      add(method=="email" && !view->sent ? L"Enviar código para desligar" : L"Desligar",L"FactorDisable",app::Service::AccountAction::DisableFactor,method=="totp" || view->sent);
      if (method=="totp") add(L"Gerar novos códigos de recuperação",L"FactorRecovery",app::Service::AccountAction::Recovery,false);
      host.Children().Append(Text(L"Para trocar de método, desligue e ligue de novo.",13,Muted));
    }
  }
  if (!view->recovery.empty()) {
    host.Children().Append(Text(L"Códigos de recuperação",16));
    host.Children().Append(Text(L"Se perder o celular, cada um destes entra no lugar do código do app, uma vez. Guarde fora deste aparelho. Gerar novos invalida estes.",13,Muted));
    std::wstring codes; for (auto const& c:view->recovery) codes+=net::Wide(c)+L"\n";
    view->proof=Text(codes,16); view->proof.IsTextSelectionEnabled(true); HarmonySelection(view->proof); view->proof.Name(L"RecoveryCodes"); host.Children().Append(view->proof);
    host.Children().Append(Action(L"Copiar",[view] {
      try {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage data; data.SetText(view->proof.Text());
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
        FindDesignPart(view->host,L"FactorStatus").as<TextBlock>().Text(L"Códigos copiados.");
      } catch (hresult_error const&) { FindDesignPart(view->host,L"FactorStatus").as<TextBlock>().Text(L"Deu algo errado. Tente de novo."); }
    }));
    auto stored=Action(L"Já guardei",[this,view] { view->Clear(); DrawSecurity(view); }); stored.Name(L"RecoveryStored"); host.Children().Append(stored);
  }
  auto status=Text(view->info,13,Muted); status.Name(L"FactorStatus"); CollapseEmpty(status); host.Children().Append(status);
}
void SecurityControls(StackPanel body) {
  auto view=std::make_shared<SecurityView>(); securityView=view; view->host=StackPanel(); view->host.Spacing(8);
  Border card; card.Background(Brush(Surface2)); card.Padding(Thickness{12,12,12,12}); card.CornerRadius(CornerRadius{8,8,8,8}); card.Child(view->host);
  view->region=ContentControl(); view->region.HorizontalContentAlignment(HorizontalAlignment::Stretch); view->region.Content(card); body.Children().Append(view->region);
  window.DispatcherQueue().TryEnqueue([this,view,panel=settingsOverlay] {
    if (settingsOverlay!=panel || securityView!=view) return;
    AccountUI(app::Service::AccountAction::Me,{},"",[this,view](net::Response result) {
      if (securityView!=view) return;
      if (result.status!=200) { view->host.Children().Append(Text(net::Wide(app::ErrorMessage(result)),13,Muted)); return; }
      view->user=result.Json()["user"]; DrawSecurity(view);
    });
  });
}
