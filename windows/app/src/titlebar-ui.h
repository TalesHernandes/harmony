// Included inside App. Windows owns non-client gestures and Snap; harmony
// owns every pixel, including the caption buttons and their pointer states.
winrt::Microsoft::UI::Input::InputNonClientPointerSource captionInput{nullptr};
std::vector<Button> captionButtons;
bool captionMaximized=false;
int captionHot=-1;
// Where you are, small and grey in the middle of the bar (the owner, 02/11/2026,
// asked what to do with the empty strip and chose this): "harmony · sol".
TextBlock captionPlace{nullptr};
void CaptionPlace(std::wstring const& where) {
  if(!captionPlace) return;
  captionPlace.Text(where.empty()?L"harmony":L"harmony \u00b7 "+where);
}
void CaptionHoverAtPointer() {
  if(captionButtons.size()!=3)return;
  POINT p{};GetCursorPos(&p);ScreenToClient(hwnd,&p);
  double scale=GetDpiForWindow(hwnd)/96.0;int selected=-1;
  for(int i=0;i<3;++i) {
    auto b=captionButtons[i];auto at=b.TransformToVisual(nullptr).TransformPoint({0,0});
    if(p.x>=at.X*scale && p.x<(at.X+b.ActualWidth())*scale && p.y>=at.Y*scale && p.y<(at.Y+b.ActualHeight())*scale)selected=i;
  }
  if(selected==captionHot)return;captionHot=selected;
  if(!designOutput.empty() && app::Environment(L"HARMONY_W8A")=="1")logf("w8a caption selected %d, client cursor %ld,%ld",selected,p.x,p.y);
  for(int i=0;i<3;++i)captionButtons[i].Background(Brush(i==selected?(i==2?0xdb3b3e:0x27272a):0x060607));
}
void CaptionRegions() {
  if(!captionInput || captionButtons.size()!=3 || host.ActualWidth()<144) return;
  using winrt::Microsoft::UI::Input::NonClientRegionKind;
  double scale=GetDpiForWindow(hwnd)/96.0;
  auto rect=[scale](FrameworkElement const& element) {
    auto p=element.TransformToVisual(nullptr).TransformPoint({0,0});
    return winrt::Windows::Graphics::RectInt32{int(std::round(p.X*scale)),int(std::round(p.Y*scale)),int(std::round(element.ActualWidth()*scale)),int(std::round(element.ActualHeight()*scale))};
  };
  auto first=rect(captionButtons[0]);
  captionInput.SetRegionRects(NonClientRegionKind::Caption,{{0,0,first.X,first.Height}});
  captionInput.SetRegionRects(NonClientRegionKind::Minimize,{rect(captionButtons[0])});
  captionInput.SetRegionRects(NonClientRegionKind::Maximize,{rect(captionButtons[1])});
  captionInput.SetRegionRects(NonClientRegionKind::Close,{rect(captionButtons[2])});
  bool maximized=IsZoomed(hwnd)!=FALSE;
  if(captionMaximized!=maximized) {
    captionMaximized=maximized;
    captionButtons[1].Content(Markup::XamlReader::Load(maximized?
      L"<Canvas xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='14' Height='14'><Path Data='M5,3 L11,3 Q12,3 12,4 L12,10 M3,5 L9,5 Q10,5 10,6 L10,12 L2,12 L2,6 Q2,5 3,5 Z' Stroke='#fcfcfc' StrokeThickness='1.5'/></Canvas>":
      L"<Canvas xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='14' Height='14'><Rectangle Width='10' Height='10' Canvas.Left='2' Canvas.Top='2' RadiusX='2' RadiusY='2' Stroke='#fcfcfc' StrokeThickness='1.5'/></Canvas>"));
    Automation::AutomationProperties::SetName(captionButtons[1],maximized?L"Restaurar":L"Maximizar");
  }
}
void BuildCaption(Grid const& titleBar) {
  using winrt::Microsoft::UI::Input::NonClientRegionKind;
  window.AppWindow().TitleBar().PreferredHeightOption(winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Collapsed);
  StackPanel buttons; buttons.Orientation(Orientation::Horizontal); buttons.HorizontalAlignment(HorizontalAlignment::Right);
  for(int i=0;i<3;++i) {
    Button button; HarmonyPointer(button); button.Name(i==0?L"CaptionMinimize":i==1?L"CaptionMaximize":L"CaptionClose");
    // Non-client input owns these states. A Button PointerOver setter would
    // override Background, then disappear when XAML hands input back to NC.
    button.Template(Markup::XamlReader::Load(L"<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='Button'><Border Background='{TemplateBinding Background}' CornerRadius='{TemplateBinding CornerRadius}'><ContentPresenter Content='{TemplateBinding Content}' HorizontalAlignment='Center' VerticalAlignment='Center'/></Border></ControlTemplate>").as<ControlTemplate>());
    button.Width(46); button.Height(32); button.MinHeight(0); button.Padding(Thickness{0}); button.CornerRadius(CornerRadius{6,6,6,6}); button.Background(Brush(0x060607));
    button.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(i==2?0xe15451:0x27272a));
    button.Resources().Insert(box_value(L"ButtonBackgroundPressed"),Brush(i==2?0xb82f32:0x323235));
    if(i==2) { button.Content(VoiceIcon("xmark",0xfcfcfc,14)); }
    else button.Content(Markup::XamlReader::Load(i==0?
      L"<Canvas xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='14' Height='14'><Path Data='M2,7 L12,7' Stroke='#fcfcfc' StrokeThickness='1.5'/></Canvas>":
      L"<Canvas xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='14' Height='14'><Rectangle Width='10' Height='10' Canvas.Left='2' Canvas.Top='2' RadiusX='2' RadiusY='2' Stroke='#fcfcfc' StrokeThickness='1.5'/></Canvas>"));
    Automation::AutomationProperties::SetName(button,i==0?L"Minimizar":i==1?L"Maximizar ou restaurar":L"Fechar");
    button.Click([this,i](auto const&,auto const&) { if(i==0) ShowWindow(hwnd,SW_MINIMIZE); else if(i==1) ShowWindow(hwnd,IsZoomed(hwnd)?SW_RESTORE:SW_MAXIMIZE); else PostMessageW(hwnd,WM_CLOSE,0,0); });
    auto reconcile=[this](auto const&,auto const&) {CaptionHoverAtPointer();};
    button.PointerEntered(reconcile);button.PointerMoved(reconcile);button.PointerExited(reconcile);
    buttons.Children().Append(button); captionButtons.push_back(button);
  }
  titleBar.Children().Append(buttons);
  captionPlace=TextBlock(); captionPlace.Name(L"CaptionPlace"); captionPlace.FontSize(12); captionPlace.Foreground(Brush(0xa1a1aa));
  captionPlace.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); captionPlace.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  captionPlace.HorizontalAlignment(HorizontalAlignment::Center); captionPlace.VerticalAlignment(VerticalAlignment::Center);
  captionPlace.TextTrimming(TextTrimming::CharacterEllipsis); captionPlace.MaxWidth(480); captionPlace.IsHitTestVisible(false); CaptionPlace(L"");
  titleBar.Children().Append(captionPlace);
  captionInput=winrt::Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(window.AppWindow().Id());
  auto hover=[this](auto const&,auto const&) { CaptionHoverAtPointer(); };
  captionInput.PointerEntered(hover); captionInput.PointerMoved(hover);
  // Exited also fires at the XAML/non-client handoff while the cursor still
  // covers the same button. Determine the exit from geometry, not that event.
  captionInput.PointerExited(hover);
  titleBar.SizeChanged([this](auto const&,auto const&) { CaptionRegions(); });
  titleBar.Loaded([this](auto const&,auto const&) { CaptionRegions(); });
}
