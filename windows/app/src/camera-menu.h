// Same 36px options, 4px inset and 8px corners as Modal.tsx Choice.
Flyout cameraMenu{nullptr};
winrt::Windows::Foundation::IAsyncAction CameraMenu(Button anchor,bool direct) {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  auto room=ReadState().voice.room;
  auto call=service->CallRead().id;
  co_await winrt::resume_background();
  auto devices=camera::Devices();
  co_await ui;
  if(exiting || !anchor.IsLoaded()) co_return;
  if(direct ? service->CallRead().id!=call : ReadState().voice.room!=room) co_return;
  if(cameraMenu) cameraMenu.Hide();
  Flyout menu; cameraMenu=menu; menu.Placement(Primitives::FlyoutPlacementMode::Top);
  menu.FlyoutPresenterStyle(Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="FlyoutPresenter"><Setter Property="FontWeight" Value="Medium"/><Setter Property="BorderThickness" Value="0"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="FlyoutPresenter"><Border Background="#323235" CornerRadius="8" Padding="4"><ContentPresenter Content="{TemplateBinding Content}"/></Border></ControlTemplate></Setter.Value></Setter></Style>)").as<Style>());
  StackPanel content; content.Name(L"CameraMenu"); content.Width(264); content.Spacing(0);
  {auto title=Text(L"CÂMERA",13,Muted);title.Margin(Thickness{8,4,8,0});content.Children().Append(title);}
  auto state=ReadState().voice; bool live=direct?service->CallRead().cam:state.sharingCamera;
  auto pick=[this,direct,room,call,menu](bool on,std::string id) {
    if(direct ? service->CallRead().id!=call : ReadState().voice.room!=room) {menu.Hide();return;}
    if(!designOutput.empty()) {designState.voice.cameraID=id;designState.voice.sharingCamera=on;}
    else service->Camera(on,std::move(id),direct);
    menu.Hide();
  };
  auto add=[&](std::wstring name,std::string id) {
    bool chosen=id==state.cameraID;
    auto button=Action(L"",[pick,id,live] {pick(live,id);});
    button.Height(36);button.MinHeight(36);button.Padding(Thickness{8,0,8,0});
    button.HorizontalAlignment(HorizontalAlignment::Stretch);button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    button.Background(chosen?Brush(Default):Tint(0,0));
    // The current camera is the highlighted, heavier row; no check mark.
    auto label=Text(name,15,Foreground);
    label.FontWeight(Weight(chosen?600:500));label.TextTrimming(TextTrimming::CharacterEllipsis);label.TextWrapping(TextWrapping::NoWrap);
    button.Content(label); Automation::AutomationProperties::SetName(button,name+(chosen?L", atual":L""));
    content.Children().Append(button);
  };
  add(L"Padrão do Windows","");
  for(auto& device:devices) add(net::Wide(device.name),device.id);
  if(!state.cameraID.empty() && std::none_of(devices.begin(),devices.end(),[&](auto& d){return d.id==state.cameraID;}))
    {auto note=Text(L"A câmera escolhida está desconectada.",13,Muted);note.Margin(Thickness{8,0,8,0});content.Children().Append(note);}
  if(devices.empty()) {auto note=Text(L"Nenhuma câmera encontrada.",13,Muted);note.Margin(Thickness{8,0,8,0});content.Children().Append(note);}
  // Turning the camera on or off lives in the same menu the button opens (the
  // owner's choice, 22/10/2026): a menu item like the tray's, not a big button.
  {Border line;line.Height(1);line.Margin(Thickness{8,4,8,4});line.Background(Brush(0x3f3f46));content.Children().Append(line);}
  {bool can=live || !devices.empty();uint32_t color=!can?Muted:live?0xf5645b:Foreground;
   auto toggle=Action(L"",[pick,id=state.cameraID,live] {pick(!live,id);});
   toggle.Height(36);toggle.MinHeight(36);toggle.Padding(Thickness{8,0,8,0});toggle.Background(Tint(0,0));
   toggle.HorizontalAlignment(HorizontalAlignment::Stretch);toggle.HorizontalContentAlignment(HorizontalAlignment::Left);
   StackPanel row;row.Orientation(Orientation::Horizontal);row.Spacing(10);row.Children().Append(VoiceIcon("camera",color,16));
   auto label=Text(live?L"Desligar a câmera":L"Ligar a câmera",15,color);label.FontWeight(Weight(500));row.Children().Append(label);
   toggle.Content(row);toggle.IsEnabled(can);Automation::AutomationProperties::SetName(toggle,live?L"Desligar a câmera":L"Ligar a câmera");
   content.Children().Append(toggle);}
  // A scroll viewer reserves a strip for its bar even when nothing scrolls;
  // only a long list gets one.
  if(content.Children().Size()>8) {ScrollViewer scroll;scroll.MaxHeight(320);scroll.Content(content);ImmediateMouseWheel(scroll);menu.Content(scroll);}
  else menu.Content(content);
  menu.ShowAt(anchor);
}

UIElement CameraControls(bool direct) {
  auto state=ReadState().voice;bool live=direct?service->CallRead().cam:state.sharingCamera;
  // One button, as before W8d: clicking it opens the menu to pick a camera
  // and to turn it on or off.
  auto camera=CallSquare("camera",Foreground,live?Accent:Default,live?0x3592f9:0x2e2e31,live?L"Câmera ligada":L"Câmera",[]{});
  camera.Name(direct?L"DirectCamera":L"ShareCamera");
  camera.Click([this,direct,weak=make_weak(camera)](auto const&,auto const&){if(auto button=weak.get())CameraMenu(button,direct);});
  return camera;
}
