TextBlock updateLabel{nullptr}; Button updateButton{nullptr}; Border updateFill{nullptr},updateTrack{nullptr};
DispatcherTimer updateTimer{nullptr};
bool updateCallBlocked=false;
TextBlock updateNote{nullptr}; Shapes::Ellipse updateDot{nullptr};
// One row, as the owner asked (22/10/2026): "Versão 13" on the left, the
// state on the right with a dot; the internal native number stays in Diagnóstico.
std::wstring VersionText() {
  auto rootPath=app::installer::InstallRoot();int64_t distribution=0;
  if(!rootPath.empty()) distribution=app::installer::ReadJSON(rootPath/L"current.json")["version"].Int();
  return L"Versão "+std::to_wstring(distribution>0?distribution:int64_t(HARMONY_NATIVE_VERSION));
}
void RefreshUpdate() {
  if(!updateLabel)return;
  auto state=updater_?updater_->Read():app::installer::UpdateState{};
  using Phase=app::installer::UpdatePhase;
  bool inCall=!ReadState().voice.room.empty() || service->CallRead().phase!=app::CallPhase::Idle;
  if(!inCall)updateCallBlocked=false;
  if(updateCallBlocked && state.phase==Phase::Ready)return;
  updateNote.Visibility(Visibility::Collapsed);
  std::wstring text=state.phase==Phase::Downloading?L"Baixando "+std::to_wstring(state.Percent())+L"%":
    state.phase==Phase::Current?L"Atualizado":state.phase==Phase::Checking?L"Verificando…":state.phase==Phase::Failed?L"Não atualizou":L"";
  uint32_t dot=state.phase==Phase::Current?Success:state.phase==Phase::Failed?0xf5a524:Accent;
  updateLabel.Text(text);
  bool ready=state.phase==Phase::Ready;
  updateLabel.Visibility(text.empty()||ready?Visibility::Collapsed:Visibility::Visible);
  updateDot.Fill(Brush(dot));updateDot.Visibility(text.empty()||ready?Visibility::Collapsed:Visibility::Visible);
  updateButton.Visibility(ready?Visibility::Visible:Visibility::Collapsed);
  updateTrack.Visibility(state.phase==Phase::Downloading?Visibility::Visible:Visibility::Collapsed);
  updateFill.Width(std::max(0.0,updateTrack.ActualWidth()*state.Percent()/100.));
}
// What each version brought (changelog.h), newest first, the one running
// marked. The island's ground, 520 wide, a list that scrolls, closed by its x,
// Esc or a click outside, like the other panels.
ContentDialog versionLogDialog{nullptr};
void ShowVersionLog() {
  if(versionLogDialog) return;
  ContentDialog dialog;versionLogDialog=dialog;dialog.Name(L"VersionLogDialog");dialog.XamlRoot(root.XamlRoot());dialog.RequestedTheme(ElementTheme::Dark);
  dialog.Background(Brush(Surface));dialog.BorderThickness(Thickness{0});dialog.CornerRadius(CornerRadius{12,12,12,12});
  dialog.Resources().Insert(box_value(L"ContentDialogMaxWidth"),box_value(520.0));
  dialog.Resources().Insert(box_value(L"ContentDialogMinWidth"),box_value(520.0));
  dialog.Resources().Insert(box_value(L"ContentDialogMaxHeight"),box_value(640.0));
  dialog.Resources().Insert(box_value(L"ContentDialogBorderWidth"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogBackground"),Brush(Surface));
  dialog.Resources().Insert(box_value(L"ContentDialogSmokeFill"),Tint(0,.7));
  dialog.Resources().Insert(box_value(L"ContentDialogSeparatorThickness"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogTopOverlay"),Tint(0,0));
  dialog.Resources().Insert(box_value(L"ContentDialogPadding"),box_value(Thickness{20,20,20,20}));
  Grid title;title.ColumnSpacing(12);title.Width(480); // the dialog does not stretch its title: 520 less 20+20
  for(auto w:{GridLength{1,GridUnitType::Star},GridLength{0,GridUnitType::Auto}}){ColumnDefinition c;c.Width(w);title.ColumnDefinitions().Append(c);}
  auto heading=Line(L"O que mudou",20,Foreground,600);heading.VerticalAlignment(VerticalAlignment::Center);title.Children().Append(heading);
  auto close=Square("xmark",L"Fechar",32,18,Muted,Default,[this]{if(versionLogDialog)versionLogDialog.Hide();});close.Name(L"VersionLogClose");
  close.VerticalAlignment(VerticalAlignment::Center);Grid::SetColumn(close,1);title.Children().Append(close);
  dialog.Title(title);
  auto current=std::stoi(VersionText().substr(7));
  StackPanel list;list.Spacing(20);list.Padding(Thickness{0,0,16,0});
  for(auto const& entry:Changelog()) {
    if(entry.version>current) continue;
    StackPanel block;block.Spacing(8);
    StackPanel head;head.Orientation(Orientation::Horizontal);head.Spacing(8);
    auto name=Line(L"Versão "+std::to_wstring(entry.version),15,Foreground,600);name.VerticalAlignment(VerticalAlignment::Center);head.Children().Append(name);
    auto when=Line(entry.date,13,Muted,500);when.VerticalAlignment(VerticalAlignment::Center);head.Children().Append(when);
    if(entry.version==current) {
      Border chip;chip.Height(22);chip.Padding(Thickness{8,0,8,0});chip.CornerRadius(CornerRadius{8,8,8,8});chip.Background(Tint(Accent,.2));chip.VerticalAlignment(VerticalAlignment::Center);
      auto here=Line(L"a sua",12,Accent,600);here.VerticalAlignment(VerticalAlignment::Center);chip.Child(here);head.Children().Append(chip);
    }
    block.Children().Append(head);
    for(auto item:entry.items) {
      Grid line;line.ColumnSpacing(8);
      for(auto w:{GridLength{12,GridUnitType::Pixel},GridLength{1,GridUnitType::Star}}){ColumnDefinition c;c.Width(w);line.ColumnDefinitions().Append(c);}
      Shapes::Ellipse dot;dot.Width(4);dot.Height(4);dot.Fill(Brush(Muted));dot.VerticalAlignment(VerticalAlignment::Top);dot.Margin(Thickness{4,8,4,0});line.Children().Append(dot);
      auto words=Text(item,13,Foreground);words.TextWrapping(TextWrapping::Wrap);Grid::SetColumn(words,1);line.Children().Append(words);
      block.Children().Append(line);
    }
    list.Children().Append(block);
  }
  ScrollViewer scroll;scroll.Name(L"VersionLogList");scroll.MaxHeight(480);scroll.Content(list);ImmediateMouseWheel(scroll);
  dialog.Content(scroll);
  DismissOnOutsideClick(dialog);
  auto op=dialog.ShowAsync();
  op.Completed([this](auto const&,auto){versionLogDialog=nullptr;});
}
Border VersionIsland() {
  auto island=Island();island.Name(L"VersionIsland");island.Margin(Thickness{0,0,8,8});island.Padding(Thickness{12,8,8,8});
  StackPanel body;body.Spacing(8);
  // 32 as the account's row: with the 8+8 padding the island is the 48 of
  // the account and composer islands beside it (the owner, 25/10/2026).
  Grid row;row.MinHeight(32);
  ColumnDefinition left;left.Width(GridLength{1,GridUnitType::Star});row.ColumnDefinitions().Append(left);
  ColumnDefinition right;right.Width(GridLength{0,GridUnitType::Auto});row.ColumnDefinitions().Append(right);
  // "Versão N" opens what changed (the owner, 25/10/2026): a plain word, the
  // link's underline under the pointer, as the web's text buttons.
  HyperlinkButton version;version.Name(L"VersionLog");HarmonyPointer(version);version.Padding(Thickness{0,0,0,0});version.MinHeight(0);version.MinWidth(0);
  version.VerticalAlignment(VerticalAlignment::Center);version.Background(Brush(Surface));version.BorderThickness(Thickness{0});
  for(auto key:{L"HyperlinkButtonBackgroundPointerOver",L"HyperlinkButtonBackgroundPressed"}) version.Resources().Insert(box_value(key),Brush(Surface));
  auto versionText=Text(VersionText(),13,Foreground);version.Content(versionText);
  version.PointerEntered([versionText](auto const&,auto const&){versionText.TextDecorations(winrt::Windows::UI::Text::TextDecorations::Underline);});
  version.PointerExited([versionText](auto const&,auto const&){versionText.TextDecorations(winrt::Windows::UI::Text::TextDecorations::None);});
  Automation::AutomationProperties::SetName(version,VersionText()+L", ver o que mudou em cada versão");ToolTipService::SetToolTip(version,box_value(L"O que mudou em cada versão"));
  version.Click([this](auto const&,auto const&){if(!exiting)ShowVersionLog();});
  row.Children().Append(version);
  StackPanel state;state.Orientation(Orientation::Horizontal);state.Spacing(6);state.VerticalAlignment(VerticalAlignment::Center);Grid::SetColumn(state,1);
  updateDot=Shapes::Ellipse();updateDot.Width(6);updateDot.Height(6);updateDot.VerticalAlignment(VerticalAlignment::Center);state.Children().Append(updateDot);
  updateLabel=Text(L"",13,Muted);updateLabel.Name(L"UpdateStatus");updateLabel.VerticalAlignment(VerticalAlignment::Center);state.Children().Append(updateLabel);
  updateButton=Action(L"Atualizar",[this]{
    if(!ReadState().voice.room.empty() || service->CallRead().phase!=app::CallPhase::Idle) {
      updateCallBlocked=true;updateNote.Text(L"Saia da chamada para atualizar, ou feche o harmony e ele atualiza ao abrir.");updateNote.Visibility(Visibility::Visible);return;
    }
    if(!updater_ || updater_->Read().phase!=app::installer::UpdatePhase::Ready)return;
    if(app::installer::ScheduleApply()) Quit();else {updateNote.Text(L"Não foi possível reiniciar. Feche e abra o harmony.");updateNote.Visibility(Visibility::Visible);}
  });
  updateButton.Name(L"ApplyUpdate");updateButton.Style(Resources().Lookup(box_value(L"HarmonyPrimaryButton")).as<Style>());
  updateButton.Height(28);updateButton.MinHeight(28);updateButton.Padding(Thickness{12,0,12,0});updateButton.FontSize(13);
  state.Children().Append(updateButton);row.Children().Append(state);body.Children().Append(row);
  updateTrack=Border();updateTrack.Height(4);updateTrack.Background(Brush(Default));updateTrack.CornerRadius(CornerRadius{2,2,2,2});
  updateFill=Border();updateFill.Height(4);updateFill.HorizontalAlignment(HorizontalAlignment::Left);updateFill.Background(Brush(Accent));updateFill.CornerRadius(CornerRadius{2,2,2,2});
  updateTrack.Child(updateFill);updateTrack.SizeChanged([this](auto const&,auto const&){RefreshUpdate();});body.Children().Append(updateTrack);
  updateNote=Text(L"",13,Muted);updateNote.Visibility(Visibility::Collapsed);body.Children().Append(updateNote);
  island.Child(body);RefreshUpdate();
  if(!updateTimer) {updateTimer=DispatcherTimer();updateTimer.Interval(std::chrono::seconds(1));updateTimer.Tick([this](auto const&,auto const&){if(!exiting && updateLabel && updateLabel.IsLoaded())RefreshUpdate();});updateTimer.Start();}
  return island;
}
