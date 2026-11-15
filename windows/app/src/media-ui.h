// Included inside App: uses the W3 web token helpers and its UI dispatcher.
Image PreviewControl(app::PreviewImage const& pixels) {
  Image image;
  if(pixels.pixels.empty()) return image;
  Media::Imaging::WriteableBitmap bitmap(pixels.width,pixels.height);
  auto access=bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
  BYTE* data=nullptr; check_hresult(access->Buffer(&data)); memcpy(data,pixels.pixels.data(),pixels.pixels.size()); bitmap.Invalidate(); image.Source(bitmap); return image;
}
// The pictures in the chooser. They used to come one at a time with two
// seconds between them, so the tenth window had a picture after twenty: now
// four captures run at once until every block has one, and only then the
// slow round that keeps them fresh. Closing schedules no more captures.
winrt::Windows::Foundation::IAsyncAction PreviewWorker(std::shared_ptr<std::atomic<bool>> open,std::vector<app::ScreenChoice> choices,std::vector<Image> images,std::shared_ptr<std::atomic<size_t>> next) {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  for(;;) {
    size_t i=(*next)++; if(i>=choices.size()||!open->load()) co_return;
    co_await winrt::resume_background();
    if(!open->load()) co_return;
    app::PreviewImage pixels;
    try {pixels=app::Preview(choices[i]);} catch(...) {logf("picker thumbnail unavailable");}
    co_await ui; if(!open->load()) co_return;
    try {if(!pixels.pixels.empty()) images[i].Source(PreviewControl(pixels).Source());} catch(...) {logf("picker thumbnail not shown");}
  }
}
winrt::Windows::Foundation::IAsyncAction UpdatePreviews(std::shared_ptr<std::atomic<bool>> open,std::vector<app::ScreenChoice> choices,std::vector<Image> images) {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  auto next=std::make_shared<std::atomic<size_t>>(0);
  // The first pass, four at a time; the slow refresh only once it is done,
  // so there are never more than four captures at once.
  std::vector<winrt::Windows::Foundation::IAsyncAction> first;
  for(int w=0;w<4;++w) first.push_back(PreviewWorker(open,choices,images,next));
  for(auto& f:first) { try {co_await f;} catch(...) {logf("picker thumbnail worker failed");} }
  if(!open->load()) co_return;
  co_await winrt::resume_after(std::chrono::seconds(1)); co_await ui;
  for(size_t i=0;open->load()&&!choices.empty();i=(i+1)%choices.size()) {
    co_await winrt::resume_background();
    if(!open->load()) break;
    app::PreviewImage pixels;
    try {pixels=app::Preview(choices[i]);} catch(...) {logf("picker thumbnail unavailable");}
    co_await ui; if(!open->load()) break;
    try {if(!pixels.pixels.empty()) images[i].Source(PreviewControl(pixels).Source());} catch(...) {logf("picker thumbnail not shown");}
    co_await winrt::resume_after(std::chrono::milliseconds(700));
  }
}
winrt::Windows::Foundation::IAsyncAction activeScreenPreviews{nullptr};
winrt::Windows::Foundation::IAsyncAction QueuePreviews(std::shared_ptr<std::atomic<bool>> open,std::vector<app::ScreenChoice> choices,std::vector<Image> images) {
  auto lifetime=get_strong();winrt::apartment_context ui;
  // A WGC activation can take two minutes to fail. Keep a single batch,
  // without keeping the closed dialog or a later open waiting for cleanup.
  while(open->load() && activeScreenPreviews && activeScreenPreviews.Status()==winrt::Windows::Foundation::AsyncStatus::Started) {
    co_await winrt::resume_after(std::chrono::milliseconds(100));co_await ui;
  }
  if(!open->load()) co_return;
  activeScreenPreviews=UpdatePreviews(open,choices,images);
  try {co_await activeScreenPreviews;}catch(...) {logf("picker preview cleanup failed");}
}
// The same chooser for a room and for a direct call (W6): only where the
// picture goes differs.
bool screenPickerPending=false;
static bool PickerPressOutside(FrameworkElement const& layer,winrt::Windows::Foundation::Point point) {
  auto surface=FindDesignPart(layer,L"BackgroundElement");
  if(!surface) return false;
  auto bounds=surface.TransformToVisual(layer).TransformBounds({0,0,float(surface.ActualWidth()),float(surface.ActualHeight())});
  return point.X<bounds.X || point.Y<bounds.Y || point.X>=bounds.X+bounds.Width || point.Y>=bounds.Y+bounds.Height;
}
winrt::Windows::Foundation::IAsyncAction ScreenPicker(bool direct=false,bool auditEndCall=false) {
  auto lifetime=get_strong();
  if(screenPickerPending) co_return;
  screenPickerPending=true;
  struct ResetPending {bool& value;~ResetPending(){value=false;}} resetPending{screenPickerPending};
  if(direct ? service->CallRead().phase==app::CallPhase::Idle : ReadState().voice.room.empty()) co_return;
  // The call this picker is for: if it ends while the picker is open, the
  // choice goes nowhere (and the worker checks the id again).
  auto callId=service->CallRead().id;
  ContentDialog dialog; dialog.Name(L"ScreenPicker"); dialog.XamlRoot(root.XamlRoot());dialog.RequestedTheme(ElementTheme::Dark);
  dialog.Resources().Insert(box_value(L"ContentDialogBackground"),Brush(0x18181b));
  dialog.Background(Brush(0x18181b));dialog.CornerRadius(CornerRadius{8,8,8,8});dialog.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));dialog.FontSize(15);
  dialog.PrimaryButtonText(L"Transmitir"); dialog.CloseButtonText(L"Cancelar"); dialog.IsPrimaryButtonEnabled(false); dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.Shadow(nullptr); dialog.BorderBrush(nullptr); dialog.BorderThickness(Thickness{0});
  dialog.PrimaryButtonStyle(Resources().Lookup(box_value(L"HarmonyPrimaryButton")).as<Style>());
  dialog.CloseButtonStyle(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
  // Scope theme resources to this dialog, including the popup items. Native
  // keyboard navigation and automation peers stay intact.
  auto resource=[&](wchar_t const* key,uint32_t color){dialog.Resources().Insert(box_value(key),Brush(color));};
  for(auto key:{L"ComboBoxBackground",L"ComboBoxBackgroundUnfocused",L"ComboBoxBackgroundFocused",L"ComboBoxDropDownBackground"})resource(key,0x232325);
  for(auto key:{L"ComboBoxBackgroundPointerOver",L"ComboBoxItemBackgroundPointerOver",L"ComboBoxItemBackgroundSelected",L"ComboBoxItemBackgroundSelectedUnfocused"})resource(key,0x2e2e31);
  for(auto key:{L"ComboBoxBackgroundPressed",L"ComboBoxItemBackgroundSelectedPointerOver",L"ComboBoxItemBackgroundSelectedPressed"})resource(key,0x323235);
  for(auto key:{L"ComboBoxBorderBrush",L"ComboBoxBorderBrushPointerOver",L"ComboBoxDropDownBorderBrush"})resource(key,0x3f3f46);
  for(auto key:{L"ComboBoxBorderBrushPressed",L"ComboBoxBackgroundBorderBrushFocused",L"ComboBoxItemPillFillBrush"})resource(key,0x0485f7);
  for(auto key:{L"ComboBoxForeground",L"ComboBoxForegroundPointerOver",L"ComboBoxForegroundPressed",L"ComboBoxForegroundFocused",L"ComboBoxDropDownForeground"})resource(key,0xfcfcfc);
  for(auto suffix:{L"",L"PointerOver",L"Pressed"}) {
    auto put=[&](wchar_t const* key,uint32_t color){auto name=std::wstring(key)+suffix;resource(name.c_str(),color);};
    put(L"CheckBoxCheckBackgroundFillChecked",suffix==std::wstring(L"Pressed")?0x0370d0:0x0485f7);
    put(L"CheckBoxCheckBackgroundStrokeChecked",0x0485f7);
    put(L"CheckBoxCheckGlyphForegroundChecked",0xfcfcfc);
    put(L"CheckBoxCheckBackgroundFillUnchecked",0x232325);
    put(L"CheckBoxCheckBackgroundStrokeUnchecked",0x52525b);
    put(L"CheckBoxForegroundChecked",0xfcfcfc);put(L"CheckBoxForegroundUnchecked",0xfcfcfc);
  }

  // "Transmitir" in harmony's accent, not Windows' (a pale grey here).
  for(auto [key,color]:{std::pair{L"AccentButtonBackground",0x0485f7u},{L"AccentButtonBackgroundPointerOver",0x3592f9u},{L"AccentButtonBackgroundPressed",0x0370d0u},
      {L"AccentButtonForeground",0xfcfcfcu},{L"AccentButtonForegroundPointerOver",0xfcfcfcu},{L"AccentButtonForegroundPressed",0xfcfcfcu}})
    dialog.Resources().Insert(box_value(key),Brush(color));
  auto selected=std::make_shared<int>(-1); auto choices=app::ScreenSources();
  // Three blocks a row: big enough to tell a game from a browser (18/10/2026)
  // without the two giant pictures the owner called "grande demais" (22/10).
  double width=std::clamp(root.ActualWidth()-64,280.0,760.0);
  int columns=width<520?2:3;
  dialog.Resources().Insert(box_value(L"ContentDialogMaxWidth"),box_value(width+48));
  dialog.Resources().Insert(box_value(L"ContentDialogMaxHeight"),box_value(std::max(240.0,root.ActualHeight()-32)));
  double block=(width-16-12*(columns-1))/columns, picture=std::floor((block-12)*9/16);
  // Rows: the "transmitting now" strip, the tabs, the pictures, the options.
  // No fixed height: the dialog is as tall as what it holds.
  StackPanel content; content.Width(width); content.Spacing(16);
  if(direct ? service->CallRead().screen : ReadState().voice.sharingScreen) {
    Border live; live.Background(Brush(Surface2)); live.CornerRadius(CornerRadius{8,8,8,8}); live.Padding(Thickness{12,8,8,8});
    Grid line; ColumnDefinition say; say.Width(GridLength{1,GridUnitType::Star}); line.ColumnDefinitions().Append(say);
    ColumnDefinition stopColumn; stopColumn.Width(GridLength{0,GridUnitType::Auto}); line.ColumnDefinitions().Append(stopColumn);
    auto words=Line(L"Você já está transmitindo. Escolha outra fonte para trocar.",13,Muted,500); words.VerticalAlignment(VerticalAlignment::Center); words.TextWrapping(TextWrapping::Wrap); line.Children().Append(words);
    auto stop=Action(L"Parar transmissão",[this,dialog,direct]{if(direct)service->CallStopScreen();else service->StopScreen();dialog.Hide();});
    stop.Name(L"ScreenStop"); stop.Height(32); stop.MinHeight(32); stop.Padding(Thickness{12,0,12,0}); stop.FontSize(13); Grid::SetColumn(stop,1); line.Children().Append(stop);
    live.Child(line); content.Children().Append(live);
  }
  // Janelas | Telas as the column's Servidor | Mensagens: one pill holding two.
  Border tabsPill; tabsPill.Background(Brush(Surface2)); tabsPill.CornerRadius(CornerRadius{8,8,8,8}); tabsPill.Padding(Thickness{4,4,4,4}); tabsPill.HorizontalAlignment(HorizontalAlignment::Left);
  StackPanel tabs; tabs.Orientation(Orientation::Horizontal); tabs.Spacing(4); tabsPill.Child(tabs);
  auto tabButton=[&](wchar_t const* label) {
    Button b; HarmonyPointer(b); b.Style(Resources().Lookup(box_value(L"HarmonyButton")).as<Style>());
    b.Width(104); b.Height(32); b.MinHeight(0); b.MinWidth(0); b.Padding(Thickness{0,0,0,0}); b.CornerRadius(CornerRadius{6,6,6,6}); b.BorderThickness(Thickness{0,0,0,0});
    b.Content(box_value(label)); b.FontSize(14); b.FontWeight(winrt::Windows::UI::Text::FontWeight{500}); return b;
  };
  auto windowsTab=tabButton(L"Janelas"),monitorsTab=tabButton(L"Telas");
  windowsTab.Name(L"ScreenWindowsTab"); monitorsTab.Name(L"ScreenMonitorsTab");
  tabs.Children().Append(windowsTab); tabs.Children().Append(monitorsTab);
  // The heading holds the title, the tabs beside it and the X: the tabs used
  // to take a row of their own under an empty stretch (the owner, 22/10/2026).
  Grid header; header.Width(width); header.ColumnSpacing(24);
  for(auto w:{GridLength{0,GridUnitType::Auto},GridLength{1,GridUnitType::Star},GridLength{0,GridUnitType::Auto}}) {ColumnDefinition c; c.Width(w); header.ColumnDefinitions().Append(c);}
  auto heading=Line(L"Compartilhar tela",20,Foreground,600); heading.VerticalAlignment(VerticalAlignment::Center); header.Children().Append(heading);
  tabsPill.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(tabsPill,1); header.Children().Append(tabsPill);
  auto closeX=Square("xmark",L"Fechar",32,16,Foreground,Surface2,[weak=make_weak(dialog)]{if(auto dialog=weak.get())dialog.Hide();}); closeX.Name(L"ScreenPickerClose"); closeX.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(closeX,2); header.Children().Append(closeX);
  dialog.Title(header);
  auto pages=std::make_shared<std::vector<Grid>>();
  StackPanel sections; sections.Spacing(12); std::vector<Image> images;
  auto previous=std::make_shared<winrt::weak_ref<Button>>();
  images.resize(choices.size());
  for(bool monitors:{false,true}) {
    std::vector<size_t> these; for(size_t i=0;i<choices.size();++i) if(bool(choices[i].monitor)==monitors) these.push_back(i);

    Grid grid; grid.ColumnSpacing(12); grid.RowSpacing(12);
    for(int c=0;c<columns;++c) {ColumnDefinition column; column.Width(GridLength{1,GridUnitType::Star}); grid.ColumnDefinitions().Append(column);}
    for(size_t k=0;k<these.size();++k) {
      if(k%columns==0) {RowDefinition row; row.Height(GridLength{1,GridUnitType::Auto}); grid.RowDefinitions().Append(row);}
      auto i=these[k]; auto c=choices[i];
      StackPanel inside; inside.Spacing(8);
      Border frame; frame.Height(picture); frame.CornerRadius(CornerRadius{6,6,6,6}); frame.Background(Brush(0x0f0f11));
      images[i]=Image(); images[i].Stretch(Stretch::Uniform); frame.Child(images[i]); inside.Children().Append(frame);
      Grid caption; caption.ColumnSpacing(8);
      ColumnDefinition iconColumn; iconColumn.Width(GridLength{1,GridUnitType::Auto}); caption.ColumnDefinitions().Append(iconColumn);
      ColumnDefinition textColumn; textColumn.Width(GridLength{1,GridUnitType::Star}); caption.ColumnDefinitions().Append(textColumn);
      if(c.window) {auto icon=PreviewControl(app::WindowIcon(c.window));icon.Width(16);icon.Height(16);icon.VerticalAlignment(VerticalAlignment::Top);icon.Margin(Thickness{0,1,0,0});caption.Children().Append(icon);}
      StackPanel words; words.Spacing(2); Grid::SetColumn(words,1);
      auto title=Line(c.title,13,Foreground,500); title.TextTrimming(TextTrimming::CharacterEllipsis); title.TextWrapping(TextWrapping::NoWrap); words.Children().Append(title);
      words.Children().Append(Line(std::to_wstring(c.width)+L" × "+std::to_wstring(c.height)+(c.minimized?L" · minimizada":L"")+(c.primary?L" · principal":L""),13,Muted,500));
      caption.Children().Append(words); inside.Children().Append(caption);
      auto button=Action(L"",[]{});
      button.Style(Resources().Lookup(box_value(L"HarmonyCaptureCard")).as<Style>());
      button.Name(L"CaptureSource"+std::to_wstring(i));
      // Chosen is an accent edge, not a blue block: the picture stays readable.
      button.Click([selected,i,weakDialog=make_weak(dialog),previous,weak=make_weak(button)](auto const&,auto const&){if(auto old=previous->get())old.BorderBrush(Brush(0x232325));if(auto current=weak.get()) {current.BorderBrush(Brush(0x0485f7));*previous=weak;}*selected=int(i);if(auto dialog=weakDialog.get())dialog.IsPrimaryButtonEnabled(true);});
      // Action() is a 40-point button: this one is as tall as its picture.
      button.Height(std::numeric_limits<double>::quiet_NaN()); button.MinHeight(0);
      button.Content(inside); button.HorizontalAlignment(HorizontalAlignment::Stretch); button.HorizontalContentAlignment(HorizontalAlignment::Stretch); button.VerticalContentAlignment(VerticalAlignment::Top);
      button.Background(Brush(0x232325)); button.BorderBrush(Brush(0x232325)); button.BorderThickness(Thickness{2,2,2,2}); button.CornerRadius(CornerRadius{8,8,8,8}); button.Padding(Thickness{6,6,6,8});
      Grid::SetRow(button,int(k/columns)); Grid::SetColumn(button,int(k%columns)); grid.Children().Append(button);
    }
    if(these.empty()) grid.Children().Append(Text(monitors?L"Nenhuma tela disponível.":L"Nenhuma janela disponível.",15,Muted));
    grid.Visibility(monitors?Visibility::Collapsed:Visibility::Visible); pages->push_back(grid);
    sections.Children().Append(grid);
  }
  // The scroll bar has its own lane (the right padding) instead of lying
  // over the right column of pictures.
  // At most two rows of pictures before it scrolls, and never taller than
  // the window leaves room for.
  double card=picture+6+8+8+38, rowsTall=2*card+12;
  ScrollViewer sources; sources.VerticalScrollBarVisibility(ScrollBarVisibility::Auto); sources.Name(L"CaptureSourcesScroll"); sources.Content(sections);
  sources.MaxHeight(std::max(card,std::min(rowsTall,root.ActualHeight()-380))); ImmediateMouseWheel(sources);
  Grid::SetRow(sources,2); content.Children().Append(sources);
  auto selectTab=[pages,weakWindows=make_weak(windowsTab),weakMonitors=make_weak(monitorsTab),selected,weakDialog=make_weak(dialog),previous,sources](bool monitors) {
    auto windowsTab=weakWindows.get();auto monitorsTab=weakMonitors.get();auto dialog=weakDialog.get();
    if(!windowsTab || !monitorsTab || !dialog)return;
    for(size_t i=0;i<pages->size();++i) (*pages)[i].Visibility(i==size_t(monitors)?Visibility::Visible:Visibility::Collapsed);
    windowsTab.Background(monitors?Tint(0,0):Brush(Default)); monitorsTab.Background(monitors?Brush(Default):Tint(0,0));
    windowsTab.Foreground(Brush(monitors?Muted:Foreground)); monitorsTab.Foreground(Brush(monitors?Foreground:Muted));
    Automation::AutomationProperties::SetHelpText(windowsTab,monitors?L"":L"Selecionada");
    Automation::AutomationProperties::SetHelpText(monitorsTab,monitors?L"Selecionada":L"");
    if(auto old=previous->get()) old.BorderBrush(Brush(Surface2));
    *previous={}; *selected=-1; dialog.IsPrimaryButtonEnabled(false); sources.ChangeView(nullptr,0.0,nullptr,true);
  };
  windowsTab.Click([selectTab](auto const&,auto const&){selectTab(false);}); monitorsTab.Click([selectTab](auto const&,auto const&){selectTab(true);}); selectTab(false);
  ComboBox resolution,fps; for(auto label:{L"Automática (1080p)",L"1080p",L"720p",L"480p"}) resolution.Items().Append(box_value(label));
  for(auto label:{L"Automática (30 quadros)",L"60 quadros",L"30 quadros",L"15 quadros"}) fps.Items().Append(box_value(label));
  resolution.Name(L"ScreenResolution");fps.Name(L"ScreenFrameRate");
  for(auto box:{resolution,fps}) {box.SelectedIndex(0);box.HorizontalAlignment(HorizontalAlignment::Stretch);box.Height(40);box.Background(Brush(0x232325));box.BorderBrush(Brush(0x3f3f46));box.BorderThickness(Thickness{1,1,1,1});box.Padding(Thickness{12,0,12,0});box.CornerRadius(CornerRadius{8,8,8,8});box.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));box.FontSize(15);}
  // The options under the pictures: two choices side by side, then the sound
  // on its own line with its one caveat shown only while it applies.
  StackPanel optionsBlock; optionsBlock.Spacing(12); Grid::SetRow(optionsBlock,3);
  Grid options; options.ColumnSpacing(12);
  for(int c=0;c<2;++c) {ColumnDefinition column; column.Width(GridLength{1,GridUnitType::Star}); options.ColumnDefinitions().Append(column);}
  auto labelled=[&](wchar_t const* label,FrameworkElement const& control,int column) {StackPanel group; group.Spacing(6); group.Children().Append(Line(label,13,Muted,500)); group.Children().Append(control); Grid::SetColumn(group,column); options.Children().Append(group);};
  labelled(L"Resolução",resolution,0); labelled(L"Quadros por segundo",fps,1);
  optionsBlock.Children().Append(options);
  CheckBox sound; sound.Style(Resources().Lookup(box_value(L"HarmonyCheckBox")).as<Style>()); sound.Name(L"ScreenSound");sound.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));sound.FontSize(15);sound.MinHeight(32);sound.Content(box_value(L"Compartilhar som")); sound.IsChecked(true);
  StackPanel soundBlock; soundBlock.Spacing(2); soundBlock.Children().Append(sound);
  auto caveat=Line(L"Som de navegador só entra transmitindo a tela inteira. A voz da chamada nunca entra.",13,Muted,500); caveat.TextWrapping(TextWrapping::Wrap); caveat.Margin(Thickness{28,0,0,0});
  soundBlock.Children().Append(caveat);
  sound.Checked([caveat](auto const&,auto const&){caveat.Visibility(Visibility::Visible);}); sound.Unchecked([caveat](auto const&,auto const&){caveat.Visibility(Visibility::Collapsed);});
  content.Children().Append(optionsBlock);
  dialog.Content(content);
  auto open=std::make_shared<std::atomic<bool>>(true); dialog.Closing([open](auto const&,auto const&){*open=false;}); dialog.Closed([open](auto const&,auto const&){*open=false;});
  struct StopPreviewsOnExit {
    std::shared_ptr<std::atomic<bool>> open;ContentDialog dialog;
    ~StopPreviewsOnExit() {if(open->exchange(false)) {try {dialog.Hide();}catch(...) {}}}
  } stopPreviews{open,dialog};
  auto previews=QueuePreviews(open,choices,images);
  winrt::apartment_context ui;
  auto configured=std::make_shared<bool>(false);
  content.Loaded([this,weak=make_weak(dialog),configured](auto const& sender,auto const&) {
    auto shown=weak.get();if(!shown || *configured) return;
    // The content can be reparented into a popup: on repeated opens the
    // dialog itself need not own the live template's visual children.
    auto layout=FindAncestor(sender.template as<FrameworkElement>(),L"LayoutRoot").template try_as<Grid>();
    if(!layout) layout=FindDesignPart(shown,L"LayoutRoot").template try_as<Grid>();
    if(!layout) {logf("picker live layout unavailable");shown.Hide();return;}
    *configured=true;
    if(auto surface=FindDesignPart(layout,L"BackgroundElement").template try_as<Border>()) { surface.Shadow(nullptr); surface.BorderBrush(nullptr); surface.BorderThickness(Thickness{0}); }
    layout.Background(Tint(0,.7));
    layout.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([weak=make_weak(shown)](auto const& sender,Input::PointerRoutedEventArgs const& e) {
      if(auto dialog=weak.get()) {
        auto layer=sender.template as<FrameworkElement>();
        if(PickerPressOutside(layer,e.GetCurrentPoint(layer).Position())) dialog.Hide();
      }
    })),true);
    if(auto scroll=FindDesignPart(layout,L"ContentScrollViewer").template try_as<ScrollViewer>()) { scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled); scroll.VerticalScrollMode(ScrollMode::Disabled); }
  });
  // Poll the identity as well as the phase; a subsequent call cannot inherit a chooser.
  DispatcherTimer callWatch; callWatch.Interval(std::chrono::milliseconds(100));
  auto voiceRoom=ReadState().voice.room;
  callWatch.Tick([this,dialog,direct,callId,voiceRoom](auto const&,auto const&) {
    if(exiting || !service) {dialog.Hide();return;}
    if(direct ? service->CallRead().id!=callId || service->CallRead().phase==app::CallPhase::Idle : ReadState().voice.room!=voiceRoom || ReadState().voice.reconnecting) dialog.Hide();
  });
  callWatch.Start();
  auto operation=dialog.ShowAsync();
  // Opened may fire before the template's button is attached. Resolve it
  // after layout, and tolerate a dialog dismissed before that happens.
  FrameworkElement primaryPart{nullptr};
  for(int attempt=0;attempt<20&&open->load();++attempt) {
    co_await winrt::resume_after(std::chrono::milliseconds(50));co_await ui;
    primaryPart=FindDesignPart(dialog,L"PrimaryButton");
    if(primaryPart)break;
  }
  logf("picker primary harmony template_ready=%d",bool(primaryPart));
  if(primaryPart) {
    auto button=primaryPart.as<Button>();
    button.Style(Resources().Lookup(box_value(L"HarmonyPrimaryButton")).as<Style>());
    // Local values survive WinUI's PrimaryAsDefault style replacement.
    button.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));button.FontSize(13);
    button.FontWeight(winrt::Windows::UI::Text::FontWeight{600});button.CharacterSpacing(-20);
    button.Height(44);button.MinHeight(44);button.Padding(Thickness{16,0,16,0});
    button.CornerRadius(CornerRadius{8,8,8,8});button.BorderThickness(Thickness{0});
    button.Foreground(Brush(0xfcfcfc));button.Background(Brush(0x0485f7));
    // The sound choice fills the empty left of the buttons' row.
    if(auto commands=FindDesignPart(dialog,L"CommandSpace").try_as<Grid>()) {
      soundBlock.VerticalAlignment(VerticalAlignment::Center); soundBlock.HorizontalAlignment(HorizontalAlignment::Left); soundBlock.MaxWidth(std::max(160.0,width-280));
      // Sound | Cancelar | Transmitir: the left column takes the room left.
      commands.HorizontalAlignment(HorizontalAlignment::Stretch); commands.Margin(Thickness{0,0,0,0});
      auto close=FindDesignPart(dialog,L"CloseButton").try_as<Button>();
      Grid::SetColumn(button,4); Grid::SetColumnSpan(button,1); if(close) {Grid::SetColumn(close,2); Grid::SetColumnSpan(close,1); close.HorizontalAlignment(HorizontalAlignment::Stretch);}
      button.HorizontalAlignment(HorizontalAlignment::Stretch);
      for(uint32_t i=0;i<commands.ColumnDefinitions().Size();++i)
        commands.ColumnDefinitions().GetAt(i).Width(i==0?GridLength{1,GridUnitType::Star}:i==1?GridLength{16,GridUnitType::Pixel}:i==3?GridLength{8,GridUnitType::Pixel}:GridLength{0,GridUnitType::Auto});
      Grid::SetColumn(soundBlock,0); Grid::SetColumnSpan(soundBlock,1); commands.Children().Append(soundBlock);
    }
  }
#ifndef HARMONY_PUBLISH
  if(app::Environment(L"HARMONY_PARITY7C")=="1") {
    co_await winrt::resume_after(std::chrono::milliseconds(700)); co_await ui;
    auto box=FindDesignPart(sound,L"Box");
    ParityRequire(box.ActualWidth()==20 && box.ActualHeight()==20);
    auto layout=Media::VisualTreeHelper::GetParent(box).as<Grid>();
    ParityRequire(layout.ColumnSpacing()==8);
    co_await SaveDesignSurface(dialog,designOutput,"round7c-screen-picker");
    co_await SaveDesignSurface(sound,designOutput,"round7c-checkbox-checked");
    sound.IsChecked(false); co_await SaveDesignSurface(sound,designOutput,"round7c-checkbox-unchecked");
    dialog.Hide();
  }
  if(screenDesign) {
    co_await winrt::resume_after(std::chrono::milliseconds(1500));co_await ui;
    co_await SaveDesignSurface(dialog,designOutput,"nativo-seletor");
    ParityRequire((*pages)[0].Visibility()==Visibility::Visible && (*pages)[1].Visibility()==Visibility::Collapsed);
    co_await SaveDesignSurface(dialog,designOutput,"round7-windows-tab");
    selectTab(true); co_await winrt::resume_after(std::chrono::milliseconds(150));co_await ui;
    ParityRequire((*pages)[0].Visibility()==Visibility::Collapsed && (*pages)[1].Visibility()==Visibility::Visible);
    co_await SaveDesignSurface(dialog,designOutput,"round7-monitors-tab");
    selectTab(false);co_await winrt::resume_after(std::chrono::milliseconds(150));co_await ui;
    size_t first=0; for(size_t i=0;i<choices.size();++i) if(choices[i].window) {first=i;break;}
    if(!choices.empty()) {auto button=FindDesignPart(dialog,L"CaptureSource"+std::to_wstring(first)).as<Button>();Automation::Peers::ButtonAutomationPeer(button).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;logf("picker primary enabled=%d",int(dialog.IsPrimaryButtonEnabled()));co_await SaveDesignSurface(dialog,designOutput,"nativo-seletor-selecionado");}
    ChatAuditPosition();
    if(primaryPart) {
      VisualStateManager::GoToState(primaryPart.as<Button>(),L"PointerOver",false); co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
      co_await SaveDesignSurface(dialog,designOutput,"round6-transmit-hover");
      co_await SaveDesignWindow(hwnd,designOutput,L"round6-picker-window.png");
      auto fill=FindDesignPart(primaryPart,L"Surface").as<Border>().Background().as<SolidColorBrush>().Color();
      ParityRequire(fill.B>fill.R+80);
    }
    auto outer=FindDesignPart(dialog,L"ContentScrollViewer").as<ScrollViewer>();
    ParityRequire(outer.VerticalScrollMode()==ScrollMode::Disabled && outer.ScrollableHeight()<1);
    auto sourceBottom=sources.TransformToVisual(nullptr).TransformPoint({0,float(sources.ActualHeight())});
    auto optionsTop=options.TransformToVisual(nullptr).TransformPoint({0,0});
    auto optionsBottom=options.TransformToVisual(nullptr).TransformPoint({0,float(options.ActualHeight())});
    auto primaryTop=primaryPart.TransformToVisual(nullptr).TransformPoint({0,0});
    ParityRequire(sources.ActualHeight()>0 && sourceBottom.Y<=optionsTop.Y && optionsBottom.Y<=primaryTop.Y);
    logf("round6 picker client %.0fx%.0f sources bottom %.2f options %.2f..%.2f primary %.2f",root.ActualWidth(),root.ActualHeight(),sourceBottom.Y,optionsTop.Y,optionsBottom.Y,primaryTop.Y);
    for(double portion:{0.0,0.5,1.0}) {
      sources.ChangeView(nullptr,sources.ScrollableHeight()*portion,nullptr,true);
      co_await winrt::resume_after(std::chrono::milliseconds(150)); co_await ui;
      ParityRequire(std::abs(sources.VerticalOffset()-sources.ScrollableHeight()*portion)<1);
      co_await SaveDesignSurface(dialog,designOutput,portion==0?"round6-sources-top":portion==1?"round6-sources-bottom":"round6-sources-middle");
    }
    if(!choices.empty()) {
      size_t lastIndex=first;for(size_t i=0;i<choices.size();++i)if(choices[i].window)lastIndex=i;
      auto last=FindDesignPart(dialog,L"CaptureSource"+std::to_wstring(lastIndex));
      auto p=last.TransformToVisual(sources).TransformPoint({0,float(last.ActualHeight())});
      ParityRequire(p.Y<=sources.ActualHeight()+1);
      auto surface=FindDesignPart(last,L"Surface");
      ParityRequire(std::abs(surface.ActualWidth()-last.ActualWidth())<1 && std::abs(surface.ActualHeight()-last.ActualHeight())<1);
    }
    for(auto box:{resolution,fps}) {
      box.IsDropDownOpen(true);co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
      for(auto popup:Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
        if(auto surface=popup.Child().try_as<FrameworkElement>();surface&&surface.Name()!=L"SmokeLayerBackground")
          co_await SaveDesignSurface(surface,designOutput,box==resolution?"nativo-seletor-resolucoes":"nativo-seletor-quadros");
      box.IsDropDownOpen(false);
    }
    sound.IsChecked(false);VisualStateManager::GoToState(sound,L"PointerOver",false);
    co_await SaveDesignSurface(dialog,designOutput,"nativo-seletor-sem-som");
    if(auditEndCall) designState.voice.room.clear();
    else {
      auto layer=FindAncestor(FindDesignPart(dialog,L"Title"),L"LayoutRoot").as<FrameworkElement>();
      auto surface=FindDesignPart(layer,L"BackgroundElement");
      ParityRequire(!PickerPressOutside(layer,surface.TransformToVisual(layer).TransformPoint({float(surface.ActualWidth()/2),float(surface.ActualHeight()/2)})));
      ParityRequire(PickerPressOutside(layer,{4,100}));
      dialog.Hide();
    }
    co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
    ParityRequire(!open->load());
    co_await SaveDesignSurface(root,designOutput,auditEndCall?"round6-picker-call-ended":"round6-picker-click-outside");
  }
#endif
  auto result=co_await operation; callWatch.Stop(); *open=false;
  // QueuePreviews retains its own lifetime and serializes batches. Closing
  // and reopening remain responsive even while an OS activation times out.
  if(result==ContentDialogResult::Primary&&*selected>=0) {
    if(exiting || !service || (!direct && (ReadState().voice.room!=voiceRoom || ReadState().voice.reconnecting))) co_return;
    // Exclusion must be successfully applied before a monitor can include us.
    if(!TransmissionNotice(true)) {if(direct)service->CallStopScreen();else service->StopScreen();co_return;}
    transmissionPendingUntil=GetTickCount64()+15000;
    auto choice=choices[*selected]; int heights[]={1080,1080,720,480},rates[]={30,60,30,15};
    bool withSound=sound.IsChecked()&&sound.IsChecked().Value();
    if(direct&&service->CallRead().id!=callId) co_return;
    if(direct) service->CallScreen(callId,choice.window,choice.monitor,heights[resolution.SelectedIndex()],rates[fps.SelectedIndex()],withSound);
    else service->ShareScreen(choice.window,choice.monitor,heights[resolution.SelectedIndex()],rates[fps.SelectedIndex()],withSound);
  }
}
bool TransmissionNotice(bool) {
  if(streamingNotice) DestroyWindow(streamingNotice);
  streamingNotice=nullptr;
  return true;
}
void FullscreenVideo(HWND h) {
  auto style=GetWindowLongPtrW(h,GWL_STYLE);
  if(style&WS_OVERLAPPEDWINDOW) {
    MONITORINFO info{sizeof(info)};GetWindowPlacement(h,&videoPlacement);GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&info);
    SetWindowLongPtrW(h,GWL_STYLE,style&~WS_OVERLAPPEDWINDOW);SetWindowPos(h,HWND_TOP,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top,SWP_FRAMECHANGED);
  } else {SetWindowLongPtrW(h,GWL_STYLE,style|WS_OVERLAPPEDWINDOW);SetWindowPlacement(h,&videoPlacement);SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
}
void OpenVideo(std::string who,bool camera) {
  if(!camera) {viewingScreens.insert(who);callSignature.clear();Render();}
  if(videoWindow) DestroyWindow(videoWindow);
  WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Harmony.Video.W4";cls.style=CS_DBLCLKS;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cls.lpfnWndProc=[](HWND h,UINT m,WPARAM w,LPARAM l)->LRESULT {
    auto self=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE) {self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self) return DefWindowProcW(h,m,w,l);
    if((m==WM_KEYDOWN&&w==VK_F11)||m==WM_LBUTTONDBLCLK||(m==WM_KEYDOWN&&w==VK_ESCAPE&&!(GetWindowLongPtrW(h,GWL_STYLE)&WS_OVERLAPPEDWINDOW))) {self->FullscreenVideo(h);return 0;}
    if(m==WM_CLOSE) {if(self->service&&!self->exiting)self->service->Watch("",nullptr);DestroyWindow(h);self->videoWindow=nullptr;self->videoWho.clear();self->RefreshCoveredTile();return 0;}
    if(m==WM_SIZE||m==WM_SHOWWINDOW||m==WM_ACTIVATE||m==WM_DPICHANGED) {RECT r{};GetClientRect(h,&r);if(self->service&&!self->exiting)self->service->WatchSize(double(r.right),1,!IsIconic(h)&&IsWindowVisible(h));}
    if(m==WM_SIZE||m==WM_SHOWWINDOW||m==WM_EXITSIZEMOVE||m==WM_DISPLAYCHANGE) self->RefreshCoveredTile();
    return DefWindowProcW(h,m,w,l);
  };RegisterClassW(&cls);
  videoWindow=CreateWindowExW(0,cls.lpszClassName,camera?L"Câmera — F11: tela cheia":L"Tela — F11: tela cheia",WS_OVERLAPPEDWINDOW,100,100,1000,650,hwnd,nullptr,cls.hInstance,this);
  videoWho=camera?std::string():who;
  if(videoWindow) {ShowWindow(videoWindow,SW_SHOW);service->Watch(std::move(who),videoWindow,camera);service->WatchSize(1000,1,true);}
}
void RefreshMediaVisibility(bool visible) {if(!service||exiting)return;mediaShown=visible;SyncQualityTimer();diagnosticHistory.Active(visible && settings && settingsTab=="diag");for(auto& [key,update]:mediaVisibility)update(visible);}
// The video window is owned by the main one, so it always sits above it: in
// full screen on the monitor that holds the whole main window, the room's
// tile of the same screen cannot be seen and stops drawing (W11: 4.6k
// presents a run nobody saw). Reception and the big window are untouched.
bool mediaShown=true; std::string coveredScreen;
bool TileCovered(std::string const& who,std::string const& source) const {return source=="screen" && !who.empty() && who==coveredScreen;}
void RefreshCoveredTile() {
  std::string covered;
  if(videoWindow && !videoWho.empty() && IsWindowVisible(videoWindow) && !IsIconic(videoWindow) && !(GetWindowLongPtrW(videoWindow,GWL_STYLE)&WS_OVERLAPPEDWINDOW)) {
    MONITORINFO screen{sizeof(screen)}; RECT main{};
    if(GetMonitorInfoW(MonitorFromWindow(videoWindow,MONITOR_DEFAULTTONEAREST),&screen) && SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&main,sizeof(main)))
       && main.left>=screen.rcMonitor.left && main.top>=screen.rcMonitor.top && main.right<=screen.rcMonitor.right && main.bottom<=screen.rcMonitor.bottom) covered=videoWho;
  }
  if(covered==coveredScreen) return;
  logf("screen tile %s", covered.empty()?"drawing again":"under the full-screen window");
  coveredScreen=std::move(covered); RefreshMediaVisibility(mediaShown);
}
SwapChainPanel MediaPanel(std::string who,std::string source) {
  SwapChainPanel panel;panel.Name(source=="cam"?L"CameraTile":L"ScreenVideo");
  auto renderer=std::make_shared<app::PanelRenderer>(winrt::get_unknown(panel),source=="screen"?app::PanelCadence::Screen:app::PanelCadence::Camera);KeepPicture(panel,renderer);
  service->Video(who,source,[renderer](auto track){renderer->Attach(track);});
  auto measure=[this,renderer,who,source](SwapChainPanel surface) {
    if(!service||exiting)return;
    double dpi=surface.XamlRoot()?surface.XamlRoot().RasterizationScale():1;
    bool visible=!TileCovered(who,source);
    renderer->Size(visible?int(surface.ActualWidth()*dpi):0,visible?int(surface.ActualHeight()*dpi):0);
    if(source=="screen")service->VideoSize(who,surface.ActualWidth(),dpi,visible&&IsWindowVisible(hwnd)&&!IsIconic(hwnd));
  };
  mediaVisibility[who+"/"+source]=[this,weak=make_weak(panel),render=std::weak_ptr<app::PanelRenderer>(renderer),who,source](bool visible) {
    if(!service||exiting)return;auto surface=weak.get();auto draw=render.lock();if(!surface||!draw)return;
    visible=visible&&!TileCovered(who,source);
    double dpi=surface.XamlRoot()?surface.XamlRoot().RasterizationScale():1;
    draw->Size(visible?int(surface.ActualWidth()*dpi):0,visible?int(surface.ActualHeight()*dpi):0);
    if(source=="screen")service->VideoSize(who,surface.ActualWidth(),dpi,visible);
  };
  panel.SizeChanged([measure](auto const& sender,auto const&){measure(sender.template as<SwapChainPanel>());});
  panel.CompositionScaleChanged([measure](auto const& sender,auto const&){measure(sender.template as<SwapChainPanel>());});
  panel.Unloaded([renderer](auto const&,auto const&){renderer->Size(0,0);renderer->Unload();});
  return panel;
}
#ifndef HARMONY_PUBLISH
winrt::Windows::Foundation::IAsyncAction DesignAuditW4() {
  auto auditRoot=designOutput;
  auto lifetime=get_strong();winrt::apartment_context ui;
  auto result=json::Value::Object();result["passed"]=false;
  try {
    co_await winrt::resume_after(std::chrono::milliseconds(500));co_await ui;
    designState.signedIn=true;designState.model.live=true;designState.model.rooms={{"w4-design","geral",{{"design-me","Observador"},{"design-visitor","Visitante",false,false,true,true}}}};
    designState.voice.self="design-me";designState.voice.Join("w4-design","geral");designState.voice.connecting=false;
    // Since W5 the rooms live in the three-column shell, and the call (with its
    // screen and camera buttons) is what opens when the room is chosen.
    showingRooms=false;showingCall=true;callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;co_await SaveDesignSurface(root,designOutput,"nativo-voz-antes");
    ShowVersionLog();co_await winrt::resume_after(std::chrono::milliseconds(600));co_await ui;if(versionLogDialog){co_await SaveDesignSurface(versionLogDialog,designOutput,"nativo-historico-versoes");versionLogDialog.Hide();}co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
    auto screen=FindDesignPart(root,L"ShareScreen").as<Button>();
    VisualStateManager::GoToState(screen,L"PointerOver",false);screen.Content(VoiceIcon("display",0xfcfcfc));
    co_await SaveDesignSurface(root,designOutput,"nativo-tela-hover");
    VisualStateManager::GoToState(screen,L"Normal",false);screen.Content(VoiceIcon("display",0xa1a1aa));
    designState.voice.sharingScreen=true;designState.voice.sharingCamera=true;Render();co_await SaveDesignSurface(root,designOutput,"nativo-transmitindo");
    // Focus (25/10/2026): the screen big, the strip under it, then folded.
    viewingScreens.insert("design-visitor");roomStage.focus="s:design-visitor";callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;co_await SaveDesignSurface(root,designOutput,"nativo-foco");
    roomStage.strip=false;callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;co_await SaveDesignSurface(root,designOutput,"nativo-foco-sem-tira");
    {auto stack=FindDesignPart(root,L"FloatingCameras");logf("design floating: %d cameras at %.0f,%.0f scale %.2f",stack?int(stack.as<Panel>().Children().Size())-8:-1,stack?Canvas::GetLeft(stack):-1.0,stack?Canvas::GetTop(stack):-1.0,floatScale);}
    floatX=0;floatY=0;floatScale=1.6;callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;co_await SaveDesignSurface(root,designOutput,"nativo-flutuantes-movidas");
    roomStage.tucked.insert("p:design-visitor");callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;co_await SaveDesignSurface(root,designOutput,"nativo-flutuante-recolhida");
    roomStage.tucked.clear();floatX=1;floatY=1;floatScale=1;
    // Full screen, on the second monitor (the first is the owner's): the
    // block over the whole monitor, then Esc's path back to focus.
    {MONITORINFO second{sizeof(second)};second.rcWork={0,0,0,0};
     EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR m,HDC,LPRECT,LPARAM data)->BOOL{MONITORINFO info{sizeof(info)};if(!GetMonitorInfoW(m,&info)||(info.dwFlags&MONITORINFOF_PRIMARY))return TRUE;*reinterpret_cast<MONITORINFO*>(data)=info;return FALSE;},reinterpret_cast<LPARAM>(&second));
     if(second.rcWork.right>second.rcWork.left) {
       window.AppWindow().Move({second.rcWork.left+32,second.rcWork.top+32});co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
       roomStage.strip=true;EnterCallFull(roomStage, "s:design-visitor");co_await winrt::resume_after(std::chrono::milliseconds(800));co_await ui;
       RECT r{};GetWindowRect(hwnd,&r);MONITORINFO on{sizeof(on)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&on);
       logf("design full screen: window %ld,%ld-%ld,%ld monitor %ld,%ld-%ld,%ld presenter %d controls %d",r.left,r.top,r.right,r.bottom,on.rcMonitor.left,on.rcMonitor.top,on.rcMonitor.right,on.rcMonitor.bottom,int(window.AppWindow().Presenter().Kind()),bool(FindDesignPart(root,L"FullscreenControls")));
       co_await SaveDesignSurface(root,designOutput,"nativo-tela-cheia");
       co_await winrt::resume_after(std::chrono::milliseconds(2900));co_await ui;
       if(auto chrome=FindDesignPart(root,L"FullscreenControls"))logf("design full screen controls after idle: opacity %.2f",chrome.Opacity());
       {CURSORINFO c{sizeof(c)};GetCursorInfo(&c);logf("design full screen cursor after idle: hidden-by-us %d showing %d",int(cursorHidden),int((c.flags&CURSOR_SHOWING)!=0));}
       LeaveCallFull();callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(800));co_await ui;
       {CURSORINFO c{sizeof(c)};GetCursorInfo(&c);logf("design full screen left: presenter %d focus %s cursor hidden-by-us %d showing %d",int(window.AppWindow().Presenter().Kind()),roomStage.focus.c_str(),int(cursorHidden),int((c.flags&CURSOR_SHOWING)!=0));}
       co_await SaveDesignSurface(root,designOutput,"nativo-saiu-tela-cheia");
       roomStage.strip=false;EnterCallFull(roomStage, "s:design-visitor");co_await winrt::resume_after(std::chrono::milliseconds(800));co_await ui;
       co_await SaveDesignSurface(root,designOutput,"nativo-tela-cheia-flutuantes");
       {auto stack=FindDesignPart(root,L"FloatingCameras");logf("design full screen floating: %d",stack?int(stack.as<Panel>().Children().Size())-8:-1);}
       LeaveCallFull();callSignature.clear();Render();co_await winrt::resume_after(std::chrono::milliseconds(500));co_await ui;
     }}
    roomStage.strip=true;roomStage.focus.clear();viewingScreens.erase("design-visitor");callSignature.clear();Render();
    auto baseOutput=designOutput;
    for(auto client:{winrt::Windows::Graphics::SizeInt32{1280,800},winrt::Windows::Graphics::SizeInt32{960,640}}) {
      window.AppWindow().ResizeClient(client); co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;
      for(bool endCall:{false,true}) {
        designState.voice.Join("w4-design","geral");
        designOutput=baseOutput/(std::to_wstring(client.Width)+L"x"+std::to_wstring(client.Height)+(endCall?L"-call-end":L"-outside"));
        std::filesystem::create_directories(designOutput);
        co_await ScreenPicker(false,endCall); co_await ui;
      }
    }
    designOutput=baseOutput; TransmissionNotice(false);
    VoiceSettingsPage();co_await SaveDesignSurface(root,designOutput,"nativo-ajustes-camera");
    result["passed"]=true;result["stage"]="done";
  } catch(hresult_error const& error) {result["stage"]="WinUI";result["hresult"]=int64_t(uint32_t(error.code().value));}
  {std::ofstream out(designOutput/L"resultado.json",std::ios::binary);out<<result.Dump();}
  window.Close();Exit();
}
// --design-audit-call <dir>: the open call at 1280x800 with two people, saved
// in the states the web's e2e-harmony/medir-chamada.spec.ts saves (both
// there, muted and not hearing, a screen not arriving here), for the
// comparison by measure. Fixture only: no server, no media.
winrt::Windows::Foundation::IAsyncAction DesignAuditCall() {
  auto lifetime=get_strong();winrt::apartment_context ui;
  auto result=json::Value::Object();result["passed"]=false;std::string stage="open";
  auto invoke=[](FrameworkElement part){Automation::Peers::ButtonAutomationPeer(part.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();};
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction{co_await winrt::resume_after(std::chrono::milliseconds(300));co_await ui;root.UpdateLayout();};
  try {
    window.AppWindow().ResizeClient({1280,802}); // the page is the client less 2, as in --chat-audit
    co_await winrt::resume_after(std::chrono::milliseconds(500));co_await ui;
    designState.signedIn=true;designState.model.live=true;designState.model.rooms={{"call-design","geral",{{"design-me","observador"},{"design-visitor","visitante"}}}};
    designState.voice.self="design-me";designState.voice.Join("call-design","geral");designState.voice.connecting=false;
    showingRooms=false;showingCall=true;callPeople=false;callSignature.clear();Render();co_await settle();
    auto peopleWidth=[this]{return root.ColumnDefinitions().GetAt(2).Width().Value;};
    check_bool(peopleIsland&&peopleIsland.Visibility()==Visibility::Collapsed&&peopleWidth()==0); // folded on the way in
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-duas");
    stage="muted";designState.voice.muted=true;designState.model.rooms[0].people[0].muted=true;designState.model.rooms[0].people[1].deafened=true;Render();co_await settle();
    check_bool(FindDesignPart(root,L"CallMute").as<Button>().IsEnabled()); // muted, still hearing: the button works
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-mudos");
    stage="watch";designState.voice.muted=false;designState.model.rooms[0].people[0].muted=false;designState.model.rooms[0].people[1].deafened=false;designState.model.rooms[0].people[1].screen=true;Render();co_await settle();
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-assistir");
    stage="speaking";designState.voice.speaking["design-visitor"]=true;Render();co_await settle();
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-falando");
    designState.voice.speaking["design-visitor"]=false;
    stage="watching";invoke(FindDesignPart(root,L"WatchScreen"));co_await settle();
    check_bool(bool(FindDesignPart(root,L"ScreenTile"))&&!FindDesignPart(root,L"WatchScreen"));
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-assistindo");
    invoke(FindDesignPart(root,L"StopWatching"));co_await settle();
    check_bool(!FindDesignPart(root,L"ScreenTile")&&bool(FindDesignPart(root,L"WatchScreen")));
    stage="people";invoke(FindDesignPart(root,L"CallPeople"));co_await settle();
    check_bool(peopleIsland.Visibility()==Visibility::Visible&&peopleWidth()==248);
    co_await SaveDesignSurface(root,designOutput,"nativo-chamada-pessoas");
    invoke(FindDesignPart(root,L"CallPeople"));co_await settle();
    check_bool(peopleIsland.Visibility()==Visibility::Collapsed);
    stage="other-room";designState.model.rooms.push_back({"call-other","outra",{{"design-far","longe"}}});Render();co_await settle();
    auto tile=FindDesignPart(root,L"CallTile");designState.model.rooms[1].people[0].muted=true;Render();co_await settle();
    check_bool(FindDesignPart(root,L"CallTile")==tile); // another room's change leaves this call's blocks alone
    stage="leave-forgets";invoke(FindDesignPart(root,L"WatchScreen"));co_await settle();check_bool(bool(FindDesignPart(root,L"ScreenTile")));
    designState.voice.Leave();Render();co_await settle();designState.voice.Join("call-design","geral");designState.voice.connecting=false;showingCall=true;Render();co_await settle();
    check_bool(!FindDesignPart(root,L"ScreenTile")&&bool(FindDesignPart(root,L"WatchScreen"))); // a new session watches nobody
    stage="channel";showingCall=false;callSignature.clear();Render();co_await settle();
    check_bool(peopleIsland.Visibility()==Visibility::Visible&&peopleWidth()==248); // a text channel has its column back
    result["passed"]=true;stage="done";
  } catch(hresult_error const& error) {result["hresult"]=int64_t(uint32_t(error.code().value));}
  result["stage"]=stage;
  {std::ofstream out(designOutput/L"resultado.json",std::ios::binary);out<<result.Dump();}
  window.Close();Exit();
}

#endif
