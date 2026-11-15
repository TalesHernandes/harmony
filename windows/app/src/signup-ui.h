// Included inside App. A pasted invite is the only entrance to registration.
ContentDialog signupDialog{nullptr};
winrt::Windows::Foundation::IAsyncAction SignupDialog() {
  if (signupDialog || ReadState().busy || ReadState().signedIn) co_return;
  if (password) password.Password(L"");
  ContentDialog dialog; signupDialog=dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark);
  dialog.Title(FormTitle(L"Criar a sua conta"));
  StackPanel fields; fields.Spacing(12);
  fields.Children().Append(Text(L"Você foi convidado. O convite vale uma vez só.",13,Muted));
  // Each field with its label (and its hint) as one group, 4 apart inside.
  StackPanel group{nullptr};
  auto field=[this,fields,&group](std::wstring label,std::wstring name) {
    group=StackPanel(); group.Spacing(4); group.Children().Append(Text(label,13,Muted));
    TextBox box=HarmonyTextBox(); box.Name(name); FormField(box); Automation::AutomationProperties::SetName(box,label); group.Children().Append(box); fields.Children().Append(group); return box;
  };
  auto invite=field(L"Link ou código de convite",L"SignupInvite");
  auto email=field(L"E-mail",L"SignupEmail");
  auto username=field(L"Usuário",L"SignupUsername"); username.PlaceholderText(L"Ex.: joao");
  group.Children().Append(Text(L"É por ele que as pessoas te chamam: @joao.",13,Muted));
  auto display=field(L"Nome que aparece",L"SignupDisplay"); display.PlaceholderText(L"Ex.: João");
  StackPanel secretGroup; secretGroup.Spacing(4); secretGroup.Children().Append(Text(L"Senha",13,Muted));
  PasswordBox secret=HarmonyPasswordBox(); secret.Name(L"SignupPassword"); FormField(secret); secretGroup.Children().Append(secret);
  secretGroup.Children().Append(Text(L"Pelo menos 10 caracteres.",13,Muted)); fields.Children().Append(secretGroup);
  auto status=Text(L"",13,Muted); status.Name(L"SignupStatus");
  auto save=AccountPrimary(L"Criar conta",[]{}); save.Name(L"SignupCreate"); save.IsEnabled(false);
  auto update=[invite,email,username,secret,save](auto const&,auto const&) { save.IsEnabled(invite.IsEnabled() && !invite.Text().empty() && !email.Text().empty() && !username.Text().empty() && !secret.Password().empty()); };
  invite.TextChanged(update); email.TextChanged(update); username.TextChanged(update); secret.PasswordChanged(update);
  save.Click([this,dialog,invite,email,username,display,secret,save,status](auto const&,auto const&) {
    if (!save.IsEnabled()) return;
    auto body=json::Value::Object(); body["code"]=to_string(invite.Text()); body["email"]=to_string(email.Text()); body["username"]=to_string(username.Text()); body["display_name"]=to_string(display.Text()); body["password"]=to_string(secret.Password());
    for(auto box:{invite,email,username,display}) box.IsEnabled(false); secret.IsEnabled(false); save.IsEnabled(false); status.Text(L"Criando…");
    auto weak=get_weak(); auto dispatcher=window.DispatcherQueue();
    service->Signup(std::move(body),[weak,dispatcher,dialog,invite,email,username,display,secret,save,status](net::Response reply) {
      dispatcher.TryEnqueue([weak,dialog,invite,email,username,display,secret,save,status,reply=std::move(reply)] {
        if (auto self=weak.get(); self && !self->exiting && self->signupDialog==dialog) {
          if (reply.status==201) { secret.Password(L""); dialog.Hide(); }
          else { for(auto box:{invite,email,username,display}) box.IsEnabled(true); secret.IsEnabled(true); save.IsEnabled(true); status.Text(net::Wide(app::ErrorMessage(reply))); }
        }
      });
    });
  });
  CollapseEmpty(status); fields.Children().Append(status); fields.Children().Append(save); FormDialog(dialog,fields,save);
  ScrollViewer scroll; scroll.Content(fields); ImmediateMouseWheel(scroll); scroll.MaxHeight(std::max(200.0,root.ActualHeight()-200)); dialog.Content(scroll);
  try { co_await dialog.ShowAsync(); } catch(hresult_error const&) { logf("invite registration dialog failed"); }
  for(auto box:{invite,email,username,display}) box.Text(L""); secret.Password(L""); signupDialog=nullptr;
}
