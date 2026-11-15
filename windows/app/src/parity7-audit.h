// Round seven exercises the rendered editor before allowing any upload.
winrt::Windows::Foundation::IAsyncAction Parity7Audit() {
  auto lifetime=get_strong();winrt::apartment_context ui;
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction {co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;root.UpdateLayout();};
  bool sent=false;auto exported=std::make_shared<std::string>();
  PhotoSave observe=[&sent,exported](std::string path,std::function<void(bool)> complete){sent=true;*exported=path;std::filesystem::copy_file(std::filesystem::path(net::Wide(path)),chatOutput/L"round7-avatar-output.jpg",std::filesystem::copy_options::overwrite_existing);complete(false);};
  co_await EditPhoto(std::filesystem::path(net::Wide(app::Environment(L"HARMONY_TEST_LANDSCAPE"))),observe);co_await ui;
  ParityRequire(photoOverlay && FindDesignPart(photoOverlay,L"SavePhotoCrop").as<Button>().IsEnabled() && !sent);
  co_await SaveDesignSurface(root,chatOutput,"round7-photo-editor");
  ClosePhotoEditor();ParityRequire(!sent);
  co_await EditPhoto(std::filesystem::path(net::Wide(app::Environment(L"HARMONY_TEST_LANDSCAPE"))),observe);co_await ui;
  auto zoom=FindDesignPart(photoOverlay,L"PhotoZoom").as<Slider>();zoom.Value(2);co_await settle();co_await ui;
  auto preview=FindDesignPart(photoOverlay,L"PhotoCropPreview");
  auto original=preview.as<Shapes::Ellipse>().Fill().as<ImageBrush>().ImageSource();
  auto beforeDrag=std::make_shared<DesignPixels>();co_await ReadDesignPixels(preview,beforeDrag);co_await ui;
  photoDragBegin({128,128});co_await settle();co_await ui;
  photoDragTo({168,148});co_await settle();co_await ui;
  photoDragEnd();co_await settle();co_await ui;
  ParityRequire(preview.as<Shapes::Ellipse>().Fill().as<ImageBrush>().ImageSource()!=original);
  auto afterDrag=std::make_shared<DesignPixels>();co_await ReadDesignPixels(preview,afterDrag);co_await ui;
  ParityRequire(beforeDrag->bytes!=afterDrag->bytes);
  co_await SaveDesignSurface(root,chatOutput,"round7-photo-crop-dragged");
  // A failed upload keeps the crop and re-enables Save so the owner can retry.
  auto save=FindDesignPart(photoOverlay,L"SavePhotoCrop").as<Button>();
  Automation::Peers::ButtonAutomationPeer(save).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
  for(int i=0;i<100&&!sent;++i){co_await settle();co_await ui;}
  ParityRequire(sent && save.IsEnabled() && photoOverlay);
  co_await SaveDesignSurface(root,chatOutput,"round7-photo-retry");
  ClosePhotoEditor();
  // Successful Save uses the real authenticated account service.
  app::ChatPerson own;service->ChatModel().Inspect([&](app::ChatState const& state){for(auto person:state.members)if(person.username==state.owner)own=person;});
  co_await EditPhoto(std::filesystem::path(net::Wide(app::Environment(L"HARMONY_TEST_LANDSCAPE"))),[this,own](std::string path,std::function<void(bool)> complete){UpdateOwnPhoto(own,path,false,complete);});co_await ui;
  save=FindDesignPart(photoOverlay,L"SavePhotoCrop").as<Button>();
  Automation::Peers::ButtonAutomationPeer(save).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
  for(int i=0;i<100&&photoOverlay;++i){co_await settle();co_await ui;}
  ParityRequire(!photoOverlay);OpenProfile(own);co_await settle();co_await ui;
  co_await SaveDesignSurface(root,chatOutput,"round7-profile-photo-saved");CloseProfile();
  OpenSettings("conta");co_await settle();co_await ui;
  auto pick=FindDesignPart(settingsOverlay,L"ChangePhoto");auto remove=FindDesignPart(settingsOverlay,L"RemovePhoto");
  ParityRequire(bool(pick) && bool(remove));
  ParityRequire(pick.ActualWidth()<200 && remove.ActualWidth()<200);
  co_await SaveDesignSurface(root,chatOutput,"round7-account-spaced");CloseSettings();
  OpenEmojiPanel();co_await settle();co_await ui;
  auto query=FindDesignPart(picker.Child(),L"EmojiSearch").as<TextBox>();query.Text(L"sdfsdfsd");query.Focus(FocusState::Programmatic);co_await settle();co_await ui;
  co_await SaveDesignSurface(picker.Child().as<FrameworkElement>(),chatOutput,"round7-emoji-typed");ClosePicker();
  std::ofstream(chatOutput/L"round7-result.json")<<"{\"passed\":true,\"photo_cancel_without_upload\":true,\"photo_drag\":true,\"photo_retry\":true,\"photo_uploaded\":true}";
}
winrt::Windows::Foundation::IAsyncAction Parity7bAudit() {
  auto lifetime=get_strong();winrt::apartment_context ui;
  auto settle=[&]()->winrt::Windows::Foundation::IAsyncAction {co_await winrt::resume_after(std::chrono::milliseconds(250));co_await ui;root.UpdateLayout();};
  ChatAuditPosition();
  OpenSearch();co_await settle();co_await ui;
  for(int i=0;i<40;++i) {Border card;card.Height(56);card.Background(Brush(Surface2));card.CornerRadius(CornerRadius{8,8,8,8});card.Child(Line(L"Scroll result fixture",15,Foreground,500));searchResults.Children().Append(card);}
  co_await settle();co_await ui;
  auto scroll=FindDesignPart(searchDialog,L"SearchScroll").as<ScrollViewer>();
  ParityRequire(scroll.ScrollableHeight()>100 && scroll.Padding().Right>=16);
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round7b middle delivery");co_await ui;
  auto middle=[this](bool down) {INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=down?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_MIDDLEUP;ParityRequire(AuditSendInput(1,&input,sizeof input)==1);};
  ParityMove(scroll);co_await settle();co_await ui;
  middle(true);co_await settle();co_await ui;middle(false);co_await settle();co_await ui;
  POINT point{};GetCursorPos(&point);AuditSetCursorPos(point.x,point.y+80);co_await settle();co_await ui;
  ParityRequire(scroll.VerticalOffset()>0);
  co_await SaveDesignSurface(host,chatOutput,"round7b-search-scroll");
  middle(true);co_await settle();co_await ui;middle(false);co_await settle();co_await ui;
  auto stopped=scroll.VerticalOffset();co_await settle();co_await ui;ParityRequire(scroll.VerticalOffset()==stopped);
  scroll.ChangeView(nullptr,0.0,nullptr,true);ParityMove(scroll);co_await settle();co_await ui;
  middle(true);co_await settle();co_await ui;GetCursorPos(&point);AuditSetCursorPos(point.x,point.y+80);co_await settle();co_await ui;
  middle(false);co_await settle();co_await ui;ParityRequire(scroll.VerticalOffset()>0);stopped=scroll.VerticalOffset();co_await settle();co_await ui;ParityRequire(scroll.VerticalOffset()==stopped);
  ParityMove(scroll);co_await settle();co_await ui;middle(true);co_await settle();co_await ui;middle(false);co_await settle();co_await ui;
  INPUT escape[2]{};for(auto& key:escape) {key.type=INPUT_KEYBOARD;key.ki.wVk=VK_ESCAPE;}escape[1].ki.dwFlags=KEYEVENTF_KEYUP;ParityRequire(AuditSendInput(2,escape,sizeof(INPUT))==2);
  co_await settle();co_await ui;ParityRequire(bool(searchDialog));stopped=scroll.VerticalOffset();GetCursorPos(&point);AuditSetCursorPos(point.x,point.y+60);co_await settle();co_await ui;ParityRequire(scroll.VerticalOffset()==stopped);
  } else logf("round7b latched/held middle scroll and Escape delivery: skipped (HARMONY_AUDIT_REAL_POINTER)");
  CloseSearch();
  auto local=co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(net::Wide(app::Environment(L"HARMONY_TEST_LANDSCAPE")));co_await ui;
  auto stream=co_await local.OpenReadAsync();co_await ui;Imaging::BitmapImage source;co_await source.SetSourceAsync(stream);co_await ui;
  app::ChatAttachment file;file.url="round7b-local-fixture";file.name="zoom.png";file.width=1600;file.height=900;shownImages.insert_or_assign(file.url,source);
  ParityRequire(!file.url.empty());app::ChatMessage message;OpenLightbox(file,message);co_await settle();co_await ui;
  auto viewport=FindDesignPart(lightbox,L"ImageViewport");
  auto findImage=[&](auto&& self,DependencyObject node)->Image {if(auto image=node.try_as<Image>()) return image;for(int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i) if(auto image=self(self,Media::VisualTreeHelper::GetChild(node,i))) return image;return nullptr;};
  auto image=findImage(findImage,viewport);ParityRequire(bool(image));
  auto transform=FindDesignPart(lightbox,L"LightboxFrame").RenderTransform().as<CompositeTransform>();
  auto invoke=[&](wchar_t const* name) {auto button=FindDesignPart(lightbox,name).as<Button>();Automation::Peers::ButtonAutomationPeer(button).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();};
  invoke(L"ImageZoomIn");co_await settle();co_await ui;ParityRequire(transform.ScaleX()==1.25);
  if(AuditRealPointer()) {
    co_await ChatAuditForeground("round7b wheel delivery");co_await ui;
    ParityMove(viewport);INPUT wheel{};wheel.type=INPUT_MOUSE;wheel.mi.dwFlags=MOUSEEVENTF_WHEEL;wheel.mi.mouseData=WHEEL_DELTA;ParityRequire(AuditSendInput(1,&wheel,sizeof wheel)==1);
    co_await settle();co_await ui;ParityRequire(transform.ScaleX()>1.25);
    invoke(L"ImageFit");invoke(L"ImageZoomIn");
  } else logf("round7b wheel delivery: skipped (HARMONY_AUDIT_REAL_POINTER)");
  lightboxWheel(WHEEL_DELTA,viewport.ActualWidth()/2,viewport.ActualHeight()/2);
  co_await settle();co_await ui;ParityRequire(transform.ScaleX()>1.25);
  lightboxDragBegin({100,100});lightboxDragTo({140,120});lightboxDragEnd();
  co_await settle();co_await ui;ParityRequire(lightbox && transform.TranslateX()>30 && transform.TranslateY()>10);
  co_await SaveDesignSurface(host,chatOutput,"round7b-image-zoom");invoke(L"ImageFit");co_await settle();co_await ui;ParityRequire(transform.ScaleX()==1 && transform.TranslateX()==0 && transform.TranslateY()==0);
  co_await SaveDesignSurface(host,chatOutput,"round7b-image-fit");CloseLightbox();
  co_await SaveDesignSurface(host,chatOutput,"round7b-presence");
  if(AuditRealPointer()) {
  co_await ChatAuditForeground("round7b close hover");co_await ui;
  ParityMove(captionButtons[2]);co_await settle();co_await ui;co_await SaveDesignWindow(hwnd,chatOutput,L"round7b-caption-hover.png");
  ParityMove(composer);co_await settle();co_await ui;co_await SaveDesignWindow(hwnd,chatOutput,L"round7b-caption-idle.png");
  } else logf("round7b non-client close hover: skipped (HARMONY_AUDIT_REAL_POINTER)");
  std::ofstream(chatOutput/L"round7b-result.json")<<"{\"passed\":true,\"middle_scroll_skipped\":"<<(AuditRealPointer()?"false":"true")<<",\"zoom\":true}";
}
