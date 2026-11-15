// Included inside App. The recovery link continues in the browser that opens it.
Button LoginLink(std::wstring label,std::function<void()> action) {
  auto button=Action(label,std::move(action));button.Background(Tint(0,0));button.Foreground(Brush(Muted));
  button.Height(18);button.MinHeight(18);button.MinWidth(0);button.Padding(Thickness{0});button.FontSize(13);button.FontWeight(Weight(500));
  button.HorizontalAlignment(HorizontalAlignment::Left);
  button.Template(Markup::XamlReader::Load(LR"(<ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Button"><ContentPresenter Content="{TemplateBinding Content}" Foreground="{TemplateBinding Foreground}" VerticalContentAlignment="Center"/></ControlTemplate>)").as<ControlTemplate>());
  return button;
}
// The forms that open over the login page wear the login's look: one width,
// fields on their own ground, the action full width, and "Voltar para entrar"
// a quiet link under it (the owner, 22/10/2026: buttons of every size that
// "não se conversam", and a red "Voltar" alone in a corner).
TextBlock FormTitle(std::wstring const& title) { auto t=Text(title,20); t.FontWeight(Weight(600)); return t; }
void FormField(Control const& field) { field.Height(44); field.MinHeight(44); field.Background(Brush(Default)); field.HorizontalAlignment(HorizontalAlignment::Stretch); }
void FormDialog(ContentDialog const& dialog, StackPanel const& content, Button const& primary) {
  dialog.Background(Brush(Surface)); dialog.BorderThickness(Thickness{0}); dialog.CornerRadius(CornerRadius{12,12,12,12});
  for (auto key:{L"ContentDialogMaxWidth",L"ContentDialogMinWidth"}) dialog.Resources().Insert(box_value(key),box_value(420.0));
  dialog.Resources().Insert(box_value(L"ContentDialogBackground"),Brush(Surface));
  dialog.Resources().Insert(box_value(L"ContentDialogBorderWidth"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogSmokeFill"),Tint(0,.7));
  dialog.Resources().Insert(box_value(L"ContentDialogSeparatorThickness"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogTopOverlay"),Tint(0,0));
  dialog.Resources().Insert(box_value(L"ContentDialogPadding"),box_value(Thickness{24,24,24,24}));
  dialog.CloseButtonText(L"");
  primary.Style(Resources().Lookup(box_value(L"HarmonyLoginButton")).as<Style>()); primary.HorizontalAlignment(HorizontalAlignment::Stretch);
  primary.Resources().Insert(box_value(L"ButtonBackgroundDisabled"),Brush(Default)); primary.Resources().Insert(box_value(L"ButtonForegroundDisabled"),Brush(Muted));
  primary.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0x3592f9));
  auto back=LoginLink(L"Voltar para entrar",[weak=make_weak(dialog)]{ if(auto d=weak.get()) d.Hide(); });
  back.Name(L"FormBack"); back.HorizontalAlignment(HorizontalAlignment::Center); content.Children().Append(back);
  Input::KeyboardAccelerator escape; escape.Key(winrt::Windows::System::VirtualKey::Escape);
  escape.Invoked([weak=make_weak(dialog)](auto const&,Input::KeyboardAcceleratorInvokedEventArgs const& e){ e.Handled(true); if(auto d=weak.get()) d.Hide(); });
  content.KeyboardAccelerators().Append(escape); content.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
}
ContentDialog forgotDialog{nullptr};
winrt::Windows::Foundation::IAsyncAction ForgotPassword() {
  if (forgotDialog || !service || ReadState().busy || ReadState().signedIn) co_return;
  if (password) password.Password(L"");
  ContentDialog dialog; forgotDialog=dialog;
  dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark);
  dialog.Title(FormTitle(L"Esqueci a senha"));
  StackPanel content; content.Spacing(12);
  content.Children().Append(Text(L"Mandamos um link para o e-mail da conta.",13,Muted));
  StackPanel field; field.Spacing(4); field.Children().Append(Text(L"Nome ou e-mail",13,Muted));
  TextBox identifier=HarmonyTextBox(); identifier.Name(L"ForgotIdentifier"); FormField(identifier); Automation::AutomationProperties::SetName(identifier,L"Nome ou e-mail"); field.Children().Append(identifier); content.Children().Append(field);
  auto statusLine=Text(L"",13,Muted); statusLine.Name(L"ForgotStatus");
  auto send=Action(L"Enviar link",[]{}); send.Name(L"SendRecoveryLink"); send.IsEnabled(false);
  identifier.TextChanged([identifier,send](auto const&,auto const&) { send.IsEnabled(identifier.IsEnabled() && !identifier.Text().empty()); });
  send.Click([this,dialog,identifier,send,statusLine](auto const&,auto const&) {
    if (!identifier.IsEnabled() || identifier.Text().empty()) return;
    identifier.IsEnabled(false); send.IsEnabled(false); statusLine.Text(L"Enviando…");
    auto weak=get_weak(); auto dispatcher=window.DispatcherQueue();
    service->ForgotPassword(to_string(identifier.Text()),[weak,dispatcher,dialog,identifier,send,statusLine](net::Response result) {
      dispatcher.TryEnqueue([weak,dialog,identifier,send,statusLine,result=std::move(result)] {
        if (auto self=weak.get(); self && !self->exiting && self->forgotDialog==dialog && !self->ReadState().signedIn) {
          if (result.status>=200 && result.status<300) {
            identifier.Text(L""); identifier.Visibility(Visibility::Collapsed); send.Visibility(Visibility::Collapsed);
            statusLine.Text(L"Se a conta tem e-mail confirmado, enviaremos um link. Confira também o spam.");
          } else {
            identifier.IsEnabled(true); send.IsEnabled(!identifier.Text().empty()); statusLine.Text(net::Wide(app::ErrorMessage(result)));
          }
        }
      });
    });
  }); content.Children().Append(statusLine); CollapseEmpty(statusLine); content.Children().Append(send); FormDialog(dialog,content,send); dialog.Content(content);
  try { co_await dialog.ShowAsync(); } catch (hresult_error const&) { logf("password recovery dialog failed"); }
  identifier.Text(L""); forgotDialog=nullptr;
}
