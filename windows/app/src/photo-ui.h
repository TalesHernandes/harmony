// Included inside App. Decode locally; only Save can send the cropped photo.
Grid photoOverlay{nullptr};
std::vector<std::function<void()>> photoCleanup;
std::function<void(winrt::Windows::Foundation::Point)> photoDragBegin,photoDragTo;
std::function<void()> photoDragEnd;
using PhotoSave=std::function<void(std::string,std::function<void(bool)>)>;
struct PhotoTempFile {
  std::filesystem::path path;
  ~PhotoTempFile(){std::error_code ec;if(!path.empty())std::filesystem::remove(path,ec);}
};
void ClosePhotoEditor() {
  photoDragBegin={};photoDragTo={};photoDragEnd={};
  for(auto& revoke:photoCleanup)revoke();
  photoCleanup.clear();
  if(!photoOverlay) return;
  uint32_t index=0; if(root.Children().IndexOf(photoOverlay,index)) root.Children().RemoveAt(index);
  photoOverlay=nullptr;
}
winrt::Windows::Foundation::IAsyncAction SavePhotoCrop(std::shared_ptr<PhotoCrop> crop,PhotoSave upload,
    Button save,Button cancel,TextBlock error,Grid panel) {
  auto lifetime=get_strong(); winrt::apartment_context ui;
  save.IsEnabled(false);cancel.IsEnabled(false);error.Text(L"");
  std::filesystem::path output;
  try {
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    co_await winrt::resume_background();
    auto pixels=crop->Render(512);
    auto directory=chatOutput.empty()?app::DataDirectory():chatOutput;
    std::filesystem::create_directories(directory);
    auto folder=co_await StorageFolder::GetFolderFromPathAsync(directory.wstring());
    auto file=co_await folder.CreateFileAsync(L"avatar-crop.jpg",CreationCollisionOption::GenerateUniqueName);
    output=file.Path().c_str();
    auto stream=co_await file.OpenAsync(FileAccessMode::ReadWrite);
    BitmapPropertySet properties; properties.Insert(L"ImageQuality",BitmapTypedValue(box_value(.9f),winrt::Windows::Foundation::PropertyType::Single));
    auto encoder=co_await BitmapEncoder::CreateAsync(BitmapEncoder::JpegEncoderId(),stream,properties);
    encoder.SetPixelData(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Ignore,512,512,96,96,pixels);
    co_await encoder.FlushAsync();stream.Close();
    co_await ui;
    if(photoOverlay!=panel || exiting) {std::error_code ec;std::filesystem::remove(output,ec);co_return;}
    auto cleanup=std::make_shared<PhotoTempFile>();cleanup->path=output;
    upload(net::Narrow(output.wstring()),[weak=get_weak(),cleanup,save,cancel,error,panel](bool ok) {
      std::error_code ec;std::filesystem::remove(cleanup->path,ec);
      if(auto self=weak.get();self && !self->exiting && self->photoOverlay==panel) {
        if(ok) self->ClosePhotoEditor();
        else {save.IsEnabled(true);cancel.IsEnabled(true);error.Text(L"Não foi possível enviar a foto. Tente salvar novamente.");}
      }
    });
    co_return;
  } catch(...) { logf("photo crop export failed"); }
  std::error_code ec;if(!output.empty())std::filesystem::remove(output,ec);
  co_await ui;
  if(photoOverlay==panel) {save.IsEnabled(true);cancel.IsEnabled(true);error.Text(L"Não foi possível salvar esta foto. Tente novamente.");}
}
winrt::Windows::Foundation::IAsyncAction EditPhoto(std::filesystem::path path,PhotoSave upload) {
  auto lifetime=get_strong();winrt::apartment_context ui;
  ClosePhotoEditor();
  Grid veil; veil.Name(L"PhotoEditor");veil.Background(Tint(0,.6));Grid::SetColumnSpan(veil,16);Grid::SetRowSpan(veil,16);
  Border card;card.Width(360);card.MaxHeight(std::max(200.0,root.ActualHeight()-32));card.Margin(Thickness{16,16,16,16});card.Padding(Thickness{20,20,20,20});card.Background(Brush(Surface));card.CornerRadius(CornerRadius{16,16,16,16});card.HorizontalAlignment(HorizontalAlignment::Center);card.VerticalAlignment(VerticalAlignment::Center);
  StackPanel body;body.Spacing(16);body.Children().Append(Line(L"Editar foto",20,Foreground,600));body.Children().Append(Text(L"Arraste para reposicionar e ajuste o zoom.",15,Muted));
  Shapes::Ellipse preview;preview.Name(L"PhotoCropPreview");preview.Width(256);preview.Height(256);preview.HorizontalAlignment(HorizontalAlignment::Center);preview.Fill(Brush(Surface2));preview.ManipulationMode(Input::ManipulationModes::None);HarmonyPointer(preview);
  auto crop=std::make_shared<PhotoCrop>();
  auto cropTarget=Action(L"",[]{});cropTarget.Content(preview);cropTarget.Width(256);cropTarget.Height(256);cropTarget.MinHeight(0);cropTarget.MinWidth(0);cropTarget.Padding(Thickness{0});cropTarget.Background(nullptr);cropTarget.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Tint(0,0));cropTarget.Resources().Insert(box_value(L"ButtonBackgroundPressed"),Tint(0,0));cropTarget.HorizontalAlignment(HorizontalAlignment::Center);
  Automation::AutomationProperties::SetName(cropTarget,L"Foto: arraste para reposicionar; use as setas ou a roda do mouse para ajustar");
  body.Children().Append(cropTarget);
  auto redraw=[this,crop,preview] { if(crop->pixels.empty())return;app::PreviewImage pixels;pixels.width=256;pixels.height=256;pixels.pixels=crop->Render(256);ImageBrush brush;brush.ImageSource(PreviewControl(pixels).Source());brush.Stretch(Stretch::Fill);preview.Fill(brush); };
  Grid zoomRow;zoomRow.ColumnSpacing(12);ColumnDefinition labelColumn;labelColumn.Width(GridLength{0,GridUnitType::Auto});zoomRow.ColumnDefinitions().Append(labelColumn);ColumnDefinition sliderColumn;sliderColumn.Width(GridLength{1,GridUnitType::Star});zoomRow.ColumnDefinitions().Append(sliderColumn);
  auto label=Text(L"Zoom",15);label.VerticalAlignment(VerticalAlignment::Center);zoomRow.Children().Append(label);
  Slider zoom;zoom.Name(L"PhotoZoom");zoom.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));zoom.FontWeight(Weight(500));zoom.Minimum(1);zoom.Maximum(4);zoom.StepFrequency(.01);zoom.Value(1);zoom.IsEnabled(false);Automation::AutomationProperties::SetName(zoom,L"Zoom da foto");Grid::SetColumn(zoom,1);zoomRow.Children().Append(zoom);body.Children().Append(zoomRow);
  auto error=Text(L"",13,DangerSoftFg);error.Name(L"PhotoEditorError");CollapseEmpty(error);body.Children().Append(error);
  StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.HorizontalAlignment(HorizontalAlignment::Right);actions.Spacing(8);
  auto cancel=Action(L"Cancelar",[this]{ClosePhotoEditor();});cancel.Name(L"CancelPhotoCrop");auto save=AccountPrimary(L"Salvar",[]{});save.Name(L"SavePhotoCrop");save.IsEnabled(false);actions.Children().Append(cancel);actions.Children().Append(save);body.Children().Append(actions);
  ScrollViewer scroll;scroll.Content(body);scroll.HorizontalContentAlignment(HorizontalAlignment::Stretch);ImmediateMouseWheel(scroll);card.Child(scroll);veil.Children().Append(card);photoOverlay=veil;root.Children().Append(veil);
  Input::KeyboardAccelerator escape;escape.Key(winrt::Windows::System::VirtualKey::Escape);escape.Invoked([this,cancel](auto const&,auto const& e){if(cancel.IsEnabled()){e.Handled(true);ClosePhotoEditor();}});veil.KeyboardAccelerators().Append(escape);veil.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
  auto dragging=std::make_shared<bool>(false);auto point=std::make_shared<winrt::Windows::Foundation::Point>();
  photoDragBegin=[point,dragging,save](winrt::Windows::Foundation::Point next) {if(!save.IsEnabled())return;*point=next;*dragging=true;logf("photo crop drag started");};
  photoDragTo=[this,point,dragging,crop,redraw,save](winrt::Windows::Foundation::Point next) {if(!*dragging||!save.IsEnabled())return;crop->x+=(next.X-point->X)*2;crop->y+=(next.Y-point->Y)*2;*point=next;crop->Constrain();if(!chatOutput.empty())logf("photo crop pan %.2f %.2f",crop->x,crop->y);redraw();};
  photoDragEnd=[dragging] {*dragging=false;logf("photo crop drag finished");};
  // Button consumes pointer input for keyboard focus. Receive handled events
  // on that same target so dragging cannot be swallowed by its click gesture.
  auto pressed=box_value(Input::PointerEventHandler([this,cropTarget,preview,save](auto const&,Input::PointerRoutedEventArgs const& e){if(!save.IsEnabled())return;photoDragBegin(e.GetCurrentPoint(preview).Position());cropTarget.CapturePointer(e.Pointer());e.Handled(true);}));
  cropTarget.AddHandler(UIElement::PointerPressedEvent(),pressed,true);photoCleanup.push_back([cropTarget,pressed]{cropTarget.RemoveHandler(UIElement::PointerPressedEvent(),pressed);});
  auto moved=box_value(Input::PointerEventHandler([this,preview](auto const&,Input::PointerRoutedEventArgs const& e){photoDragTo(e.GetCurrentPoint(preview).Position());}));
  cropTarget.AddHandler(UIElement::PointerMovedEvent(),moved,true);photoCleanup.push_back([cropTarget,moved]{cropTarget.RemoveHandler(UIElement::PointerMovedEvent(),moved);});
  auto released=box_value(Input::PointerEventHandler([this,cropTarget](auto const&,Input::PointerRoutedEventArgs const& e){photoDragEnd();cropTarget.ReleasePointerCapture(e.Pointer());}));
  cropTarget.AddHandler(UIElement::PointerReleasedEvent(),released,true);photoCleanup.push_back([cropTarget,released]{cropTarget.RemoveHandler(UIElement::PointerReleasedEvent(),released);});
  auto lost=cropTarget.PointerCaptureLost([this](auto const&,auto const&){photoDragEnd();if(!chatOutput.empty())logf("photo crop capture lost");});photoCleanup.push_back([cropTarget,lost]{cropTarget.PointerCaptureLost(lost);});
  auto wheel=box_value(Input::PointerEventHandler([crop,zoom,save](auto const&,Input::PointerRoutedEventArgs const& e){if(!save.IsEnabled())return;zoom.Value(std::clamp(crop->zoom*std::exp(e.GetCurrentPoint(nullptr).Properties().MouseWheelDelta()*.002),1.0,4.0));e.Handled(true);}));
  cropTarget.AddHandler(UIElement::PointerWheelChangedEvent(),wheel,true);photoCleanup.push_back([cropTarget,wheel]{cropTarget.RemoveHandler(UIElement::PointerWheelChangedEvent(),wheel);});
  auto photoValueChanged=zoom.ValueChanged([crop,redraw,save](auto const&,auto const& e){if(!save.IsEnabled())return;crop->zoom=e.NewValue();crop->Constrain();redraw();});
  photoCleanup.push_back([zoom,photoValueChanged]{zoom.ValueChanged(photoValueChanged);});
  auto photoKeyDown=cropTarget.KeyDown([crop,redraw,save](auto const&,Input::KeyRoutedEventArgs const& e){
    if(!save.IsEnabled())return;using Key=winrt::Windows::System::VirtualKey;
    if(e.Key()==Key::Left)crop->x-=10;else if(e.Key()==Key::Right)crop->x+=10;else if(e.Key()==Key::Up)crop->y-=10;else if(e.Key()==Key::Down)crop->y+=10;else return;
    e.Handled(true);crop->Constrain();redraw();
  });
  photoCleanup.push_back([cropTarget,photoKeyDown]{cropTarget.KeyDown(photoKeyDown);});
  auto photoClick=save.Click([this,crop,upload,save,cancel,error,veil](auto const&,auto const&){SavePhotoCrop(crop,upload,save,cancel,error,veil);});
  photoCleanup.push_back([save,photoClick]{save.Click(photoClick);});
  hstring failure;
  try {
    using namespace winrt::Windows::Graphics::Imaging;using namespace winrt::Windows::Storage;
    co_await winrt::resume_background();
    if(std::filesystem::file_size(path)>5u*1024*1024)throw hresult_invalid_argument(L"A foto é grande demais (o limite é 5 MiB).");
    auto file=co_await StorageFile::GetFileFromPathAsync(path.wstring());auto stream=co_await file.OpenReadAsync();auto decoder=co_await BitmapDecoder::CreateAsync(stream);
    auto id=decoder.DecoderInformation().CodecId();
    if(id!=BitmapDecoder::JpegDecoderId()&&id!=BitmapDecoder::PngDecoderId()&&id!=BitmapDecoder::GifDecoderId())throw hresult_invalid_argument(L"Escolha uma foto JPEG, PNG ou GIF. WebP não é aceito.");
    auto w=decoder.OrientedPixelWidth(),h=decoder.OrientedPixelHeight();
    if(!w||!h||w>8000||h>8000||uint64_t(w)*h>40000000)throw hresult_invalid_argument(L"A foto excede 8000 pixels por lado ou 40 megapixels.");
    auto data=co_await decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Premultiplied,BitmapTransform{},ExifOrientationMode::RespectExifOrientation,ColorManagementMode::ColorManageToSRgb);auto bytes=data.DetachPixelData();stream.Close();
    co_await ui;if(photoOverlay!=veil||exiting)co_return;
    crop->width=int(w);crop->height=int(h);crop->pixels.assign(bytes.begin(),bytes.end());crop->Constrain();redraw();zoom.IsEnabled(true);save.IsEnabled(true);cancel.Focus(FocusState::Programmatic);logf("photo editor opened");
  }catch(winrt::hresult_error const& e){failure=e.code()==E_INVALIDARG?e.message():L"Não foi possível abrir esta foto. Escolha outra imagem.";}
  catch(...){failure=L"Não foi possível abrir esta foto. Escolha outra imagem.";}
  co_await ui;if(!failure.empty()&&photoOverlay==veil){error.Text(failure);logf("photo editor decode failed");}
}
