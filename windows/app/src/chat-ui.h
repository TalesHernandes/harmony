// Included inside App: the W5 chat and the shell around it, copied from the
// web by photo and measure (medidas-web.md), not drawn anew. Every number in
// here is one measured on the web at 1280x800; where the web's own comment
// explains a number, the same reason holds here.
//
// The model is app::Chat (chat.h). The screen reads it under Inspect and asks
// for work through Service::ChatWork; nothing here waits on the network.

std::string chatTab = "server";          // "server" or "dm", the two tabs
std::string chatActive;                  // the conversation on screen
Button headerCall{nullptr};              // the handset on a direct conversation (W6)
bool peopleShown = true;
app::ChatPerson headerPeer;
uint64_t chatSeen = 0;                   // last journal entry applied to the list
Grid shell{nullptr}, conversation{nullptr};
StackPanel channelList{nullptr}, peopleList{nullptr};
TextBlock historyStart{nullptr};
TextBlock headerTitle{nullptr}, typingLine{nullptr}, composerError{nullptr}, emptyLine{nullptr};
Border headerMark{nullptr}, replyBar{nullptr}, tabServer{nullptr}, tabDirect{nullptr};
TextBlock replyName{nullptr}, replyBody{nullptr};
ListView messageList{nullptr};
winrt::Windows::Foundation::Collections::IObservableVector<IInspectable> messageItems{nullptr};
ScrollViewer messageScroll{nullptr};
TextBox composer{nullptr}, composerHint{nullptr};
Button sendButton{nullptr};
int64_t replyingTo = 0, editingId = 0, hoveredId = 0;
int64_t openedRead = 0;
bool stickToBottom = true, composerLoading = false, chatShown = true;
std::string accountPhoto;
std::string pendingSent;                 // the text on its way; the box keeps it until the server says yes
bool sendWaiting = false;                // a message (text or files) is on its way
std::string channelsSignature, peopleSignature;

static constexpr uint32_t Background = 0x060607, Surface = 0x18181b, Surface2 = 0x232325, Surface3 = 0x323235,
    Default = 0x27272a, BorderLine = 0x28282c, Foreground = 0xfcfcfc, Muted = 0xa1a1aa, Accent = 0x0485f7,
    AccentSoftFg = 0x61a8fc, Success = 0x17c964, DangerSoftFg = 0xf5645b;

static SolidColorBrush Tint(uint32_t rgb, double alpha) {
  return SolidColorBrush(winrt::Windows::UI::Color{BYTE(alpha * 255 + 0.5), BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)});
}
static winrt::Windows::UI::Text::FontWeight Weight(uint16_t w) { return winrt::Windows::UI::Text::FontWeight{uint16_t(std::max(500,int(w)))}; }
// A line of one style, never wrapped: the web truncates every single-line
// label (names, channel names, the reply quote) with an ellipsis.
static TextBlock Line(std::wstring const& value, double size, uint32_t color, uint16_t weight) {
  auto text = Text(value, size, color); text.FontWeight(Weight(weight));
  text.TextWrapping(TextWrapping::NoWrap); text.TextTrimming(TextTrimming::CharacterEllipsis);
  return text;
}
// Avatar.tsx: first and last initials, 13px, a round face at every size.
static Border Face(std::wstring const& name, double size, bool present = true) {
  Border face; face.Width(size); face.Height(size); face.CornerRadius(CornerRadius{size / 2, size / 2, size / 2, size / 2});
  face.Background(Brush(Surface3));
  auto text = Line(Initials(name), 13, present ? Foreground : Muted, 600);
  text.LineHeight(18); text.CharacterSpacing(size == 28 ? 60 : -20);
  text.HorizontalAlignment(HorizontalAlignment::Center); text.VerticalAlignment(VerticalAlignment::Center);
  face.Child(text); return face;
}
Button Square(std::string const& icon, std::wstring const& label, double box, double glyph, uint32_t color,
              uint32_t hover, std::function<void()> clicked) {
  Button button; HarmonyPointer(button); button.Width(box); button.Height(box); button.MinWidth(0); button.MinHeight(0);
  button.Padding(Thickness{0, 0, 0, 0}); button.CornerRadius(CornerRadius{8, 8, 8, 8}); button.BorderThickness(Thickness{0, 0, 0, 0});
  bool negative=icon=="xmark" || icon=="trash-bin" || label.find(L"Recusar")!=std::wstring::npos || label.find(L"Sair")!=std::wstring::npos || label==L"Desligar";
  if(negative) { color=0xfcfcfc; hover=0xe15451; }
  button.Background(negative?Brush(0xdb3b3e):Tint(0,0)); button.Content(VoiceIcon(icon,color,glyph));
  if(negative) {
    NegativeButton(button);
    button.PointerEntered([icon, glyph](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();IconInk(button.Content(),0xfcfcfc);});
    button.PointerExited([icon, glyph](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();IconInk(button.Content(),0xfcfcfc);});
  }
  // Messages.tsx `action`: grey at rest, the full white under the pointer.
  else if(color==Muted) {
    button.PointerEntered([icon, glyph](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();IconInk(button.Content(),Foreground);});
    button.PointerExited([icon, glyph](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();IconInk(button.Content(),Muted);});
  }
  button.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(hover));
  button.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(hover));
  Automation::AutomationProperties::SetName(button, label);
  ToolTipService::SetToolTip(button, box_value(label));
  button.Click([this, clicked](auto const&, auto const&) { if (!exiting) clicked(); });
  return button;
}
// A right-click menu opens at the pointer, as on the web and in Discord (the
// owner, 02/11/2026), not under the element; from the keyboard (Shift+F10,
// the menu key) there is no pointer, and it opens at the element.
static void OpenContextMenu(UIElement const& target, Controls::Primitives::FlyoutBase const& menu,
    std::optional<winrt::Windows::Foundation::Point> at = {}) {
  Controls::Primitives::FlyoutShowOptions options;
  if (at) options.Position(*at);
  menu.ShowAt(target, options);
}
static void MenuAtPointer(UIElement const& element, Controls::Primitives::FlyoutBase const& menu) {
  element.ContextFlyout(menu);
  element.ContextRequested([menu](IInspectable const& sender, Input::ContextRequestedEventArgs const& args) {
    auto target = sender.as<UIElement>(); winrt::Windows::Foundation::Point at{};
    OpenContextMenu(target, menu, args.TryGetPosition(target, at) ? std::optional{at} : std::nullopt);
    args.Handled(true);
  });
}
// The same for a flyout made on the press (the person's volume): where the
// pointer went down, if it did.
static void ShowAtPointer(Controls::Primitives::FlyoutBase const& flyout, FrameworkElement const& target, std::optional<winrt::Windows::Foundation::Point> at) {
  Controls::Primitives::FlyoutShowOptions options;
  if (at) { options.Position(*at); options.Placement(Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft); }
  flyout.ShowAt(target, options);
}
void BindFieldRing(TextBox const& field,Border const& surface) {
  auto weakField=make_weak(field); std::wstring name(surface.Name());
  // Resolve the live visual ancestor: the temporary Border wrapper's weak
  // reference can expire while its XAML surface is still on screen.
  auto update=[weakField,name] {
    if(auto box=weakField.get()) if(auto border=FindAncestor(box,name).try_as<Border>())
      border.BorderBrush(box.FocusState()==FocusState::Unfocused?border.Background():Brush(Accent));
  };
  field.RegisterPropertyChangedCallback(UIElement::FocusStateProperty(),[update](auto const&,auto const&) {update();});
  field.Loaded([update](auto const&,auto const&) {update();});
}
static Border Island(double radius = 8) {
  Border island; island.Background(Brush(Surface)); island.CornerRadius(CornerRadius{radius, radius, radius, radius});
  return island;
}
std::wstring PersonName(app::ChatPerson const& p) {
  auto n = !p.nickname.empty() ? p.nickname : !p.displayName.empty() ? p.displayName : p.username;
  return net::Wide(n);
}

// ---------------------------------------------------------------------------
// Pictures from the server (Twemoji, photos, avatars). The bytes come through
// the chat's bounded cache on the worker; decoding happens here, once per
// address, and every Image waiting for the same address gets the same source.
// A Twemoji the set does not have comes back empty and the character itself
// is drawn instead, as the web's onError does.
// ---------------------------------------------------------------------------
std::map<std::string, ImageSource> shownImages;
app::ImageCacheBudget decodedBudget;
std::map<std::string, std::vector<std::function<void(ImageSource)>>> waitingImages;
std::map<std::string, ULONGLONG> missingImages; // when it failed: tried again after a minute
uint64_t pictureEpoch=0;
// `pixels`: an SVG is drawn at exactly that many device pixels, and the
// cache keeps one source per size. Left to itself SvgImageSource draws at the
// file's 36 and the Image shrinks that without smoothing: the jagged emoji.
void LoadPicture(std::string const& address, bool svg, std::function<void(ImageSource)> use, int pixels = 0) {
  auto url = pixels ? address + "#" + std::to_string(pixels) : address;
  if (auto it = shownImages.find(url); it != shownImages.end()) {decodedBudget.Touch(url);use(it->second);return;}
  if (auto miss = missingImages.find(url); miss != missingImages.end()) {
    if (GetTickCount64() - miss->second < 60000) { use(nullptr); return; }
    missingImages.erase(miss);
  }
  if (!service) return; // quitting: Quit() has handed the service to its cleanup
  auto& waiting = waitingImages[url]; waiting.push_back(std::move(use));
  if (waiting.size() > 1) return;
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  service->ChatImage(address, [dispatcher, weak, url, svg, pixels,epoch=pictureEpoch](std::shared_ptr<const std::string> bytes) {
    dispatcher.TryEnqueue([weak, url, svg, pixels, bytes,epoch] { if (auto self = weak.get(); self && !self->exiting && self->pictureEpoch==epoch) self->Arrive({url, svg, bytes, pixels}); });
  });
}
// The height, in device pixels, to decode a message's picture at: enough for a
// box `tall` high (and, cropped to fill, `side` across), never more than the
// file has. Zero: decode it as it is.
int PictureDecodeHeight(app::ChatAttachment const& f, double tall, double side = 0) {
  if (f.width <= 0 || f.height <= 0) return 0;
  double dpi = root && root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1;
  double need = tall * dpi;
  if (side > 0 && f.height > f.width) need = side * dpi * double(f.height) / double(f.width);
  int height = int(std::ceil(need));
  return height > 0 && height < f.height ? height : 0;
}
// Pictures are decoded a few at a time, 8 ms of the window's turn at most,
// with whatever else the window has to do (a click, a scroll, a frame) in
// between: the emoji panel asks for hundreds at once.
struct Arrival { std::string url; bool svg; std::shared_ptr<const std::string> bytes; int pixels; };
std::deque<Arrival> arrivals; bool decoding = false;
void Arrive(Arrival a) { arrivals.push_back(std::move(a)); if (!decoding) DecodeArrivals(); }
winrt::fire_and_forget DecodeArrivals() {
  auto lifetime = get_strong(); decoding = true;
  auto queue = window.DispatcherQueue();
  while (!arrivals.empty() && !exiting) {
    auto start = GetTickCount64();
    while (!arrivals.empty() && !exiting && GetTickCount64() - start < 8) {
      auto a = std::move(arrivals.front()); arrivals.pop_front();
      co_await ImageArrived(std::move(a.url), a.svg, std::move(a.bytes), a.pixels);
    }
    struct LowTurn { winrt::Microsoft::UI::Dispatching::DispatcherQueue q; bool await_ready() { return false; }
      void await_suspend(std::coroutine_handle<> h) { if (!q.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [h] { h(); })) h(); }
      void await_resume() {} };
    co_await LowTurn{queue};
  }
  decoding = false;
}
winrt::Windows::Foundation::IAsyncAction ImageArrived(std::string url, bool svg, std::shared_ptr<const std::string> bytes, int pixels) {
  auto lifetime = get_strong();
  auto epoch=pictureEpoch;
  ImageSource source{nullptr};
  // Leaving: pictures still arriving are dropped, not decoded into a
  // framework that is shutting down.
  if (bytes && !bytes->empty()) {
    try {
      winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
      winrt::Windows::Storage::Streams::DataWriter writer(stream);
      auto data = reinterpret_cast<uint8_t const*>(bytes->data());
      writer.WriteBytes(array_view<uint8_t const>(data, data + bytes->size()));
      co_await writer.StoreAsync(); writer.DetachStream(); stream.Seek(0);
      if (svg) {
        Media::Imaging::SvgImageSource picture;
        if (pixels) { picture.RasterizePixelWidth(pixels); picture.RasterizePixelHeight(pixels); }
        if (co_await picture.SetSourceAsync(stream) == Media::Imaging::SvgImageSourceLoadStatus::Success) source = picture;
      } else {
        // A photograph shown in a 384-tall box was decoded whole: 48 MB for a
        // phone picture, 8 MB for a screenshot. Decoded at the height it is
        // drawn, it is a tenth of that. The viewer asks without a size.
        Media::Imaging::BitmapImage picture;
        if (pixels) { picture.DecodePixelType(Media::Imaging::DecodePixelType::Physical); picture.DecodePixelHeight(pixels); }
        co_await picture.SetSourceAsync(stream); source = picture;
      }
    } catch (...) { logf("picture could not be decoded"); }
  }
  if (exiting || epoch!=pictureEpoch) co_return;
  uint64_t rasterBytes=uint64_t(std::max(1,pixels?pixels:36))*std::max(1,pixels?pixels:36)*4;
  if(auto bitmap=source.try_as<Media::Imaging::BitmapSource>())rasterBytes=uint64_t(bitmap.PixelWidth())*bitmap.PixelHeight()*4;
  bool retain=source && decodedBudget.Retain(url,rasterBytes,[this](auto const& key){shownImages.erase(key);});
  if (missingImages.size() > 512) missingImages.clear();
  if (retain) shownImages.insert_or_assign(url, source); else if(!source) missingImages.insert_or_assign(url, GetTickCount64());
  auto waiting = std::move(waitingImages[url]); waitingImages.erase(url);
  for (auto& use : waiting) use(source);
}
void ResetAccountView() {
  // A channel id can be shared by two accounts. Keeping it selected across
  // sign-out prevented the new model from ever opening its own history.
  composerLoading=true; if (composer) composer.Text(L""); composerLoading=false;
  peopleRows.clear();peopleMetadata.clear();peopleSignature.clear();
  chatActive.clear(); chatTab="server"; chatSeen=0; pendingSent.clear(); sendWaiting=false;
  replyingTo=0; editingId=0; openedRead=0; accountPhoto.clear();
  ++pictureEpoch; shownImages.clear(); decodedBudget.Clear(); waitingImages.clear(); missingImages.clear(); arrivals.clear();
  for(auto const& file:pendingFiles) if(file.cancel) *file.cancel=true;
  pendingFiles.clear(); uploadError.clear();
}
// One Twemoji as an Image of `size`, or its character when the set has none.
UIElement EmojiPicture(std::string const& emoji, std::string const& file, double size) {
  Grid holder; holder.Width(size); holder.Height(size);
  Image image; image.Width(size); image.Height(size); image.Stretch(Stretch::Uniform);
  Automation::AutomationProperties::SetName(image, net::Wide(emoji));
  holder.Children().Append(image);
  double scale = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1.0;
  LoadPicture(std::string(app::TwemojiBase) + file + ".svg", true, [image, holder, emoji, size](ImageSource source) {
    if (source) { image.Source(source); return; }
    holder.Children().Clear(); auto t = Text(net::Wide(emoji), size * 0.75, Foreground); holder.Children().Append(t);
  }, int(std::lround(size * scale)));
  return holder;
}

// The face with the person's photo when there is one (Avatar.tsx): the same
// address as the web, versioned by the avatar field so a new photo is a new
// picture; the initial stays underneath until it arrives, and stays if it fails.
Border PhotoFace(app::ChatPerson const& p, double size, bool present = true, bool presenceCutout = false) {
  auto face = Face(PersonName(p), size, present);
  Shapes::Path silhouette{nullptr};
  if (presenceCutout) {
    auto initial = face.Child().as<TextBlock>(); initial.FontSize(13); initial.LineHeight(18); initial.CharacterSpacing(-20);
    // Two arcs trace the face minus the dot's 7-DIP hole. Unlike a painted
    // ring this stays transparent on selected and hovered rows too.
    double d = std::sqrt(2.0) * 11.328;
    double a = (16 * 16 - 7 * 7 + d * d) / (2 * d);
    double h = std::sqrt(16 * 16 - a * a);
    float hi = float(16 + (a + h) / std::sqrt(2.0)), lo = float(16 + (a - h) / std::sqrt(2.0));
    PathFigure outline; outline.StartPoint({hi, lo}); outline.IsClosed(true);
    ArcSegment rim; rim.Point({lo, hi}); rim.Size({16, 16}); rim.IsLargeArc(true); rim.SweepDirection(SweepDirection::Counterclockwise);
    ArcSegment notch; notch.Point({hi, lo}); notch.Size({7, 7}); notch.IsLargeArc(false); notch.SweepDirection(SweepDirection::Clockwise);
    outline.Segments().Append(rim); outline.Segments().Append(notch);
    PathGeometry shape; shape.Figures().Append(outline);
    silhouette = Shapes::Path(); silhouette.Data(shape); silhouette.Fill(face.Background());
    Grid layers; auto letter = face.Child(); face.Child(nullptr); face.Background(Tint(0, 0));
    layers.Children().Append(silhouette); layers.Children().Append(letter); face.Child(layers);
  }
  if (p.avatar.empty() || p.username.empty()) return face;
  auto url = "/api/people/" + app::ChatEscape(p.username) + "/avatar?v=" + app::ChatEscape(p.avatar);
  LoadPicture(url, false, [face, silhouette](ImageSource s) {
    if (!s) return;
    Media::ImageBrush photo; photo.ImageSource(s); photo.Stretch(Stretch::UniformToFill);
    if (silhouette) {
      auto layers = face.Child().as<Grid>();
      if (layers.Children().Size() > 1) layers.Children().RemoveAt(1);
      silhouette.Fill(photo);
    } else { face.Child(nullptr); face.Background(photo); }
  }, app::AvatarDecodePixels(size,root && root.XamlRoot()?root.XamlRoot().RasterizationScale():1.0));
  return face;
}

// ---------------------------------------------------------------------------
// The shell: three columns, as on the web. 256 for channels (the islands end
// 4 inside it, the handle's gap is the next 8), the conversation, and 240 for
// the people, each island 8 from the window's edge.
// ---------------------------------------------------------------------------
double channelWidth=256;
bool channelWidthLoaded=false;
void SetChannelWidth(double width, bool remember=false) {
  channelWidth=208+8*std::clamp(int(std::lround((width-208)/8)),0,30);
  if (root.ColumnDefinitions().Size()) root.ColumnDefinitions().GetAt(0).Width(GridLength{channelWidth,GridUnitType::Pixel});
  if (remember && chatOutput.empty() && designOutput.empty() && callOutput.empty()) {
    try { auto dir=app::DataDirectory(); std::filesystem::create_directories(dir); std::ofstream(dir/L"column-width") << channelWidth; }
    catch (...) { logf("column width could not be saved"); }
  }
}
double channelResizePosition=256;
void ChannelResizeBegin() { channelResizePosition=channelWidth; }
void ChannelResizeTo(double delta) { channelResizePosition+=delta; SetChannelWidth(channelResizePosition); }
void ChannelResizeEnd() { SetChannelWidth(channelWidth,true); }
Primitives::Thumb ChannelResizer() {
  Primitives::Thumb handle; HarmonyPointer(handle,winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast);
  handle.Name(L"ChannelResizer"); handle.Width(8); handle.HorizontalAlignment(HorizontalAlignment::Right); handle.Margin(Thickness{0,0,-4,0});
  handle.IsTabStop(true); handle.UseSystemFocusVisuals(false);
  Automation::AutomationProperties::SetName(handle,L"largura da coluna");
  handle.Template(Markup::XamlReader::Load(LR"(<ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TargetType="Thumb"><Grid Background="Transparent"><VisualStateManager.VisualStateGroups><VisualStateGroup x:Name="CommonStates"><VisualState x:Name="Normal"/><VisualState x:Name="PointerOver"><VisualState.Setters><Setter Target="Line.Opacity" Value="1"/></VisualState.Setters></VisualState><VisualState x:Name="Pressed"><VisualState.Setters><Setter Target="Line.Opacity" Value="1"/></VisualState.Setters></VisualState></VisualStateGroup><VisualStateGroup x:Name="FocusStates"><VisualState x:Name="Unfocused"/><VisualState x:Name="Focused"><VisualState.Setters><Setter Target="Line.Opacity" Value="1"/></VisualState.Setters></VisualState></VisualStateGroup></VisualStateManager.VisualStateGroups><Border x:Name="Line" Width="2" Background="#52525b" Opacity="0"/></Grid></ControlTemplate>)").as<ControlTemplate>());
  handle.DragStarted([this](auto const&,auto const&) { ChannelResizeBegin(); });
  handle.DragDelta([this](auto const&,Primitives::DragDeltaEventArgs const& e) { ChannelResizeTo(e.HorizontalChange()); });
  handle.DragCompleted([this](auto const&,auto const&) { ChannelResizeEnd(); });
  handle.DoubleTapped([this](auto const&,Input::DoubleTappedRoutedEventArgs const& e) { SetChannelWidth(256,true); e.Handled(true); });
  handle.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    if(e.Key()==VirtualKey::Left || e.Key()==VirtualKey::Right) { SetChannelWidth(channelWidth+(e.Key()==VirtualKey::Right?8:-8),true); e.Handled(true); }
  });
  Canvas::SetZIndex(handle,5); return handle;
}
void TabHover(Border const& tab,std::string const& key,bool hovered) {
  tab.Background(chatTab==key?Brush(Default):hovered?Brush(Surface2):Tint(0,0));
}
static void AuthorHover(TextBlock const& name,bool hovered) {
  name.TextDecorations(hovered?winrt::Windows::UI::Text::TextDecorations::Underline:winrt::Windows::UI::Text::TextDecorations::None);
}
static void MessageHoverVisual(Border const& band,Border const& holder,bool mentioned,bool hovered) {
  if(!mentioned)band.Background(hovered?Brush(Surface):Tint(0,0));
  holder.Opacity(hovered?1:0);holder.IsHitTestVisible(hovered);
}
void OpenOwnProfile() {
  app::ChatPerson self;
  service->ChatModel().Inspect([&](app::ChatState const& s) {for(auto const& p:s.members)if(p.username==s.owner)self=p;});
  if(!self.username.empty())OpenProfile(self);
}
void ShellPage() {
  CloseSearch();
  roomsSignature.clear(); barSignature.clear(); callSignature.clear(); callView = nullptr; callFaces.clear();
  channelsSignature.clear(); peopleSignature.clear(); chatSeen = 0;
  // A new list and a new box: the old scroll hook belongs to the list that
  // went (Ajustes and back), and the box starts empty.
  messageScroll = nullptr; lastSeenNewest = 0; editingId = 0; editSent = false; CloseMentions();
  showingRooms = true; page = nullptr; root.Children().Clear(); root.ColumnDefinitions().Clear(); root.RowDefinitions().Clear();
  if (!channelWidthLoaded) {
    channelWidthLoaded=true;
    if(chatOutput.empty() && designOutput.empty() && callOutput.empty()) {
      try { double saved=256; if(std::ifstream(app::DataDirectory()/L"column-width") >> saved) SetChannelWidth(saved); } catch (...) {}
    }
  }
  for (auto width : {GridLength{channelWidth, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{peopleShown ? 248.0 : 0.0, GridUnitType::Pixel}}) {
    ColumnDefinition c; c.Width(width); root.ColumnDefinitions().Append(c);
  }
  if (!dropHooked) { dropHooked = true; HookDrop(root); }
  root.Children().Append(ChannelColumn()); root.Children().Append(ChannelResizer());
  auto middle = ConversationColumn(); Grid::SetColumn(middle, 1); root.Children().Append(middle);
  auto people = PeopleColumn(); people.Visibility(peopleShown ? Visibility::Visible : Visibility::Collapsed); Grid::SetColumn(people, 2); root.Children().Append(people);
  status = Text(L"", 13, Muted); error = Text(L"", 13, DangerSoftFg); // kept for the voice code paths
  if (!chatActive.empty() && service) LoadDraft();
}

// tools/harmony-icon.svg, viewBox 96 101 903 903: the same three paths, and
// they change together. The geometry is scaled, not the drawing (VoiceIcon).
UIElement HarmonyMark(double size) {
  static const wchar_t* shapes[][2] = {
    {L"#051939", L"M274.73 101.43C292.5 100.17 310.68 101.18 328.5 101.18C361.5 101.19 394.5 101.12 427.5 101.17C520.17 101.3 612.83 100.95 705.5 101.2C730.16 101.27 754.83 101.25 779.5 101.21C797.06 101.18 815.01 100.17 832.5 102.08C860.74 105.17 888.24 114.95 912.05 130.44C955.57 158.76 986.76 204.99 994.83 256.52C997.47 273.33 996.9 290.54 996.8 307.5C996.68 327.83 996.85 348.17 996.72 368.5C995.92 503.16 997.02 637.84 996.59 772.5C996.51 797.32 998.2 822.74 994.65 847.37C987.93 893.9 960.86 937.81 923.49 966.01C898.71 984.7 869.32 997.56 838.44 1001.64C814.74 1004.78 790.35 1003.33 766.5 1003.3C718.84 1003.22 671.17 1003.26 623.5 1003.38C537.17 1003.61 450.83 1002.8 364.5 1003.58C344.17 1003.76 323.83 1003.78 303.5 1003.75C287.12 1003.73 270.8 1004.03 254.53 1001.76C203.74 994.65 157.64 963.47 129.33 921.21C113.12 897.01 102.42 868.36 99.38 839.38C97.29 819.36 98.59 798.6 98.66 778.5C98.79 746.17 98.75 713.83 98.64 681.5C98.51 637.5 98.57 593.5 98.6 549.5C98.64 483.17 98.11 416.83 98.49 350.5C98.6 332.5 98.51 314.5 98.49 296.5C98.48 278.26 98.73 260.37 103.17 242.56C117.46 185.26 159.58 136.02 214.72 114.23C234.08 106.58 254.05 102.91 274.73 101.43Z"},
    {L"#30b4fd", L"F1M346.68 327.38C358.02 325.77 370.72 326.33 381.71 329.69C395.16 333.81 407.08 340.4 417.01 350.48C422.95 356.52 427.64 363.66 431.21 371.32C454.38 421.08 421.82 479.98 367.44 486.67C355.99 488.08 343.61 487.27 332.55 483.9C322.81 480.93 312.85 476.13 305.09 469.44C297.16 462.58 290.14 454.52 285.43 445.1C259.86 394.04 290.28 335.38 346.68 327.38ZM460.85 651.1C462.95 656.77 470.85 662.04 475.4 666.09C490.05 679.11 505.64 690.63 521.84 701.62C573.63 736.72 634.76 758.97 696.7 766.56C712.7 768.53 729.4 769.07 745.5 768.42C753.97 768.07 762.54 767.1 771.01 767.21C764.22 774.6 746.1 785.09 736.98 790.48C705.23 809.23 671.46 823.58 635.53 832.1C533.76 856.25 432.36 843.87 343.17 788.34C319.06 773.32 297.35 754.2 278.03 733.48C253.89 707.62 224.1 667.09 216.37 632.25C213.55 619.51 212.38 606.56 214.31 593.58C222.79 536.56 276.87 501.81 332.54 513.8C344.92 516.47 357.7 521.96 367.86 529.62C386.2 543.43 393.69 558.28 405.15 577.33C414.46 592.81 424.41 607.8 435.32 622.2C440.79 629.42 453.35 647.27 460.85 651.1Z"},
    {L"#70f8b7", L"F1M725.69 326.41C737.23 325.11 749.69 326.04 760.76 329.66C772.17 333.39 782.37 339.15 791.31 347.16C796.83 352.09 801.21 358.11 805.13 364.34C836.38 413.94 801.07 480.04 744.5 486.8C732.5 488.24 719.26 487.53 707.74 483.7C697.52 480.32 687.62 475.1 679.51 467.98C672.2 461.57 666.56 454.13 661.85 445.66C656.26 435.64 653.29 424.06 652.55 412.65C649.68 368.08 681.65 331.35 725.69 326.41ZM784 510.8C785.67 510.83 787.33 510.87 789 510.91C798.31 512.56 807.38 513.87 816.31 517.23C841.33 526.65 862.11 546.82 871.54 571.93C875.53 582.56 876.57 592.9 878.08 604C878.08 606.67 878.07 609.33 878.06 612C876.01 623.99 874.62 635.38 870.47 646.95C859.08 678.7 831.5 712.6 807.6 736.08C797.58 745.92 783.01 760.33 771.01 767.21C762.54 767.1 753.97 768.07 745.5 768.42C729.4 769.07 712.7 768.53 696.7 766.56C634.76 758.97 573.63 736.72 521.84 701.62C505.64 690.63 490.05 679.11 475.4 666.09C470.85 662.04 462.95 656.77 460.85 651.1C466.57 651.35 472.28 651.6 478 651.86C480 651.89 482 651.93 484 651.96C488.67 651.96 493.33 651.96 498 651.96C500 651.92 502 651.87 504 651.83C517.01 652.42 530.88 650.03 543.55 647.23C586.62 637.68 623.82 616.22 656.87 587.39C682.84 564.73 703.57 534.84 735.97 520.49C751.69 513.53 767.06 511.74 784 510.8Z"},
  };
  double s = size / 903.0;
  Canvas box; box.Width(size); box.Height(size);
  for (auto const& [fill, data] : shapes) {
    auto path = Markup::XamlReader::Load(std::wstring(L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Fill='") + fill + L"' Data='" + data + L"'/>").as<Shapes::Path>();
    MatrixTransform scaled; scaled.Matrix(Media::Matrix{s, 0, 0, s, -96 * s, -101 * s}); path.Data().Transform(scaled);
    box.Children().Append(path);
  }
  return box;
}

Grid ChannelColumn() {
  Grid column; column.Name(L"ChannelColumn");
  for (auto h : {GridLength{64, GridUnitType::Pixel}, GridLength{0, GridUnitType::Auto}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) {
    RowDefinition r; r.Height(h); column.RowDefinitions().Append(r);
  }
  // "harmony", 20/28 at 600, 24 in (px-6), centred in the 64.
  auto wordmark = Line(L"harmony", 20, Foreground, 600); wordmark.CharacterSpacing(-15);
  wordmark.Margin(Thickness{24, 0, 24, 0}); wordmark.VerticalAlignment(VerticalAlignment::Center); column.Children().Append(wordmark);

  // Servidor | Mensagens: an island with padding 8, two 112x36 tabs 4 apart.
  auto tabs = Island(); tabs.Name(L"TabsIsland"); tabs.Margin(Thickness{8, 0, 4, 0}); tabs.Padding(Thickness{8, 8, 8, 8});
  Grid tabRow; tabRow.ColumnSpacing(4);
  for (int i = 0; i < 2; ++i) { ColumnDefinition c; c.Width(GridLength{1, GridUnitType::Star}); tabRow.ColumnDefinitions().Append(c); }
  auto tab = [&](std::wstring const& label, std::string const& key, int col) {
    Border b; b.Height(36); b.CornerRadius(CornerRadius{8, 8, 8, 8});
    auto t = Line(label, 16, Muted, 500); t.HorizontalAlignment(HorizontalAlignment::Center); t.VerticalAlignment(VerticalAlignment::Center);
    b.Child(t); Grid::SetColumn(b, col);
    b.PointerEntered([this, key](auto const& eventSender,auto const&) { auto b = eventSender.template as<Border>(); TabHover(b,key,true); });
    b.PointerExited([this, key](auto const& eventSender,auto const&) { auto b = eventSender.template as<Border>(); TabHover(b,key,false); });
    HarmonyPointer(b); b.Tapped([this, key](auto const&, auto const&) { if (chatTab != key) { chatTab = key; channelsSignature.clear(); FollowTab(); Render(); } });
    Automation::AutomationProperties::SetName(b, label); tabRow.Children().Append(b); return b;
  };
  tabServer = tab(L"Servidor", "server", 0); tabDirect = tab(L"Mensagens", "dm", 1);
  tabs.Child(tabRow); Grid::SetRow(tabs, 1); column.Children().Append(tabs);

  // The channel island: text channels, then the voice rooms with who is in
  // them (roomList, drawn by the voice code as before).
  auto list = Island(); list.Name(L"RoomsIsland"); list.Margin(Thickness{8, 8, 4, 0});
  StackPanel body; body.Padding(Thickness{8, 8, 8, 8});
  // The rows and, over them, the line that shows where a drag would land.
  Grid channelHost; channelList = StackPanel(); channelHost.Children().Append(channelList); SetupDragLayer(channelHost);
  body.Children().Append(channelHost);
  roomList = StackPanel(); body.Children().Append(roomList);
  ScrollViewer scroll; scroll.Content(body); ImmediateMouseWheel(scroll); ScrollLaneWhenNeeded(scroll); list.Child(scroll); Grid::SetRow(list, 2); column.Children().Append(list);

  // The account island: the call bar when in a room, then face, name, the
  // two voice toggles and the gear (py-2, pl-4 pr-2, row of 32).
  auto account = Island(); account.Name(L"SettingsIsland"); account.Margin(Thickness{8, 8, 4, 8}); account.Padding(Thickness{0, 8, 0, 8});
  StackPanel accountBody;
  voiceBar = StackPanel(); voiceBar.Spacing(0); accountBody.Children().Append(voiceBar);
  Grid row; row.Height(32); row.Margin(Thickness{16, 0, 8, 0}); row.ColumnSpacing(4);
  for (auto w : {GridLength{32, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}, GridLength{0, GridUnitType::Auto}}) {
    ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c);
  }
  accountFace = Grid(); row.Children().Append(accountFace);
  auto ownProfile=[this](auto const&,Input::TappedRoutedEventArgs const& e) {e.Handled(true);OpenOwnProfile();};
  accountFace.Tapped(ownProfile); HarmonyPointer(accountFace);
  // Microphone and hearing sit on this line while in a room (Voice.tsx
  // VoiceToggles), before the gear; outside a room they are not drawn.
  accountToggles = StackPanel(); accountToggles.Orientation(Orientation::Horizontal); accountToggles.Spacing(4);
  Grid::SetColumn(accountToggles, 2); row.Children().Append(accountToggles);
  accountName = Line(L"", 16, Foreground, 500); accountName.Margin(Thickness{2, 0, 0, 0});
  accountName.Tapped(ownProfile); HarmonyPointer(accountName); accountName.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(accountName, 1); row.Children().Append(accountName);
  auto gear = Square("gear", L"Ajustes", 32, 20, Foreground, Surface3, [this] { SettingsPage(); });
  gear.Background(Brush(Surface2)); gear.Name(L"AccountSettings"); accountSettings = gear;
  Grid::SetColumn(gear, 3); row.Children().Append(gear);
  accountBody.Children().Append(row); account.Child(accountBody);
  Grid::SetRow(account, 3); column.Children().Append(account);
  return column;
}
Grid accountFace{nullptr}; TextBlock accountName{nullptr}; StackPanel accountToggles{nullptr};

// One row of the channel list: 36 tall, 8 in, the mark in a 20 box, the name
// at 16 (600 when chosen, 500 muted otherwise), and the chosen one on default
// with a 3px accent stroke on the island's edge.
UIElement ChannelRow(std::wstring const& name, std::string const& icon, bool chosen, int64_t unread, int64_t mentions,
                     bool muted, std::function<void()> open, UIElement face = nullptr) {
  Grid line; line.Height(face ? 40 : 36); line.Margin(Thickness{0, 0, 0, 2});
  Border pill; pill.CornerRadius(CornerRadius{8, 8, 8, 8}); pill.Background(chosen ? Brush(Default) : Tint(0, 0)); pill.Padding(Thickness{8, 0, 8, 0});
  Grid content; content.ColumnSpacing(6);
  for (auto w : {GridLength{face ? 32.0 : 18.0, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) {
    ColumnDefinition c; c.Width(w); content.ColumnDefinitions().Append(c);
  }
  bool bright = chosen || (unread > 0 && !muted);
  auto mark = (face ? face : VoiceIcon(icon, bright ? Foreground : Muted, 18)).as<FrameworkElement>(); mark.VerticalAlignment(VerticalAlignment::Center); content.Children().Append(mark);
  auto text = Line(name, 16, bright ? Foreground : Muted, uint16_t(chosen || (unread > 0 && !muted) ? 600 : 500));
  text.CharacterSpacing(-15);
  text.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(text, 1); content.Children().Append(text);
  StackPanel tail; tail.Orientation(Orientation::Horizontal); tail.Spacing(6); tail.VerticalAlignment(VerticalAlignment::Center);
  if (muted) tail.Children().Append(VoiceIcon("bell-slash", bright ? Foreground : Muted, 14));
  // Every DM is addressed to you: it uses the plain unread count, never the
  // channel's @ pill. Ordinary channel unread counts must remain visible too.
  bool mention = !face && mentions > 0;
  auto count = mention ? mentions : unread;
  if (count > 0) {
    Border badge; badge.Name(mention ? L"MentionBadge" : L"UnreadBadge"); badge.MinWidth(24); badge.Height(24);
    badge.CornerRadius(CornerRadius{12, 12, 12, 12}); badge.Background(mention ? Tint(Accent, .12) : Brush(Foreground)); badge.Padding(Thickness{8, 0, 8, 0});
    StackPanel value; value.Orientation(Orientation::Horizontal); value.Spacing(2); value.HorizontalAlignment(HorizontalAlignment::Center); value.VerticalAlignment(VerticalAlignment::Center);
    if (mention) value.Children().Append(VoiceIcon("at", 0x61a8fc, 12));
    auto n = Line(count > 99 ? L"99+" : std::to_wstring(count), mention ? 13 : 14, mention ? 0x61a8fc : Surface, 600);
    n.CharacterSpacing(mention ? 60 : -20); value.Children().Append(n);
    badge.Child(value); tail.Children().Append(badge);
  }
  Grid::SetColumn(tail, 2); content.Children().Append(tail);
  pill.Child(content); line.Children().Append(pill);
  if (chosen) { Border stroke; stroke.Width(3); stroke.Height(line.Height()); stroke.CornerRadius(CornerRadius{0,2,2,0}); stroke.Background(Brush(Accent)); stroke.HorizontalAlignment(HorizontalAlignment::Left); stroke.Margin(Thickness{-8, 0, 0, 0}); line.Children().Append(stroke); }
  line.PointerEntered([pill, text, chosen](auto const&, auto const&) { if (!chosen) { pill.Background(Brush(Surface2)); text.Foreground(Brush(Foreground)); } });
  line.PointerExited([pill, text, chosen, bright](auto const&, auto const&) { if (!chosen) { pill.Background(Tint(0, 0)); text.Foreground(Brush(bright ? Foreground : Muted)); } });
  HarmonyPointer(line); line.Tapped([open](auto const&, auto const&) { open(); });
  Automation::AutomationProperties::SetName(line, name);
  return line;
}
// A tab whose list does not hold the open conversation opens its first one
// (App.tsx, the tabs' onClick): otherwise "Mensagens" showed #geral beside
// the list of DMs. An empty tab leaves the conversation where it is.
void FollowTab() {
  std::vector<app::ChatConversation> list;
  service->ChatModel().Inspect([&](app::ChatState const& s) { list = s.conversations; });
  app::ChatOrderConversations(list);
  bool dm = chatTab == "dm"; std::string first; bool inside = false;
  for (auto const& c : list) {
    if (c.hidden || (c.kind == "dm") != dm) continue;
    if (first.empty()) first = c.id;
    if (c.id == chatActive) inside = true;
  }
  if (!inside && !first.empty()) OpenConversation(first);
}
// "CANAIS" with its +: 12/16 at 600, .06em, muted; the square is 32 at x=212.
UIElement SectionLabel(std::wstring const& label) {
  Grid row; row.Height(32); row.Margin(Thickness{0, 0, 0, 2});
  auto t = Line(label, 13, Muted, 600); t.CharacterSpacing(60); t.Margin(Thickness{8, 0, 0, 0}); t.VerticalAlignment(VerticalAlignment::Center);
  row.Children().Append(t); return row;
}

MenuFlyout ConversationMenu(app::ChatConversation const& c, bool manage = false) {
  MenuFlyout menu;
  MenuFlyoutItem mute; HarmonyPointer(mute); mute.Text(c.muted ? L"Voltar a avisar" : L"Silenciar");
  mute.Click([this, id = c.id, on = !c.muted](auto const&, auto const&) {
    service->ChatWork([id, on](app::Chat& chat) { chat.Mute(id,on); });
  });
  menu.Items().Append(mute);
  if (c.kind == "text" && manage) {
    menu.Items().Append(ColumnItem(L"Renomear canal", [this, id = c.id, name = c.name] { RenameRoom(id, name); }));
    menu.Items().Append(ColumnItem(L"Apagar canal", [this, id = c.id, name = c.name] { RemoveRoom(id, name, false); }, true));
  }
  if (c.kind == "dm") {
    if (!c.peer.username.empty()) for (auto const& action : PersonActions(c.peer)) {
      MenuFlyoutItem item; HarmonyPointer(item); item.Text(action.label); item.Click([pick=action.pick](auto const&,auto const&) { pick(); }); menu.Items().Append(item);
    }
    MenuFlyoutItem hide; HarmonyPointer(hide); hide.Text(L"Fechar conversa");
    hide.Style(Resources().Lookup(box_value(L"HarmonyDangerMenuItem")).as<Style>());
    hide.Click([this, id = c.id](auto const&, auto const&) { CloseConversation(id); });
    menu.Items().Append(hide);
  }
  return menu;
}
void CloseConversation(std::string const& id) {
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  service->ChatWork([id, dispatcher, weak](app::Chat& chat) {
    chat.Hide(id);
    bool hidden = false;
    chat.Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.id == id) hidden = c.hidden; });
    if (!hidden) return;
    dispatcher.TryEnqueue([id, weak] { if (auto self = weak.get(); self && !self->exiting && self->chatActive == id) {
      std::vector<app::ChatConversation> list;
      self->service->ChatModel().Inspect([&](app::ChatState const& s) { list = s.conversations; });
      app::ChatOrderConversations(list);
      auto next = std::find_if(list.begin(),list.end(),[](auto const& c) { return !c.hidden && c.kind == "dm"; });
      if (next == list.end()) next = std::find_if(list.begin(),list.end(),[](auto const& c) { return !c.hidden && c.kind == "text"; });
      if (next != list.end()) { self->chatTab = next->kind == "dm" ? "dm" : "server"; self->OpenConversation(next->id); }
    }});
  });
}

// ---------------------------------------------------------------------------
// D12b: the column's folders, names and order, and pinned messages — what the
// web got in the D12a (App.tsx, reorder.ts, Pins.tsx), drawn the native way.
// ---------------------------------------------------------------------------

// Which folders are closed is this computer's own business, like the web's
// localStorage: one id per line beside the column width.
std::set<std::string> closedFolders; bool closedFoldersLoaded = false;
std::filesystem::path ClosedFoldersFile() { return app::DataDirectory() / L"pastas-fechadas.txt"; }
void ToggleFolder(std::string const& id) {
  if (dragSwallow) return;
  if (!closedFolders.erase(id)) closedFolders.insert(id);
  try { auto dir = app::DataDirectory(); std::filesystem::create_directories(dir); std::ofstream out(ClosedFoldersFile()); for (auto const& f : closedFolders) out << f << '\n'; } catch (...) {}
  channelsSignature.clear(); Render();
}

// NewChannel.tsx channelName: the server takes lower-case letters, digits and
// hyphens, so the name is shaped as it is typed rather than refused after.
static std::wstring ShapeChannelName(std::wstring const& typed) {
  std::wstring plain = typed;
  if (int n = NormalizeString(NormalizationD, typed.c_str(), -1, nullptr, 0); n > 0) {
    std::wstring d(size_t(n), L'\0'); n = NormalizeString(NormalizationD, typed.c_str(), -1, d.data(), n);
    if (n > 0) { d.resize(size_t(n)); if (!d.empty() && d.back() == L'\0') d.pop_back(); plain = d; }
  }
  std::wstring out;
  for (wchar_t c : plain) {
    if (c >= 0x300 && c <= 0x36f) continue;
    if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
    if (c == L' ' || c == L'_' || c == L'\t') c = L'-';
    if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) continue;
    if (c == L'-' && (out.empty() || out.back() == L'-')) continue;
    out += c;
  }
  if (out.size() > 32) out.resize(32);
  return out;
}

// The panel that names something (NewChannel.tsx): a title, one box, the rule
// under it, the server's answer when it says no, and Cancelar | the action
// side by side. It stays open on a refusal so the name can be fixed.
Grid nameDialog{nullptr};
void CloseNameDialog() {
  if (!nameDialog) return;
  uint32_t at{}; if (root.Children().IndexOf(nameDialog, at)) root.Children().RemoveAt(at);
  nameDialog = nullptr;
}
void NameDialog(std::wstring const& title, std::wstring const& initial, std::wstring const& placeholder, bool free,
                std::wstring const& action, std::function<std::string(app::Chat&, std::string const&)> work) {
  CloseNameDialog();
  Grid dialog; dialog.Name(L"NameDialog"); dialog.Background(Tint(0,.7)); dialog.TabFocusNavigation(Input::KeyboardNavigationMode::Cycle);
  Automation::AutomationProperties::SetName(dialog, title); Grid::SetColumnSpan(dialog,3); Canvas::SetZIndex(dialog,1000);
  Border card; card.Name(L"BackgroundElement"); card.Background(Brush(Surface)); card.Padding(Thickness{24,24,24,24}); card.CornerRadius(CornerRadius{8,8,8,8});
  card.Width(400); card.HorizontalAlignment(HorizontalAlignment::Center); card.VerticalAlignment(VerticalAlignment::Center);
  StackPanel panel; panel.Spacing(0);
  Grid heading; ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); heading.ColumnDefinitions().Append(words);
  ColumnDefinition shut; shut.Width(GridLength{0,GridUnitType::Auto}); heading.ColumnDefinitions().Append(shut);
  auto head = Line(title,20,Foreground,600); head.VerticalAlignment(VerticalAlignment::Center); heading.Children().Append(head);
  auto close = Square("xmark",L"Fechar",32,16,Foreground,Surface2,[this] { CloseNameDialog(); }); Grid::SetColumn(close,1); heading.Children().Append(close);
  panel.Children().Append(heading);
  auto label = Line(L"NOME",13,Muted,600); label.CharacterSpacing(60); label.Margin(Thickness{0,16,0,8}); panel.Children().Append(label);
  TextBox box = HarmonyTextBox(); box.Name(L"NameBox"); box.Background(Brush(Surface2)); box.FontSize(16); box.MaxLength(32);
  box.PlaceholderText(placeholder); box.Text(initial); Automation::AutomationProperties::SetName(box, title); panel.Children().Append(box);
  auto hint = Text(free ? L"Até 32 caracteres. A categoria aparece em maiúsculas na lista." : L"Letras minúsculas, números e traço. Espaço vira traço.", 13, Muted);
  hint.Margin(Thickness{0,8,0,0}); panel.Children().Append(hint);
  auto problem = Text(L"", 13, DangerSoftFg); problem.Name(L"NameProblem"); problem.Margin(Thickness{0,8,0,0}); problem.Visibility(Visibility::Collapsed); panel.Children().Append(problem);
  if (!free) box.TextChanged([](auto const& eventSender, auto const&) { auto box = eventSender.template as<TextBox>();
    auto typed = std::wstring(box.Text()); auto shaped = ShapeChannelName(typed);
    if (shaped != typed) { box.Text(shaped); box.SelectionStart(int32_t(shaped.size())); }
  });
  Grid buttons; buttons.ColumnSpacing(8); buttons.Margin(Thickness{0,24,0,0});
  for (int i = 0; i < 2; ++i) { ColumnDefinition c; c.Width(GridLength{1,GridUnitType::Star}); buttons.ColumnDefinitions().Append(c); }
  auto cancel = Action(L"Cancelar", [this] { CloseNameDialog(); }); NegativeButton(cancel);
  auto busy = std::make_shared<bool>(false);
  auto go = [this, weakBox = make_weak(box), problem, free, work, busy] {
    auto box = weakBox.get(); if (!box) return;
    auto typed = std::wstring(box.Text());
    if (free) { auto a = typed.find_first_not_of(L' '), b = typed.find_last_not_of(L' '); typed = a == std::wstring::npos ? L"" : typed.substr(a, b - a + 1); }
    else while (!typed.empty() && typed.back() == L'-') typed.pop_back();
    if (typed.empty() || *busy) return;
    *busy = true; auto name = net::Narrow(typed);
    auto dispatcher = window.DispatcherQueue(); auto weak = get_weak(); auto dialog = nameDialog;
    service->ChatWork([work, name, dispatcher, weak, problem, dialog, busy](app::Chat& chat) {
      auto error = work(chat, name);
      dispatcher.TryEnqueue([weak, error, problem, dialog, busy] {
        *busy = false; auto self = weak.get(); if (!self || self->exiting || self->nameDialog != dialog) return;
        if (error.empty()) { self->CloseNameDialog(); self->channelsSignature.clear(); self->Render(); return; }
        problem.Text(net::Wide(error)); problem.Visibility(Visibility::Visible);
      });
    });
  };
  auto save = Action(action, go); save.Name(L"NameSave"); save.Style(Resources().Lookup(box_value(L"HarmonyLoginButton")).as<Style>());
  save.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0x3592f9));
  for (auto b : {cancel, save}) { b.Height(44); b.HorizontalAlignment(HorizontalAlignment::Stretch); }
  Grid::SetColumn(save,1); buttons.Children().Append(cancel); buttons.Children().Append(save); panel.Children().Append(buttons);
  box.KeyDown([go](auto const&, Input::KeyRoutedEventArgs const& e) { if (e.Key() == winrt::Windows::System::VirtualKey::Enter) { go(); e.Handled(true); } });
  dialog.KeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) { if (e.Key() == winrt::Windows::System::VirtualKey::Escape) { CloseNameDialog(); e.Handled(true); } });
  dialog.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([this,card](auto const&,Input::PointerRoutedEventArgs const& e) {
    auto p=e.GetCurrentPoint(card).Position();
    if(p.X<0 || p.Y<0 || p.X>card.ActualWidth() || p.Y>card.ActualHeight()) { CloseNameDialog(); e.Handled(true); }
  })),true);
  box.Loaded([](auto const& eventSender, auto const&) { auto box = eventSender.template as<TextBox>(); box.Focus(FocusState::Programmatic); box.SelectAll(); });
  card.Child(panel); dialog.Children().Append(card); nameDialog = dialog; root.Children().Append(dialog);
}

// A refusal that has no panel of its own to show in (a drag the server
// turned down, a pin past the ceiling): the confirmation look, one button.
void ShowChatProblem(std::wstring const& text) {
  ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark); dialog.Name(L"ChatProblemDialog");
  ConfirmLook(dialog);
  dialog.Title(Line(L"Não deu certo", 20, Foreground, 600));
  dialog.Content(Text(text, 15, Muted)); dialog.CloseButtonText(L"Fechar");
  dialog.ShowAsync();
}
void ReportChatProblem(std::string const& error) {
  if (error.empty()) return;
  auto weak = get_weak();
  window.DispatcherQueue().TryEnqueue([weak, error] { if (auto self = weak.get(); self && !self->exiting) self->ShowChatProblem(net::Wide(error)); });
}

MenuFlyoutItem ColumnItem(std::wstring const& label, std::function<void()> pick, bool danger = false) {
  MenuFlyoutItem item; HarmonyPointer(item); item.Text(label);
  if (danger) item.Style(Resources().Lookup(box_value(L"HarmonyDangerMenuItem")).as<Style>());
  item.Click([pick](auto const&, auto const&) { pick(); });
  return item;
}
void NewFolder() {
  NameDialog(L"Nova categoria", L"", L"Jogos", true, L"Criar", [](app::Chat& chat, std::string const& name) { return chat.CreateCategory(name); });
}
void RenameRoom(std::string const& id, std::string const& name) {
  NameDialog(L"Renomear canal", net::Wide(name), L"musica", false, L"Salvar",
             [id](app::Chat& chat, std::string const& typed) { return chat.RenameChannel(id, typed); });
}
// The column heading's menu and a folder's: the owner's alone. Deleting a
// folder deletes nothing in it (the rooms go to the top), so it does not ask.
MenuFlyout ColumnMenu() { MenuFlyout menu; menu.Items().Append(ColumnItem(L"Nova categoria", [this] { NewFolder(); })); return menu; }
MenuFlyout FolderMenu(app::ChatCategory const& f) {
  MenuFlyout menu;
  menu.Items().Append(ColumnItem(L"Renomear categoria", [this, f] {
    NameDialog(L"Renomear categoria", net::Wide(f.name), L"Jogos", true, L"Salvar",
               [id = f.id](app::Chat& chat, std::string const& name) { return chat.RenameCategory(id, name); });
  }));
  menu.Items().Append(ColumnItem(L"Nova categoria", [this] { NewFolder(); }));
  menu.Items().Append(ColumnItem(L"Apagar categoria", [this, id = f.id] {
    service->ChatWork([this, id](app::Chat& chat) { ReportChatProblem(chat.RemoveCategory(id)); });
  }, true));
  return menu;
}
MenuFlyout RoomMenu(std::string const& id, std::string const& name) {
  MenuFlyout menu; menu.Items().Append(ColumnItem(L"Renomear canal", [this, id, name] { RenameRoom(id, name); }));
  menu.Items().Append(ColumnItem(L"Apagar canal", [this, id, name] { RemoveRoom(id, name, true); }, true));
  return menu;
}
// The + beside CANAIS and VOZ (App.tsx newChannel): a text channel opens once
// it exists; a voice room only appears, as joining it is a choice.
void NewRoom(bool voice) {
  NameDialog(voice ? L"Novo canal de voz" : L"Novo canal", L"", voice ? L"sala-2" : L"jogos", false, L"Criar",
             [this, voice](app::Chat& chat, std::string const& name) {
    std::string created;
    auto error = chat.CreateChannel(name, voice ? "voice" : "text", &created);
    if (error.empty() && !voice && !created.empty()) {
      auto weak = get_weak();
      window.DispatcherQueue().TryEnqueue([weak, created] { if (auto self = weak.get(); self && !self->exiting) { self->chatTab = "server"; self->OpenConversation(created); } });
    }
    return error;
  });
}
// Apagar canal (App.tsx deleting): asks first, naming the room, because the
// whole room goes for everybody at once.
void RemoveRoom(std::string const& id, std::string const& name, bool voice) {
  ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark); dialog.Name(L"RemoveRoomDialog");
  ConfirmLook(dialog);
  dialog.Title(Line(L"Apagar " + std::wstring(voice ? L"" : L"#") + net::Wide(name) + L"?", 20, Foreground, 600));
  dialog.Content(Text(voice ? L"A sala some para todo mundo."
                            : L"O canal some para todo mundo, com as mensagens. Elas não são apagadas do servidor: se foi engano, dá para trazer de volta.", 13, Muted));
  dialog.PrimaryButtonText(L"Apagar"); dialog.CloseButtonText(L"Cancelar"); dialog.DefaultButton(ContentDialogButton::Close);
  auto op = dialog.ShowAsync();
  op.Completed([this, id](auto const& result, auto) {
    if (result.GetResults() != ContentDialogResult::Primary) return;
    service->ChatWork([this, id](app::Chat& chat) { ReportChatProblem(chat.RemoveChannel(id)); });
  });
}
// A heading of the column with the owner's + at its right end.
Grid HeadingWithPlus(std::wstring const& label, std::wstring const& tip, std::function<void()> plus) {
  auto row = SectionLabel(label).as<Grid>(); row.Background(Tint(0,0));
  ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); row.ColumnDefinitions().Append(words);
  ColumnDefinition end; end.Width(GridLength{0,GridUnitType::Auto}); row.ColumnDefinitions().Append(end);
  if (plus) {
    auto add = Square("plus", tip, 32, 12, Muted, Surface2, plus); add.Name(tip == L"Novo canal" ? L"NewTextChannel" : L"NewVoiceChannel");
    ToolTipService::SetToolTip(add, box_value(tip)); Grid::SetColumn(add, 1); row.Children().Append(add);
  }
  return row;
}

// Dragging, the owner's alone (App.tsx grab/dropMark): a press that travels
// 6 px picks the row up; under the pointer a room goes before or after
// another room, or into a folder (or the top of the column, under CANAIS and
// VOZ); a folder goes before or after another folder. The whole order goes to
// the server at once (Chat::SaveLayout), which redraws first.
struct DragSpot { FrameworkElement el{nullptr}; bool folder = false; std::string id; };
std::vector<DragSpot> dragSpots;
std::vector<app::ChatPlace> dragPlaces;
std::map<std::string, StackPanel> voiceGroups;          // drawn by the voice code (main.cpp), placed here
std::map<std::string, FrameworkElement> voiceHandles;    // their title rows: what a drag picks up
uint64_t voiceGroupsVersion = 0;
std::vector<std::pair<winrt::weak_ref<FrameworkElement>, IInspectable>> dragHandlers;
bool canShape = false, dragArmed = false, dragMoving = false, dragFolder = false, dragSwallow = false;
std::string dragId; winrt::Windows::Foundation::Point dragStart{};
FrameworkElement dragElement{nullptr}; Panel dragLit{nullptr};
std::optional<app::ChatDrop> dragDrop;
Canvas dragLayer{nullptr}; Border dragBar{nullptr};

// Drop target only: CANAIS and VOZ take a room into the top of the column.
void DropSpot(FrameworkElement const& el, std::string const& folder) { dragSpots.push_back({el, true, folder}); }
void ChannelDragBegin(FrameworkElement const& spot, bool folder, std::string const& id, winrt::Windows::Foundation::Point point) {
  dragArmed = true; dragMoving = false; dragFolder = folder; dragId = id; dragElement = spot; dragStart = point; dragDrop.reset();
}
void Draggable(FrameworkElement const& spot, FrameworkElement const& handle, bool folder, std::string const& id) {
  dragSpots.push_back({spot, folder, id});
  if (!canShape) return;
  auto handler = box_value(Input::PointerEventHandler(
    [this, weak = make_weak(spot), folder, id](auto const&, Input::PointerRoutedEventArgs const& e) {
      auto point = e.GetCurrentPoint(channelList);
      if (!point.Properties().IsLeftButtonPressed()) return;
      if (auto target = weak.get()) ChannelDragBegin(target, folder, id, point.Position());
    }));
  handle.AddHandler(UIElement::PointerPressedEvent(), handler, true);
  dragHandlers.emplace_back(make_weak(handle), handler);
}
void ShowDropBar(double y) {
  if (!dragBar) return;
  dragBar.Width(channelList.ActualWidth()); Canvas::SetTop(dragBar, y - 1); dragBar.Visibility(Visibility::Visible);
}
void LightFolder(Panel const& header) {
  if (dragLit == header) return;
  if (dragLit) dragLit.Background(Tint(0,0));
  dragLit = header; if (dragLit) dragLit.Background(Tint(Accent,.12));
}
void EndDrag() {
  if (dragElement) dragElement.Opacity(1);
  if (dragBar) dragBar.Visibility(Visibility::Collapsed);
  LightFolder(nullptr);
  dragArmed = dragMoving = false; dragElement = nullptr; dragDrop.reset();
}
void ChannelDragTo(winrt::Windows::Foundation::Point p) {
  if (!dragArmed) return;
  if (!dragMoving) {
    if (std::hypot(p.X - dragStart.X, p.Y - dragStart.Y) < 6) return;
    dragMoving = true; if (dragElement) dragElement.Opacity(.5);
  }
  for (auto const& s : dragSpots) {
    if (!s.el || !s.el.Parent()) continue;
    auto r = s.el.TransformToVisual(channelList).TransformBounds({0, 0, float(s.el.ActualWidth()), float(s.el.ActualHeight())});
    if (p.Y < r.Y || p.Y >= r.Y + r.Height || r.Height <= 0) continue;
    bool upper = p.Y < r.Y + r.Height / 2;
    if (dragFolder) {
      if (!s.folder || s.id.empty() || s.id == dragId) return;
      dragDrop = app::ChatDrop{app::ChatDrop::Target::Folder, s.id, upper ? app::ChatDrop::Where::Before : app::ChatDrop::Where::After};
      LightFolder(nullptr); ShowDropBar(upper ? r.Y : r.Y + r.Height);
    } else if (s.folder) {
      dragDrop = app::ChatDrop{app::ChatDrop::Target::Folder, s.id, app::ChatDrop::Where::Into};
      if (dragBar) dragBar.Visibility(Visibility::Collapsed);
      LightFolder(s.el.try_as<Panel>());
    } else {
      if (s.id == dragId) { dragDrop.reset(); LightFolder(nullptr); if (dragBar) dragBar.Visibility(Visibility::Collapsed); return; }
      dragDrop = app::ChatDrop{app::ChatDrop::Target::Room, s.id, upper ? app::ChatDrop::Where::Before : app::ChatDrop::Where::After};
      LightFolder(nullptr); ShowDropBar(upper ? r.Y : r.Y + r.Height);
    }
    return;
  }
}
void ChannelDragEnd() {
  if (!dragArmed) return;
  bool moved = dragMoving; auto drop = dragDrop; auto id = dragId; bool folder = dragFolder; auto places = dragPlaces;
  EndDrag();
  if (!moved) return;
  // The click that ends a drag is not a click on the row it ends over.
  dragSwallow = true;
  auto weak = get_weak();
  window.DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak] { if (auto self = weak.get()) self->dragSwallow = false; });
  if (!drop) return;
  auto order = folder ? app::ChatMoveFolder(places, id, *drop) : app::ChatMoveRoom(places, id, *drop);
  if (!order) return;
  service->ChatWork([this, order = *order](app::Chat& chat) { ReportChatProblem(chat.SaveLayout(order)); });
}
void DragMoved(Input::PointerRoutedEventArgs const& e) {
  bool moving = dragMoving;
  ChannelDragTo(e.GetCurrentPoint(channelList).Position());
  if (!moving && dragMoving) channelList.CapturePointer(e.Pointer());
  if (dragMoving) e.Handled(true);
}
void DragReleased(Input::PointerRoutedEventArgs const& e) {
  bool moved = dragArmed && dragMoving;
  ChannelDragEnd();
  if (moved) { channelList.ReleasePointerCapture(e.Pointer()); e.Handled(true); }
}
void SetupDragLayer(Grid const& host) {
  dragLayer = Canvas(); dragLayer.IsHitTestVisible(false);
  dragBar = Border(); dragBar.Height(2); dragBar.CornerRadius(CornerRadius{1,1,1,1}); dragBar.Background(Brush(Accent)); dragBar.Visibility(Visibility::Collapsed);
  dragLayer.Children().Append(dragBar); host.Children().Append(dragLayer);
  channelList.AddHandler(UIElement::PointerMovedEvent(), box_value(Input::PointerEventHandler([this](auto const&, Input::PointerRoutedEventArgs const& e) { DragMoved(e); })), true);
  channelList.AddHandler(UIElement::PointerReleasedEvent(), box_value(Input::PointerEventHandler([this](auto const&, Input::PointerRoutedEventArgs const& e) { DragReleased(e); })), true);
  channelList.PointerCaptureLost([this](auto const&, auto const&) { if (dragMoving) EndDrag(); });
}
static void Detach(UIElement const& e) {
  auto fe = e.try_as<FrameworkElement>(); if (!fe) return;
  if (auto parent = fe.Parent().try_as<Panel>()) { uint32_t at{}; if (parent.Children().IndexOf(e, at)) parent.Children().RemoveAt(at); }
}

// A folder's heading (App.tsx): chevron, the name in capitals, pressed to
// open or close; the owner's menu on the right button.
Grid FolderHeading(app::ChatCategory const& f, bool shut) {
  Grid row; row.Name(L"FolderHeading"); row.Height(32); row.Margin(Thickness{0, 8, 0, 2}); row.Background(Tint(0,0)); row.CornerRadius(CornerRadius{8,8,8,8});
  StackPanel words; words.Orientation(Orientation::Horizontal); words.Spacing(4); words.Margin(Thickness{4,0,0,0}); words.VerticalAlignment(VerticalAlignment::Center);
  auto chevron = VoiceIcon("chevron-down", Muted, 12).as<FrameworkElement>(); chevron.VerticalAlignment(VerticalAlignment::Center);
  if (shut) { RotateTransform turn; turn.Angle(-90); chevron.RenderTransformOrigin({.5,.5}); chevron.RenderTransform(turn); }
  words.Children().Append(chevron);
  std::wstring upper = net::Wide(f.name);
  if (!upper.empty()) { LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, upper.c_str(), int(upper.size()), upper.data(), int(upper.size()), nullptr, nullptr, 0); }
  auto name = Line(upper, 13, Muted, 600); name.CharacterSpacing(60); name.VerticalAlignment(VerticalAlignment::Center); words.Children().Append(name);
  row.Children().Append(words);
  row.PointerEntered([name, chevron](auto const&, auto const&) { name.Foreground(Brush(Foreground)); });
  row.PointerExited([name](auto const&, auto const&) { name.Foreground(Brush(Muted)); });
  HarmonyPointer(row); row.Tapped([this, id = f.id](auto const&, auto const&) { ToggleFolder(id); });
  Automation::AutomationProperties::SetName(row, L"categoria " + net::Wide(f.name));
  ToolTipService::SetToolTip(row, box_value(canShape ? L"Clique para abrir ou fechar; arraste para mudar de lugar; botão direito para as opções" : L"Clique para abrir ou fechar"));
  if (canShape) MenuAtPointer(row, FolderMenu(f));
  return row;
}

// The pinned messages of the open conversation (Pins.tsx), the most recently
// pinned first; pressing one goes to it, and whoever may pin can unpin here.
void PinMessage(std::string const& channel, int64_t id, bool on) {
  // A refusal (the 250th pin) lands in the conversation's own error line.
  service->ChatWork([channel, id, on](app::Chat& chat) { chat.Pin(channel, id, on); });
}
void OpenPins() {
  CloseSearch();
  std::string channel = chatActive; bool mayPin = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    mayPin = s.Pins(channel);
  });
  if (channel.empty()) return;
  Grid dialog; dialog.Name(L"PinsDialog"); dialog.Background(Tint(0,.7)); dialog.TabFocusNavigation(Input::KeyboardNavigationMode::Cycle);
  Automation::AutomationProperties::SetName(dialog,L"Mensagens fixadas"); Grid::SetColumnSpan(dialog,3); Canvas::SetZIndex(dialog,1000);
  Border card; card.Name(L"BackgroundElement"); card.Background(Brush(Surface)); card.Padding(Thickness{24,24,24,24}); card.CornerRadius(CornerRadius{8,8,8,8});
  card.HorizontalAlignment(HorizontalAlignment::Center); card.VerticalAlignment(VerticalAlignment::Center);
  StackPanel panel; panel.Spacing(16);
  Grid heading; ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); heading.ColumnDefinitions().Append(words);
  ColumnDefinition action; action.Width(GridLength{0,GridUnitType::Auto}); heading.ColumnDefinitions().Append(action);
  auto title=Line(L"Mensagens fixadas",20,Foreground,600); title.VerticalAlignment(VerticalAlignment::Center); heading.Children().Append(title);
  auto close=Square("xmark",L"Fechar",32,16,Foreground,Surface2,[this] { CloseSearch(); }); Grid::SetColumn(close,1); heading.Children().Append(close); panel.Children().Append(heading);
  StackPanel content; content.Width(448); content.Spacing(12);
  // Searching among the pinned (the owner, 02/11/2026): the list is already
  // here, so the words filter it as they are typed, by text and by author.
  TextBox query=HarmonyTextBox(); query.Name(L"PinsQuery"); query.Background(Brush(Surface2)); query.BorderBrush(Brush(0x3f3f46)); query.BorderThickness(Thickness{2}); query.PlaceholderText(L"Procurar nas fixadas"); query.FontSize(16);
  Automation::AutomationProperties::SetName(query,L"procurar nas mensagens fixadas"); content.Children().Append(query);
  auto status = Text(L"Carregando…",13,Muted); content.Children().Append(status);
  ScrollViewer scroll; scroll.MaxHeight(420); StackPanel hits; hits.Spacing(4); scroll.Content(hits); ImmediateMouseWheel(scroll); content.Children().Append(scroll);
  // Borrows the search panel's slot, so one closes the other and Escape,
  // the outside click and CloseSearch all work the same.
  searchBox = query; searchResults = hits; searchDialog = dialog; searchOpen = true; auto generation = ++searchGeneration;
  searchQueryChanged = query.TextChanged([this, status, hits](auto const& eventSender, auto const&) { auto query = eventSender.template as<TextBox>(); FilterPins(to_string(query.Text()), status, hits); });
  panel.Children().Append(content); card.Child(panel); dialog.Children().Append(card);
  dialog.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([this,card](auto const&,Input::PointerRoutedEventArgs const& e) {
    auto p=e.GetCurrentPoint(card).Position();
    if(p.X<0 || p.Y<0 || p.X>card.ActualWidth() || p.Y>card.ActualHeight()) { CloseSearch(); e.Handled(true); }
  })),true);
  dialog.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& e) { if(e.Key()==winrt::Windows::System::VirtualKey::Escape) { CloseSearch(); e.Handled(true); } });
  root.Children().Append(dialog); query.Focus(FocusState::Programmatic);
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  service->ChatWork([channel, generation, mayPin, dispatcher, weak, status, hits, query](app::Chat& chat) {
    std::string error; auto list = chat.Pins(channel, error);
    dispatcher.TryEnqueue([channel, generation, mayPin, weak, status, hits, error, list, query] {
      auto self = weak.get(); if (!self || self->exiting || !self->searchOpen || generation != self->searchGeneration) return;
      if (!error.empty()) { status.Text(net::Wide(error)); return; }
      status.Text(list.empty() ? L"Nenhuma mensagem fixada ainda." : L"");
      status.Visibility(list.empty() ? Visibility::Visible : Visibility::Collapsed);
      for (auto const& m : list) {
        bool blocked = self->PersonalState().Blocked(m.author.username) && !self->personalRevealed.contains(m.id);
        StackPanel label; label.Spacing(2);
        label.Children().Append(self->Line(self->PersonName(m.author) + L"   " + app::ChatShortDate(m.createdAt),13,Muted,500));
        auto body = Text(blocked ? L"Mensagem de pessoa bloqueada — Mostrar" : m.body.empty() ? L"(anexo)" : net::Wide(m.body),16,Foreground); body.MaxLines(4); body.TextTrimming(TextTrimming::CharacterEllipsis);
        label.Children().Append(body);
        Grid line; line.ColumnSpacing(8);
        for (auto w : {GridLength{28,GridUnitType::Pixel}, GridLength{1,GridUnitType::Star}, GridLength{0,GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); line.ColumnDefinitions().Append(c); }
        line.Children().Append(self->PhotoFace(m.author,28)); Grid::SetColumn(label,1); line.Children().Append(label);
        Button button; HarmonyPointer(button); button.MinHeight(56); button.HorizontalAlignment(HorizontalAlignment::Stretch); button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        button.Padding(Thickness{12,12,12,12}); button.Background(Brush(Surface2)); button.CornerRadius(CornerRadius{8,8,8,8});
        button.Content(line); Automation::AutomationProperties::SetName(button, L"ir para a mensagem fixada");
        button.Tag(box_value(PinFold(net::Wide(m.body) + L" " + self->PersonName(m.author) + L" " + net::Wide(m.author.username))));
        button.Click([weak, channel, id = m.id, body, text = m.body, who = m.author.username](auto const&, auto const&) {
          if (auto app = weak.get(); app && !app->exiting) {
            if (app->PersonalState().Blocked(who) && !app->personalRevealed.contains(id)) { app->personalRevealed.insert(id); body.Text(text.empty() ? L"(anexo)" : net::Wide(text)); return; }
            app->CloseSearch(); app->JumpToMessage(channel, id);
          }
        });
        if (mayPin) {
          // Unpinned, it leaves the list for good: no Tag, no filter brings it back.
          auto unpin = self->Square("xmark", L"desafixar", 28, 14, Muted, 0x3f3f46, [weak, channel, id = m.id, button] {
            auto app = weak.get(); if (!app) return;
            app->PinMessage(channel, id, false); button.Tag(nullptr); button.Visibility(Visibility::Collapsed);
          });
          unpin.Name(L"Unpin"); unpin.VerticalAlignment(VerticalAlignment::Top); Grid::SetColumn(unpin,2); line.Children().Append(unpin);
        }
        hits.Children().Append(button);
      }
      if (!list.empty()) self->FilterPins(to_string(query.Text()), status, hits);
    });
  });
}
// Lower case without accents: "acao" finds "Ação".
static std::wstring PinFold(std::wstring const& text) {
  std::wstring folded;
  if (int n = NormalizeString(NormalizationD, text.c_str(), int(text.size()), nullptr, 0); n > 0) {
    std::wstring d(size_t(n), wchar_t(0)); n = NormalizeString(NormalizationD, text.c_str(), int(text.size()), d.data(), n);
    d.resize(n > 0 ? size_t(n) : 0); if (d.empty()) d = text;
    for (wchar_t c : d) if (c < 0x300 || c > 0x36f) folded += wchar_t(std::towlower(c));
  } else for (wchar_t c : text) folded += wchar_t(std::towlower(c));
  return folded;
}
void FilterPins(std::string const& typed, TextBlock const& status, StackPanel const& hits) {
  auto words = PinFold(net::Wide(app::ChatTrim(typed))); int shown = 0, total = 0;
  for (auto const& child : hits.Children()) {
    auto button = child.try_as<Button>(); if (!button || !button.Tag()) continue;
    ++total; bool match = words.empty() || std::wstring(unbox_value<hstring>(button.Tag())).find(words) != std::wstring::npos;
    button.Visibility(match ? Visibility::Visible : Visibility::Collapsed); shown += match;
  }
  if (!total) return;
  status.Text(shown ? L"" : L"Nenhuma fixada com essas palavras.");
  status.Visibility(shown ? Visibility::Collapsed : Visibility::Visible);
}

void DrawChannels(app::State const& state) {
  std::string me; std::vector<app::ChatConversation> list, rooms; std::vector<app::ChatPerson> members; std::vector<app::ChatCategory> categories;
  bool manage = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; list = s.conversations; members = s.members; rooms = s.rooms; categories = s.categories; manage = s.canModerate; });
  app::ChatOrderConversations(list);
  if (!closedFoldersLoaded) {
    closedFoldersLoaded = true;
    try { std::ifstream in(ClosedFoldersFile()); std::string line; while (std::getline(in, line)) if (!line.empty()) closedFolders.insert(line); } catch (...) {}
  }
  std::string signature = chatTab + "|" + chatActive + (showingCall ? "|call" : "|chat") + (manage ? "|own" : "") + "|v" + std::to_string(voiceGroupsVersion) + "|" + state.voice.room;
  for (auto const& c : list) signature += "|" + c.id + c.name + c.peer.username + c.peer.avatar + net::Narrow(PersonName(c.peer)) + std::to_string(c.unread) + "/" + std::to_string(c.mentions) + (c.muted ? "m" : "") + (c.hidden ? "h" : "") + "@" + c.category + "#" + std::to_string(c.position);
  for (auto const& c : rooms) signature += "|r" + c.id + c.name + "@" + c.category + "#" + std::to_string(c.position);
  for (auto const& f : categories) signature += "|f" + f.id + f.name + "#" + std::to_string(f.position);
  for (auto const& f : closedFolders) signature += "|x" + f;
  for (auto const& p : members) signature += "|" + p.username + "/" + p.avatar;
  for (auto const& [who, st] : state.model.presence) signature += "|" + who + st;
  for (auto const& room : state.model.rooms) for (auto const& p : room.people) signature += "|call:" + p.username;
  if (signature == channelsSignature) return;
  SpentMs spent{channelsMs};
  channelsSignature = signature;
  tabServer.Background(chatTab == "server" ? Brush(Default) : Tint(0, 0));
  tabDirect.Background(chatTab == "dm" ? Brush(Default) : Tint(0, 0));
  for (auto [tab, on] : {std::pair{tabServer, chatTab == "server"}, std::pair{tabDirect, chatTab == "dm"}}) {
    auto t = tab.Child().as<TextBlock>(); t.Foreground(Brush(on ? Foreground : Muted)); t.FontWeight(Weight(on ? 600 : 500));
  }
  channelList.Children().Clear();
  bool server = chatTab == "server";
  roomList.Visibility(Visibility::Collapsed);
  for (auto const& [weak, handler] : dragHandlers)
    if (auto handle = weak.get()) handle.RemoveHandler(UIElement::PointerPressedEvent(), handler);
  dragHandlers.clear();
  canShape = manage; dragSpots.clear(); EndDrag();
  int shown = 0;
  auto textRow = [&](app::ChatConversation const& c) {
    bool dm = c.kind == "dm";
    auto name = dm ? PersonName(c.peer) : net::Wide(c.name);
    Grid face{nullptr};
    if (dm) {
      auto person = c.peer;
      for (auto const& member : members) if (member.username == person.username) { person.avatar = member.avatar; break; }
      auto status = state.model.StateOf(person.username);
      bool here = status != "offline", away = status == "away";
      face = Grid(); face.Name(L"DirectAvatar"); face.Width(32); face.Height(32);
      face.Children().Append(PhotoFace(person, 32, here, true));
      Shapes::Ellipse dot; dot.Name(L"DirectPresence"); dot.Width(10); dot.Height(10);
      dot.Fill(here ? Brush(away ? 0xf5a524 : Success) : Tint(Muted, .4));
      dot.HorizontalAlignment(HorizontalAlignment::Left); dot.VerticalAlignment(VerticalAlignment::Top);
      TranslateTransform position; position.X(22.328); position.Y(22.328); dot.RenderTransform(position);
      face.Children().Append(dot);
    }
    auto entry = ChannelRow(name, "hashtag", c.id == chatActive && !showingCall, c.unread, c.mentions, c.muted,
                            [this, id = c.id] { if (!dragSwallow) OpenConversation(id); }, face).as<FrameworkElement>();
    MenuAtPointer(entry, ConversationMenu(c, manage)); channelList.Children().Append(entry);
    if (!dm) Draggable(entry, entry, false, c.id);
  };
  if (!server) {
    for (auto const& c : list) if (c.kind == "dm" && !c.hidden) { ++shown; textRow(c); }
    if (shown == 0) { auto none = Text(L"Nenhuma conversa ainda.", 13, Muted); none.Margin(Thickness{8, 4, 8, 0}); channelList.Children().Append(none); }
  } else {
    // The column (reorder.ts column): the top of it, CANAIS then VOZ as
    // before, then one place per folder; inside each, text above voice.
    std::vector<app::ChatConversation> texts;
    for (auto const& c : list) if (c.kind == "text" && !c.hidden) texts.push_back(c);
    auto places = app::ChatColumn(texts, rooms, categories); dragPlaces = places;
    std::set<std::string> peopled;
    for (auto const& r : state.model.rooms) if (!r.people.empty()) peopled.insert(r.id);
    auto voiceRow = [&](app::ChatConversation const& c) {
      auto group = voiceGroups.find(c.id); if (group == voiceGroups.end()) return;
      Detach(group->second); channelList.Children().Append(group->second);
      auto handle = voiceHandles.find(c.id);
      Draggable(group->second, handle != voiceHandles.end() ? handle->second : group->second, false, c.id);
    };
    for (size_t i = 0; i < places.size(); ++i) {
      auto const& place = places[i];
      if (i == 0) {
        auto canais = HeadingWithPlus(L"CANAIS", L"Novo canal", manage ? std::function<void()>([this] { NewRoom(false); }) : nullptr);
        if (manage) MenuAtPointer(canais, ColumnMenu());
        channelList.Children().Append(canais); DropSpot(canais, "");
        for (auto const& c : place.texts) textRow(c);
        auto voz = HeadingWithPlus(L"VOZ", L"Novo canal de voz", manage ? std::function<void()>([this] { NewRoom(true); }) : nullptr);
        voz.Margin(Thickness{0, 8, 0, 2});
        if (manage) MenuAtPointer(voz, ColumnMenu());
        channelList.Children().Append(voz); DropSpot(voz, "");
        for (auto const& c : place.voices) voiceRow(c);
        if (rooms.empty() && voiceGroups.empty()) { auto none = Text(L"Nenhuma sala de voz.", 13, Muted); none.Margin(Thickness{8, 4, 8, 0}); channelList.Children().Append(none); }
        continue;
      }
      auto const& f = *place.folder;
      bool shut = closedFolders.contains(f.id);
      auto heading = FolderHeading(f, shut); channelList.Children().Append(heading);
      Draggable(heading, heading, true, f.id);
      // Closed, a folder still shows the room you are in and any voice room
      // with somebody inside, as on Discord (App.tsx).
      for (auto const& c : place.texts) if (!shut || c.id == chatActive) textRow(c);
      for (auto const& c : place.voices) if (!shut || c.id == state.voice.room || peopled.contains(c.id)) voiceRow(c);
    }
  }
  // The open channel was deleted (here or by the owner elsewhere): another
  // text channel takes its place, as App.tsx deleteChannel does.
  if (!chatActive.empty() && !list.empty() && std::none_of(list.begin(), list.end(), [&](auto const& c) { return c.id == chatActive; })) {
    chatActive.clear(); messageItems.Clear();
  }
  if (chatActive.empty()) for (auto const& c : list) if (c.kind != "dm" && !c.hidden) { OpenConversation(c.id); break; }
}

// ---------------------------------------------------------------------------
// The conversation: the header island, the list, the line saying who types,
// the reply bar and the box you write in.
// ---------------------------------------------------------------------------
void SyncComposerHint() {
  if (!composer || !composerHint) return;
  composerHint.Text(composer.PlaceholderText());
  composerHint.Select(0, 0);
  composerHint.Visibility(composer.Text().empty() ? Visibility::Visible : Visibility::Collapsed);
}
Grid ConversationColumn() {
  conversation = Grid(); conversation.Margin(Thickness{4, 0, 8, 0});
  for (auto h : {GridLength{64, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) {
    RowDefinition r; r.Height(h); conversation.RowDefinitions().Append(r);
  }
  // Header: an island 8 from the top, 48 tall, padding 0 8 0 16; the mark
  // in a 28 box, the name at 20/28 600, two 32 squares on the right.
  auto header = Island(); header.Name(L"HeaderIsland"); header.Height(48); header.Margin(Thickness{0, 8, 0, 8}); header.Padding(Thickness{16, 0, 8, 0});
  Grid h; h.ColumnSpacing(5);
  for (auto w : {GridLength{28, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) {
    ColumnDefinition c; c.Width(w); h.ColumnDefinitions().Append(c);
  }
  headerMark = Border(); headerMark.Width(28); headerMark.Height(28); headerMark.VerticalAlignment(VerticalAlignment::Center); h.Children().Append(headerMark);
  headerTitle = Line(L"", 20, Foreground, 600); headerTitle.CharacterSpacing(-15); headerTitle.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(headerTitle, 1); h.Children().Append(headerTitle);
  // Calling somebody is a thing you do to a person, so the button is in the
  // one place that is about a person: their conversation (App.tsx).
  headerCall = Square("handset", L"Ligar", 32, 18, Muted, Surface2, [this] {
    if (!headerPeer.username.empty()) { directSmall = false; service->CallRing(app::CallPerson{headerPeer.username, net::Narrow(PersonName(headerPeer))}); }
  });
  headerCall.Name(L"DirectRing"); headerCall.VerticalAlignment(VerticalAlignment::Center); headerCall.Visibility(Visibility::Collapsed);
  StackPanel actions; actions.Orientation(Orientation::Horizontal); actions.Spacing(4); actions.VerticalAlignment(VerticalAlignment::Center);
  auto pins = Square("pin", L"Mensagens fixadas", 32, 18, Muted, Surface2, [this] { OpenPins(); }); pins.Name(L"HeaderPins");
  actions.Children().Append(pins);
  actions.Children().Append(Square("magnifier", L"Procurar uma mensagem", 32, 18, Muted, Surface2, [this] { OpenSearch(); }));
  actions.Children().Append(headerCall);
  auto people = Square("persons", L"pessoas", 32, 18, peopleShown ? Foreground : Muted, Surface2, [this] {
    peopleShown = !peopleShown; callSignature.clear();
    auto button = FindDesignPart(root,L"ChatPeople").as<Button>();
    button.Content(VoiceIcon("persons",peopleShown ? Foreground : Muted,18));
    button.Background(peopleShown ? Tint(Default,.5) : Tint(0,0));
    ToolTipService::SetToolTip(button,box_value(peopleShown ? L"Esconder quem está aqui" : L"Ver quem está aqui"));
    if (peopleIsland) peopleIsland.Visibility(peopleShown ? Visibility::Visible : Visibility::Collapsed);
    if (root.ColumnDefinitions().Size() >= 3) root.ColumnDefinitions().GetAt(2).Width(GridLength{peopleShown ? 248.0 : 0.0,GridUnitType::Pixel});
    Render();
  });
  people.Background(peopleShown ? Tint(Default,.5) : Tint(0,0));
  ToolTipService::SetToolTip(people,box_value(peopleShown ? L"Esconder quem está aqui" : L"Ver quem está aqui"));
  people.Name(L"ChatPeople"); actions.Children().Append(people);
  Grid::SetColumn(actions, 2); h.Children().Append(actions);
  header.Child(h); conversation.Children().Append(header);

  // The list. Recycled rows (the W1 proof: 5,000 messages at 240 Hz), each
  // filled from the model when it scrolls into view.
  Grid middle; Grid::SetRow(middle, 1); middle.Margin(Thickness{0,0,0,8});
  messageItems = single_threaded_observable_vector<IInspectable>();
  messageList = ListView(); messageList.Name(L"Messages"); messageList.SelectionMode(ListViewSelectionMode::None);
  // Let a short history take its desired height at the bottom; longer lists
  // still receive the finite viewport needed by ItemsStackPanel recycling.
  messageList.VerticalAlignment(VerticalAlignment::Bottom);
  messageList.IsItemClickEnabled(false); messageList.Padding(Thickness{0, 0, 0, 0});
  Automation::AutomationProperties::SetName(messageList, L"mensagens");
  messageList.ItemTemplate(Markup::XamlReader::Load(L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><Grid/></DataTemplate>").as<DataTemplate>());
  messageList.ItemContainerStyle(Markup::XamlReader::Load(
      L"<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='ListViewItem'>"
      L"<Setter Property='Padding' Value='0'/><Setter Property='Margin' Value='0'/><Setter Property='MinHeight' Value='0'/>"
      L"<Setter Property='HorizontalContentAlignment' Value='Stretch'/><Setter Property='IsTabStop' Value='False'/>"
      L"<Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ListViewItem'><ContentPresenter Content='{TemplateBinding Content}' ContentTemplate='{TemplateBinding ContentTemplate}' HorizontalContentAlignment='Stretch'/></ControlTemplate></Setter.Value></Setter></Style>").as<Style>());
  for (auto key : {L"ListViewItemBackgroundPointerOver", L"ListViewItemBackgroundPressed", L"ListViewItemBackground"})
    messageList.Resources().Insert(box_value(key), Tint(0, 0));
  messageList.ContainerContentChanging([this](ListViewBase const& sender, ContainerContentChangingEventArgs const& args) { FillContainer(sender, args); });
  messageList.ItemsSource(messageItems);
  historyStart = Text(L"Come\u00e7o da conversa.",13,Muted); historyStart.HorizontalAlignment(HorizontalAlignment::Center);
  historyStart.Margin(Thickness{0,16,0,16}); historyStart.Visibility(Visibility::Collapsed); messageList.Header(historyStart);
  messageList.Loaded([this](auto const&, auto const&) { HookScroll(); });
  middle.Children().Append(messageList);
  emptyLine = Text(L"Ninguém falou nada aqui ainda.", 16, Muted); emptyLine.HorizontalAlignment(HorizontalAlignment::Center);
  // Messages.tsx's empty paragraph sits at the end of the padded history.
  // Match the measured ink, including the scrollbar's horizontal reservation.
  emptyLine.VerticalAlignment(VerticalAlignment::Bottom); emptyLine.Margin(Thickness{0, 0, 8, 13}); emptyLine.Visibility(Visibility::Collapsed);
  middle.Children().Append(emptyLine);
  conversation.Children().Append(middle);

  StackPanel bottom; bottom.Name(L"ComposerArea"); Grid::SetRow(bottom, 2); bottom.Padding(Thickness{0, 0, 0, 8});
  personalComposer = StackPanel(); personalComposer.Name(L"PersonalConversation"); personalComposer.Spacing(8);
  personalComposer.Margin(Thickness{12, 8, 12, 8}); personalComposer.Visibility(Visibility::Collapsed); bottom.Children().Append(personalComposer);
  typingLine = Line(L"", 13, Muted, 400); typingLine.Margin(Thickness{20, 0, 20, 4}); typingLine.Visibility(Visibility::Collapsed); bottom.Children().Append(typingLine);
  // "Respondendo a Fulano  texto…  ×": 40 tall on surface, pl-3 pr-1, 8 above the box.
  replyBar = Island(); replyBar.Height(40); replyBar.Margin(Thickness{0, 0, 0, 8}); replyBar.Padding(Thickness{12, 0, 4, 0}); replyBar.Visibility(Visibility::Collapsed);
  Grid rb; rb.ColumnSpacing(8);
  for (auto w : {GridLength{0, GridUnitType::Auto}, GridLength{0, GridUnitType::Auto}, GridLength{1, GridUnitType::Star}, GridLength{32, GridUnitType::Pixel}}) {
    ColumnDefinition c; c.Width(w); rb.ColumnDefinitions().Append(c);
  }
  auto lead = Line(L"Respondendo a", 13, Muted, 400); lead.VerticalAlignment(VerticalAlignment::Center); rb.Children().Append(lead);
  replyName = Line(L"", 13, Foreground, 600); replyName.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(replyName, 1); rb.Children().Append(replyName);
  replyBody = Line(L"", 13, Muted, 400); replyBody.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(replyBody, 2); rb.Children().Append(replyBody);
  auto close = Square("xmark", L"cancelar resposta", 32, 16, DangerSoftFg, 0xe15451, [this] { SetReply(0); });
  NegativeButton(close);
  close.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(close, 3); rb.Children().Append(close);
  replyBar.Child(rb); bottom.Children().Append(replyBar);
  uploadRow = StackPanel(); uploadRow.Orientation(Orientation::Horizontal); uploadRow.Margin(Thickness{0, 0, 0, 8}); uploadRow.Visibility(Visibility::Collapsed);
  bottom.Children().Append(uploadRow); DrawUploads();
  composerError = Text(L"", 13, DangerSoftFg); composerError.Margin(Thickness{4, 0, 4, 8}); composerError.Visibility(Visibility::Collapsed); bottom.Children().Append(composerError);

  // The box: an island with p-1.5, the tools 36 square, the text at 16/24
  // with py-1.5 px-1, and "Enviar" 36 tall on the accent.
  auto island = Island(); island.Name(L"ComposerIsland"); island.Padding(Thickness{4,4,4,4}); island.BorderThickness(Thickness{2,2,2,2}); island.BorderBrush(Brush(Surface));
  Grid row; row.ColumnSpacing(4);
  for (auto w : {GridLength{36, GridUnitType::Pixel}, GridLength{36, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) {
    ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c);
  }
  auto attach = Square("file", L"anexar", 36, 16, Muted, Default, [this] { PickAttachment(); }); attach.Name(L"ComposerAttach"); attach.VerticalAlignment(VerticalAlignment::Bottom); row.Children().Append(attach);
  auto emoji = Square("face-smile", L"emoji", 36, 16, Muted, Default, [this] { OpenEmojiPanel(); }); emoji.VerticalAlignment(VerticalAlignment::Bottom); Grid::SetColumn(emoji, 1); row.Children().Append(emoji);
  auto makeInput = [this](uint32_t foreground) {
  TextBox composer=HarmonyTextBox();
  // Not the login fields' style (assets/harmony.xaml: Height 44, the line
  // pinned at 24): the box grows with what you write, up to 160.
  composer.Style(Resources().Lookup(box_value(L"HarmonyEditor")).as<Style>()); composer.AcceptsReturn(true); composer.TextWrapping(TextWrapping::Wrap);
  // The WinUI text engine paints Inter 3px above Chromium at equal padding.
  // Keep the same 12px total inset and move both real text and hint together.
  // TextBox has no public line-spacing setting. Padding moves the block but
  // cannot turn its 20px line advance into the web's 24px. WinUI's supported
  // route is RichEditBox.Document + ParagraphFormat.SetLineSpacing(Exactly,
  // 18pt). Migrating requires adapting plain-text selection, mentions, paste,
  // drafts and Enter handling together; do not fake it with font-size changes.
  composer.MinHeight(36); composer.MaxHeight(160); composer.FontSize(16); composer.Padding(Thickness{4, 9, 4, 3});
  composer.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); composer.BorderThickness(Thickness{0, 0, 0, 0});
  composer.Background(Tint(0, 0)); composer.VerticalAlignment(VerticalAlignment::Stretch);
  composer.Foreground(Brush(foreground));
  for (auto key : {L"TextControlForeground", L"TextControlForegroundPointerOver", L"TextControlForegroundFocused"}) composer.Resources().Insert(box_value(key), Brush(foreground));
  for (auto key : {L"TextControlBackgroundPointerOver", L"TextControlBackgroundFocused", L"TextControlBackground"}) composer.Resources().Insert(box_value(key), Tint(0, 0));
  for (auto key : {L"TextControlBorderBrushFocused", L"TextControlBorderBrushPointerOver", L"TextControlBorderBrush"}) composer.Resources().Insert(box_value(key), Tint(0, 0));
  // The default template reads its floor and its padding from the theme.
  // The web's textarea is py-1.5 px-1 on a 24 line: 36.
  composer.Resources().Insert(box_value(L"TextControlThemeMinHeight"), box_value(24.0));
  composer.Resources().Insert(box_value(L"TextControlThemePadding"), box_value(Thickness{4, 9, 4, 3}));
  composer.Resources().Insert(box_value(L"TextControlPlaceholderForeground"), Brush(Muted));
  composer.Resources().Insert(box_value(L"TextControlPlaceholderForegroundFocused"), Brush(Muted));
  composer.Resources().Insert(box_value(L"TextControlPlaceholderForegroundPointerOver"), Brush(Muted));
  // The native text engine is shared by the editable text and the hint.
  // TextBlock has different baseline/line metrics even with the same font.
  composer.Loaded([](IInspectable const& sender, auto const&) {
    auto box = sender.as<TextBox>();
    if (auto hint = FindDesignPart(box, L"PlaceholderTextContentPresenter").try_as<TextBlock>()) {
      // TextBox may toggle Visibility again when PlaceholderText changes.
      // Keep that legacy part out of both painting and desired-size layout.
      hint.Visibility(Visibility::Collapsed); hint.Opacity(0); hint.Height(0);
    }
    if (auto content = FindDesignPart(box, L"ContentElement").try_as<ScrollViewer>())
      content.VerticalContentAlignment(VerticalAlignment::Center);
  });
  return composer;
  };
  composer = makeInput(Foreground); composer.Name(L"Composer"); composer.FontWeight(Weight(500));
  BindFieldRing(composer,island);
  composerHint = makeInput(Muted); composerHint.Name(L"ComposerHint");
  composerHint.IsReadOnly(true); composerHint.IsTabStop(false); composerHint.IsHitTestVisible(false);
  composerHint.Foreground(Brush(Muted));
  Automation::AutomationProperties::SetAccessibilityView(composerHint, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetName(composer, L"mensagem");
  composer.RegisterPropertyChangedCallback(TextBox::PlaceholderTextProperty(), [this](auto const&, auto const&) { SyncComposerHint(); });
  // Enter sends and Shift+Enter is a new line, as on the web; Esc leaves a
  // reply or an edit. Arrow keys outside mentions keep text navigation.
  composer.PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) {
    e.Handled(ComposerKey(e.Key(), (GetKeyState(VK_SHIFT) & 0x8000) != 0));
  });
  composer.TextChanged([this](auto const&, auto const&) {
    SyncComposerHint();
    // Text changes arrive after Quit() too (the box is cleared as the window
    // goes), when the service already belongs to its cleanup thread.
    if (composerLoading || chatActive.empty() || !service) return;
    UpdateSend(); UpdateMentions();
    auto text = to_string(composer.Text());
    service->ChatWork([id = chatActive, text, reply = replyingTo](app::Chat& c) { c.Draft(id, text, reply); c.Typing(id, text); });
  });
  HookPaste();
  composer.SelectionChanged([this](auto const&, auto const&) { if (!composerLoading && mentionPopup) UpdateMentions(); });
  Grid input; input.Name(L"ComposerInputSurface"); input.Background(Tint(0,0));
  input.Children().Append(composer); input.Children().Append(composerHint);
  Grid::SetColumn(input, 2); row.Children().Append(input);
  sendButton = Button(); sendButton.Height(36); sendButton.MinHeight(36); sendButton.Padding(Thickness{16, 0, 16, 0});
  sendButton.CornerRadius(CornerRadius{8, 8, 8, 8}); sendButton.BorderThickness(Thickness{0, 0, 0, 0}); sendButton.VerticalAlignment(VerticalAlignment::Bottom);
  auto label = Line(L"Enviar", 13, Foreground, 600); sendButton.Content(label);
  sendButton.Background(Brush(Accent));
  sendButton.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(0x3592f9));
  sendButton.Resources().Insert(box_value(L"ButtonBackgroundDisabled"), Brush(Default));
  sendButton.Resources().Insert(box_value(L"ButtonForegroundDisabled"), Brush(Muted));
  sendButton.Click([this](auto const&, auto const&) { SendComposer(); });
  Grid::SetColumn(sendButton, 3); row.Children().Append(sendButton);
  UpdateSend(); // an empty box: Enviar waits, from the first frame
  island.Child(row); bottom.Children().Append(island);
  conversation.Children().Append(bottom);
  return conversation;
}

// Autocomplete must run before send in both the routed event and the audit.
bool ComposerKey(winrt::Windows::System::VirtualKey key, bool shift) {
  using winrt::Windows::System::VirtualKey;
  if (MentionKey(key)) return true;
  if (key == VirtualKey::Enter && !shift) { SendComposer(); return true; }
  if (key == VirtualKey::Escape && (replyingTo || editingId)) { SetReply(0); CancelEdit(); return true; }
  return false;
}
void UpdateSend() {
  // As the web: nothing to send, or a file still climbing, and it waits.
  bool climbing = Climbing();
  bool any = !composer.IsReadOnly() && (!app::ChatTrim(to_string(composer.Text())).empty() || ReadyFiles() > 0) && !climbing && !sendWaiting;
  sendButton.IsEnabled(any);
  ToolTipService::SetToolTip(sendButton, climbing ? box_value(L"Esperando o arquivo subir") : nullptr);
  sendButton.Content().as<TextBlock>().Foreground(Brush(any ? Foreground : Muted));
}

void OpenConversation(std::string const& id) {
  // The conversation that was open under the call: pressing it leaves the call's view.
  if (id == chatActive) { if (showingCall) { showingCall = false; callSignature.clear(); Render(); } return; }
  pendingJump = 0;
  personalRevealed.clear();
  chatActive = id; replyingTo = 0; editingId = 0; stickToBottom = true; chatSeen = 0; pendingSent.clear(); sendWaiting = false; ForgetAllUploads(); CloseMentions();
  openedRead = 0;
  service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.id == id) openedRead = c.lastRead; });
  landing = Landing{}; landing.waiting = true;
  showingCall = false; callSignature.clear();
  channelsSignature.clear();
  { SpentMs spent{clearMs}; messageItems.Clear(); }
  service->ChatWork([id](app::Chat& c) { c.Open(id); });
  RefreshHeader(); LoadDraft();
  Render();
}

int64_t pendingJump = 0;
void JumpToMessage(std::string const& channel, int64_t id) {
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    for (auto const& c : s.conversations) if (c.id == channel) chatTab = c.kind == "dm" ? "dm" : "server";
  });
  channelsSignature.clear();
  if (channel != chatActive) OpenConversation(channel);
  stickToBottom = false; pendingJump = id; landing = Landing{};
  if (IndexOf(id) < 0) service->ChatWork([channel, id](app::Chat& chat) { chat.Open(channel,id); });
  else FinishMessageJump();
}
void FinishMessageJump() {
  if (!pendingJump) return;
  int i = IndexOf(pendingJump);
  if (i < 0) return;
  messageList.ScrollIntoView(messageItems.GetAt(uint32_t(i)), ScrollIntoViewAlignment::Default);
  pendingJump = 0;
}
uint64_t searchGeneration = 0;
bool searchOpen = false;
TextBox searchBox{nullptr}; StackPanel searchResults{nullptr}; Grid searchDialog{nullptr};
winrt::event_token searchQueryChanged{};
winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer searchTimer{nullptr};
bool SearchPressOutside(FrameworkElement const& card,winrt::Windows::Foundation::Point p) {
  if(p.X>=0 && p.Y>=0 && p.X<=card.ActualWidth() && p.Y<=card.ActualHeight()) return false;
  CloseSearch(); return true;
}
void CloseSearch() {
  if(!searchOpen) return;
  searchOpen=false; ++searchGeneration;
  if(searchTimer) { searchTimer.Stop(); searchTimer=nullptr; }
  searchBox.TextChanged(searchQueryChanged); searchBox.Text(L""); searchResults.Children().Clear();
  uint32_t at{}; if(root.Children().IndexOf(searchDialog,at)) root.Children().RemoveAt(at);
  searchDialog.Children().Clear(); searchDialog=nullptr; searchBox=nullptr; searchResults=nullptr;
  if(composer) composer.Focus(FocusState::Programmatic);
}
void OpenSearch() {
  CloseSearch();
  Grid dialog; dialog.Background(Tint(0,.7)); dialog.TabFocusNavigation(Input::KeyboardNavigationMode::Cycle);
  Automation::AutomationProperties::SetName(dialog,L"Procurar"); Grid::SetColumnSpan(dialog,3); Canvas::SetZIndex(dialog,1000);
  Border card; card.Name(L"BackgroundElement"); card.Background(Brush(Surface)); card.Padding(Thickness{24,24,24,24}); card.CornerRadius(CornerRadius{8,8,8,8});
  card.HorizontalAlignment(HorizontalAlignment::Center); card.VerticalAlignment(VerticalAlignment::Center);
  StackPanel panel; panel.Spacing(16);
  Grid heading; ColumnDefinition words; words.Width(GridLength{1,GridUnitType::Star}); heading.ColumnDefinitions().Append(words);
  ColumnDefinition action; action.Width(GridLength{0,GridUnitType::Auto}); heading.ColumnDefinitions().Append(action);
  auto title=Line(L"Procurar",20,Foreground,600); title.Name(L"Title"); title.VerticalAlignment(VerticalAlignment::Center); heading.Children().Append(title);
  auto close=Square("xmark",L"Fechar",32,16,Foreground,Surface2,[this] { CloseSearch(); }); Grid::SetColumn(close,1); heading.Children().Append(close); panel.Children().Append(heading);
  StackPanel content; content.Name(L"Content"); content.Width(448); content.Spacing(12);
  TextBox query=HarmonyTextBox(); query.Background(Brush(Surface2)); query.BorderBrush(Brush(0x3f3f46)); query.BorderThickness(Thickness{2}); query.PlaceholderText(L"Uma palavra que estava na mensagem"); query.FontSize(16);
  Automation::AutomationProperties::SetName(query,L"procurar mensagens"); content.Children().Append(query);
  auto status = Text(L"Digite pelo menos duas letras.",13,Muted); status.HorizontalAlignment(HorizontalAlignment::Left); content.Children().Append(status);
  ScrollViewer scroll; scroll.Name(L"SearchScroll"); scroll.MaxHeight(420); StackPanel hits; hits.Spacing(4); scroll.Content(hits); ImmediateMouseWheel(scroll); content.Children().Append(scroll);
  searchBox = query; searchResults = hits; searchDialog = dialog;
  panel.Children().Append(content); card.Child(panel); dialog.Children().Append(card); searchOpen = true; ++searchGeneration;
  searchQueryChanged = query.TextChanged([this, status, hits](auto const& eventSender, auto const&) { auto query = eventSender.template as<TextBox>();
    auto generation = ++searchGeneration;
    if (searchTimer) searchTimer.Stop();
    hits.Children().Clear(); status.Visibility(Visibility::Visible);
    auto text = app::ChatTrim(to_string(query.Text()));
    if (net::Wide(text).size() < 2) { status.Text(L"Digite pelo menos duas letras."); return; }
    status.Text(L"Procurando\u2026");
    auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
    searchTimer = dispatcher.CreateTimer(); searchTimer.Interval(std::chrono::milliseconds(250)); searchTimer.IsRepeating(false);
    searchTimer.Tick([this, generation, text, dispatcher, weak, status, hits](auto const&, auto const&) {
      service->ChatWork([generation, text, dispatcher, weak, status, hits](app::Chat& chat) {
        auto response = chat.Search(text);
        dispatcher.TryEnqueue([generation, weak, response, status, hits] {
          auto self = weak.get(); if (!self || self->exiting || !self->searchOpen || generation != self->searchGeneration) return;
          if (response.status < 200 || response.status >= 300) { status.Text(net::Wide(app::ChatError(response))); return; }
          auto values = response.Json()["messages"];
          status.Text(values.Elements().empty() ? L"Nada com essa palavra." : L"");
          status.Visibility(values.Elements().empty() ? Visibility::Visible : Visibility::Collapsed);
          for (auto const& hit : values.Elements()) {
            auto m = app::ParseChatMessage(hit); auto ch = hit["channel"];
            auto place = ch["kind"].Str() == "dm" ? net::Wide(ch["peer"]["display_name"].Str()) : L"#" + net::Wide(ch["name"].Str());
            StackPanel label; label.Spacing(2);
            label.Children().Append(self->Line(place + L"   " + self->PersonName(m.author) + L"   " + app::ChatShortDate(m.createdAt),13,Muted,500));
            bool blocked = self->PersonalState().Blocked(m.author.username) && !self->personalRevealed.contains(m.id);
            auto body = Text(blocked ? L"Mensagem de pessoa bloqueada — Mostrar" : net::Wide(m.body),16,Foreground);
            label.Children().Append(body);
            Grid resultRow; resultRow.ColumnSpacing(8);
            ColumnDefinition faceColumn; faceColumn.Width(GridLength{28,GridUnitType::Pixel}); resultRow.ColumnDefinitions().Append(faceColumn);
            ColumnDefinition wordsColumn; wordsColumn.Width(GridLength{1,GridUnitType::Star}); resultRow.ColumnDefinitions().Append(wordsColumn);
            resultRow.Children().Append(self->PhotoFace(m.author,28)); Grid::SetColumn(label,1); resultRow.Children().Append(label);
            Button button; HarmonyPointer(button); button.MinHeight(56); button.Content(resultRow); button.HorizontalAlignment(HorizontalAlignment::Stretch); button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            button.Padding(Thickness{12,12,12,12}); button.Background(Brush(Surface2)); button.CornerRadius(CornerRadius{8,8,8,8});
            button.Click([weak, channel = m.channel, id = m.id, body, text = m.body, who = m.author.username](auto const&, auto const&) {
              if (auto app = weak.get(); app && !app->exiting) {
                if (app->PersonalState().Blocked(who) && !app->personalRevealed.contains(id)) { app->personalRevealed.insert(id); body.Text(net::Wide(text)); return; }
                app->CloseSearch(); app->JumpToMessage(channel,id);
              }
            });
            hits.Children().Append(button);
          }
        });
      });
    });
    searchTimer.Start();
  });
  dialog.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler(
    [this,card](auto const&,Input::PointerRoutedEventArgs const& e) {
      if(SearchPressOutside(card,e.GetCurrentPoint(card).Position())) e.Handled(true);
    })),true);
  dialog.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& e) {
    if(e.Key()==winrt::Windows::System::VirtualKey::Escape) { CloseSearch(); e.Handled(true); }
  });
  query.Loaded([this](auto const& eventSender,auto const&) { auto query = eventSender.template as<TextBox>(); if(searchOpen && searchBox==query) query.Focus(FocusState::Programmatic); });
  root.Children().Append(dialog);
}

void RefreshHeader() {
  app::ChatConversation conv; std::string me;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    me = s.owner;
    for (auto const& c : s.conversations) if (c.id == chatActive) conv = c;
  });
  bool dm = conv.kind == "dm";
  auto name = dm ? PersonName(conv.peer) : net::Wide(conv.name);
  headerTitle.Text(name); CaptionPlace(name.empty() ? L"" : dm ? name : L"#" + name);
  headerPeer = dm ? conv.peer : app::ChatPerson{};
  if (headerCall) {
    headerCall.Visibility(dm && !conv.peer.username.empty() ? Visibility::Visible : Visibility::Collapsed);
    Automation::AutomationProperties::SetName(headerCall, L"Ligar para " + name); ToolTipService::SetToolTip(headerCall, box_value(L"Ligar para " + name));
  }
  Grid::SetColumn(headerTitle,dm ? 0 : 1); Grid::SetColumnSpan(headerTitle,dm ? 2 : 1);
  headerMark.Visibility(dm ? Visibility::Collapsed : Visibility::Visible);
  auto mark = VoiceIcon("hashtag", Muted, 22).as<FrameworkElement>();
  mark.HorizontalAlignment(HorizontalAlignment::Center); mark.VerticalAlignment(VerticalAlignment::Center); headerMark.Child(mark);
  composer.PlaceholderText(dm ? L"Conversar com " + name : L"Escrever em #" + name);
}

void LoadDraft() {
  std::string draft; int64_t reply = 0;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    if (auto t = s.timelines.find(chatActive); t != s.timelines.end()) { draft = t->second.draft; reply = t->second.replyTo; }
  });
  composerLoading = true; composer.Text(net::Wide(draft)); composer.SelectionStart(int32_t(composer.Text().size())); composerLoading = false;
  SetReply(reply); UpdateSend();
}

void SetReply(int64_t id) {
  if (!service) return;
  replyingTo = id;
  if (!id) { replyBar.Visibility(Visibility::Collapsed); if (!chatActive.empty() && !composerLoading) service->ChatWork([c = chatActive, t = to_string(composer.Text())](app::Chat& chat) { chat.Draft(c, t, 0); }); return; }
  std::optional<app::ChatMessage> m;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    if (auto t = s.timelines.find(chatActive); t != s.timelines.end()) for (auto const& x : t->second.messages) if (x.id == id) m = x;
  });
  if (!m) { replyingTo = 0; replyBar.Visibility(Visibility::Collapsed); return; }
  replyName.Text(PersonName(m->author)); replyBody.Text(net::Wide(m->body.empty() ? std::string("Imagem") : m->body));
  replyBar.Visibility(Visibility::Visible);
  if (!composerLoading) service->ChatWork([c = chatActive, t = to_string(composer.Text()), id](app::Chat& chat) { chat.Draft(c, t, id); });
  composer.Focus(FocusState::Programmatic);
}

void SendComposer() {
  if (!service) return;
  auto text = app::ChatTrim(to_string(composer.Text()));
  // The web keeps the text in the box while the message is on its way and
  // clears it only if nothing was written meanwhile (Composer.tsx send), so
  // a failure never eats what you wrote.
  if (chatActive.empty() || Climbing() || sendWaiting) return;
  std::vector<std::string> files; std::set<int> keys;
  for (auto const& f : pendingFiles) if (f.stored) { files.push_back(f.stored->id); keys.insert(f.key); }
  if (text.empty() && files.empty()) return;
  sendingFiles = std::move(keys);
  stickToBottom = true; pendingSent = to_string(composer.Text()); sendWaiting = true; uploadError.clear();
  UpdateSend();
  service->ChatSend(chatActive, text, files, replyingTo);
}
void SettleSend(bool sending, std::string const& failure) {
  if (!sendWaiting || sending) return;
  if (failure.empty()) {
    // Only what went: whatever was written or attached meanwhile is the
    // next message, not this one.
    if (to_string(composer.Text()) == pendingSent) { composerLoading = true; composer.Text(L""); composerLoading = false; SetReply(0); }
    std::erase_if(pendingFiles, [&](auto const& f) { return sendingFiles.count(f.key) > 0; });
    DrawUploads(); UpdateSend();
  }
  sendingFiles.clear(); pendingSent.clear(); sendWaiting = false; UpdateSend();
}

// The edit closes when the message on screen says the new text, and stays
// open with what was written when the server refuses (Messages.tsx commit).
bool editSent = false; std::string editSentText; int64_t lastSeenNewest = 0;
void SettleEdit(std::string const& failure) {
  if (!editSent || !editingId) return;
  if (!failure.empty()) { editSent = false; if (inlineEditError) { inlineEditError.Text(L"N\u00e3o salvou: " + net::Wide(failure)); inlineEditError.Visibility(Visibility::Visible); } return; }
  bool taken = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    if (auto t = s.timelines.find(chatActive); t != s.timelines.end())
      for (auto const& m : t->second.messages) if (m.id == editingId) taken = m.body == editSentText;
  });
  if (taken) { editSent = false; CancelEdit(); }
}

// Editing belongs to the message; the composer remains an independent draft.
TextBox inlineEditor{nullptr}; TextBlock inlineEditError{nullptr};
std::wstring inlineDraft;
bool focusInlineEdit = false;
void StartEdit(app::ChatMessage const& m) {
  auto previous = editingId;
  editingId = m.id; editSent = false; inlineDraft = net::Wide(m.body); focusInlineEdit = true;
  RefreshAt(IndexOf(previous)); RefreshAt(IndexOf(editingId));
}
void CancelEdit() {
  auto previous = editingId;
  editingId = 0; editSent = false; inlineEditor = nullptr; inlineEditError = nullptr; inlineDraft.clear();
  RefreshAt(IndexOf(previous));
}
void SaveInlineEdit() {
  if (!service || !editingId || editSent) return;
  auto text = app::ChatTrim(net::Narrow(inlineDraft));
  if (text.empty()) { CancelEdit(); return; }
  editSent = true; editSentText = text;
  if (inlineEditError) { inlineEditError.Text(L""); inlineEditError.Visibility(Visibility::Collapsed); }
  service->ChatWork([c = chatActive, id = editingId, text](app::Chat& chat) { chat.Edit(c, id, text); });
}
UIElement InlineEditBox() {
  StackPanel panel; panel.Spacing(8); panel.Padding(Thickness{0,4,0,4});
  TextBox box=HarmonyTextBox(); box.Name(L"InlineMessageEditor"); box.AcceptsReturn(true); box.TextWrapping(TextWrapping::Wrap);
  box.FontSize(16); box.MinHeight(64); box.Padding(Thickness{12,8,12,8}); box.Background(Brush(Surface2));
  box.Foreground(Brush(Foreground)); box.BorderThickness(Thickness{0}); box.CornerRadius(CornerRadius{8,8,8,8});
  box.Text(inlineDraft); inlineEditor = box;
  box.TextChanged([this](auto const& eventSender, auto const&) { auto box = eventSender.template as<TextBox>(); inlineDraft = box.Text(); });
  box.PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    if (e.Key() == VirtualKey::Escape) { e.Handled(true); CancelEdit(); }
    else if (e.Key() == VirtualKey::Enter && !(GetKeyState(VK_SHIFT) & 0x8000)) { e.Handled(true); SaveInlineEdit(); }
  });
  box.Loaded([this](auto const& eventSender, auto const&) { auto box = eventSender.template as<TextBox>();
    if (focusInlineEdit) { focusInlineEdit = false; box.Focus(FocusState::Programmatic); box.SelectionStart(int32_t(box.Text().size())); }
  });
  panel.Children().Append(box);
  inlineEditError = Text(L"",13,DangerSoftFg); inlineEditError.Visibility(Visibility::Collapsed); panel.Children().Append(inlineEditError);
  StackPanel buttons; buttons.Orientation(Orientation::Horizontal); buttons.Spacing(8);
  for (bool save : {true, false}) {
    Button button; HarmonyPointer(button); button.Content(box_value(save ? L"Salvar" : L"Cancelar")); button.Height(32); button.MinHeight(0);
    button.Padding(Thickness{12,0,12,0}); button.FontSize(13); button.Background(Brush(save ? Accent : Default));
    button.Foreground(Brush(Foreground)); button.CornerRadius(CornerRadius{8,8,8,8});
    button.Style(Resources().Lookup(box_value(save ? L"HarmonyPrimaryButton" : L"HarmonyDangerButton")).as<Style>());
    if(!save) {NegativeButton(button);}
    button.Click([this, save](auto const&, auto const&) { if (save) SaveInlineEdit(); else CancelEdit(); });
    buttons.Children().Append(button);
  }
  auto hint = Text(L"Enter salva, Esc cancela.",13,Muted); hint.VerticalAlignment(VerticalAlignment::Center); buttons.Children().Append(hint);
  panel.Children().Append(buttons); return panel;
}
void EditLastOwn() {
  std::optional<app::ChatMessage> last;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    if (auto t = s.timelines.find(chatActive); t != s.timelines.end())
      for (auto it = t->second.messages.rbegin(); it != t->second.messages.rend(); ++it)
        if (it->author.username == s.owner && !it->deleted) { last = *it; break; }
  });
  if (last) StartEdit(*last);
}

// A click on the dimmed backdrop closes a dialog, as it does the panels.
void DismissOnOutsideClick(ContentDialog const& dialog) {
  dialog.Loaded([](auto const& sender,auto const&) {
    auto shown=sender.template as<ContentDialog>();
    auto layout=FindDesignPart(shown,L"LayoutRoot").template try_as<Grid>(); if(!layout) return;
    layout.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([weak=make_weak(shown)](auto const& sender,Input::PointerRoutedEventArgs const& e) {
      auto dialog=weak.get(); if(!dialog) return;
      auto layer=sender.template as<FrameworkElement>(); auto surface=FindDesignPart(layer,L"BackgroundElement"); if(!surface) return;
      auto point=e.GetCurrentPoint(layer).Position(); auto bounds=surface.TransformToVisual(layer).TransformBounds({0,0,float(surface.ActualWidth()),float(surface.ActualHeight())});
      if(point.X<bounds.X || point.Y<bounds.Y || point.X>=bounds.X+bounds.Width || point.Y>=bounds.Y+bounds.Height) dialog.Hide();
    })),true);
  });
}
// The confirmation look of "Sair desta conta?": the island's ground, 400
// wide, 20 in, and two buttons of 44 side by side at the right.
void ConfirmLook(ContentDialog const& dialog) {
  dialog.Background(Brush(Surface)); dialog.BorderThickness(Thickness{0}); dialog.CornerRadius(CornerRadius{12,12,12,12});
  dialog.Resources().Insert(box_value(L"ContentDialogMaxWidth"),box_value(400.0));
  dialog.Resources().Insert(box_value(L"ContentDialogMinWidth"),box_value(360.0));
  dialog.Resources().Insert(box_value(L"ContentDialogBorderWidth"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogBackground"),Brush(Surface));
  dialog.Resources().Insert(box_value(L"ContentDialogSmokeFill"),Tint(0,.7));
  dialog.Resources().Insert(box_value(L"ContentDialogSeparatorThickness"),box_value(Thickness{0}));
  dialog.Resources().Insert(box_value(L"ContentDialogTopOverlay"),Tint(0,0));
  dialog.Resources().Insert(box_value(L"ContentDialogPadding"),box_value(Thickness{20,20,20,20}));
  dialog.PrimaryButtonStyle(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
  dialog.CloseButtonStyle(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
  dialog.Loaded([this](auto const& sender,auto const&) {
    auto shown=sender.template as<ContentDialog>();
    auto commands=FindDesignPart(shown,L"CommandSpace").template try_as<Grid>();
    auto primary=FindDesignPart(shown,L"PrimaryButton").template try_as<Button>();
    auto cancel=FindDesignPart(shown,L"CloseButton").template try_as<Button>();
    if(!commands || !primary || !cancel) return;
    for(auto b:{primary,cancel}) {
      b.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); b.FontSize(14); b.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
      b.Height(44); b.MinHeight(44); b.MinWidth(96); b.Padding(Thickness{16,0,16,0}); b.HorizontalAlignment(HorizontalAlignment::Stretch);
      b.CornerRadius(CornerRadius{8,8,8,8}); b.BorderThickness(Thickness{0}); b.Foreground(Brush(Foreground));
      NegativeButton(b);
      // The default (Cancel) must not wear a lighter "accent" red at rest.
      for(auto key:{L"AccentButtonBackground",L"AccentButtonBackgroundPointerOver",L"AccentButtonBackgroundPressed"}) b.Resources().Insert(box_value(key),Brush(std::wstring(key)==L"AccentButtonBackground"?0xdb3b3e:0xe15451));
      for(auto key:{L"AccentButtonForeground",L"AccentButtonForegroundPointerOver",L"AccentButtonForegroundPressed"}) b.Resources().Insert(box_value(key),Brush(Foreground));
    }
    Grid::SetColumn(cancel,2); Grid::SetColumn(primary,4); Grid::SetColumnSpan(cancel,1); Grid::SetColumnSpan(primary,1);
    for(uint32_t i=0;i<commands.ColumnDefinitions().Size();++i)
      commands.ColumnDefinitions().GetAt(i).Width(i==0?GridLength{1,GridUnitType::Star}:i==1?GridLength{0,GridUnitType::Pixel}:i==3?GridLength{8,GridUnitType::Pixel}:GridLength{0,GridUnitType::Auto});
  });
  DismissOnOutsideClick(dialog);
}
void RemoveMessage(app::ChatMessage const& m) {
  // The web's pause before a message goes, with the message itself in the
  // box, because the mistake guarded against is the wrong one under the
  // pointer. Shift skips it.
  if (GetKeyState(VK_SHIFT) & 0x8000) { service->ChatWork([c = chatActive, id = m.id](app::Chat& chat) { chat.Remove(c, id); }); return; }
  ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark); dialog.Name(L"RemoveMessageDialog");
  ConfirmLook(dialog);
  dialog.Title(Line(L"Remover mensagem?", 20, Foreground, 600));
  StackPanel body; body.Spacing(12);
  body.Children().Append(Text(L"Ela some para todo mundo, e não dá para desfazer.", 13, Muted));
  // The message as it reads in the list: its author over it, on a raised ground.
  Border quoteBox; quoteBox.Background(Brush(Surface2)); quoteBox.CornerRadius(CornerRadius{8,8,8,8}); quoteBox.Padding(Thickness{12,10,12,10});
  StackPanel quoteBody; quoteBody.Spacing(2);
  quoteBody.Children().Append(Line(PersonName(m.author), 13, Muted, 600));
  auto quote = Text(m.body.empty() ? L"(anexo)" : net::Wide(m.body), 15, Foreground); quote.MaxLines(4); quote.TextTrimming(TextTrimming::CharacterEllipsis); quoteBody.Children().Append(quote);
  quoteBox.Child(quoteBody); body.Children().Append(quoteBox);
  dialog.Content(body); dialog.PrimaryButtonText(L"Remover"); dialog.CloseButtonText(L"Cancelar"); dialog.DefaultButton(ContentDialogButton::Close);
  auto op = dialog.ShowAsync();
  op.Completed([this, c = chatActive, id = m.id](auto const& result, auto) {
    if (result.GetResults() == ContentDialogResult::Primary) service->ChatWork([c, id](app::Chat& chat) { chat.Remove(c, id); });
  });
}

// ---------------------------------------------------------------------------
// The list: the journal from the model says what changed, and only that
// changes on screen.
// ---------------------------------------------------------------------------
uint64_t listRebuilds = 0; // read by the chat audit
// Where a conversation switch spends the window (W12): row building, the
// channel list and the scroll to the end, summed in ms for the audit.
double fillMs = 0, channelsMs = 0, scrollMs = 0, clearMs = 0; uint64_t fills = 0;
struct SpentMs { double& total; LARGE_INTEGER t0; SpentMs(double& t) : total(t) { QueryPerformanceCounter(&t0); }
  ~SpentMs() { LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f); total += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart); } };
uint64_t rowsRoles = 0;  // the roles the drawn rows were painted with
void DrawMessages() {
  if (chatActive.empty()) return;
  std::vector<app::ChatChange> changes; uint64_t sequence = 0, roles = 0; bool missed = false;
  std::vector<int64_t> ids; bool loaded = false, sending = false, hasMore = true; std::string failure;
  std::vector<std::wstring> typers;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    sequence = s.sequence; roles = s.rolesVersion;
    if (!s.changes.empty() && chatSeen && s.changes.front().sequence > chatSeen + 1) missed = true;
    for (auto const& c : s.changes) if (c.sequence > chatSeen && c.channel == chatActive) changes.push_back(c);
    auto t = s.timelines.find(chatActive);
    if (t != s.timelines.end()) {
      loaded = t->second.loaded; hasMore = t->second.hasMore; failure = t->second.error; sending = t->second.sending;
      for (auto const& m : t->second.messages) ids.push_back(m.id);
    }
    for (auto const& [key, who] : s.typing) if (key.first == chatActive && key.second != s.owner) typers.push_back(net::Wide(who.name));
  });
  bool rebuild = !chatSeen || missed;
  if (!rebuild) for (auto const& c : changes) if (c.kind == app::ChatChangeKind::Reset) rebuild = true;
  if (rebuild) {
    ++listRebuilds;
    std::vector<IInspectable> boxed; for (auto id : ids) boxed.push_back(box_value(id));
    messageItems.ReplaceAll(boxed);
    if (stickToBottom) ScrollToEnd();
  } else {
    for (auto const& c : changes) ApplyChange(c);
    // A journal applied out of step with the model is a bug to see, not to
    // keep drawing: fall back to the model's own order.
    bool same = ids.size() == messageItems.Size();
    for (size_t i = 0; same && i < ids.size(); ++i) same = unbox_value<int64_t>(messageItems.GetAt(uint32_t(i))) == ids[i];
    if (!same) { logf("chat list out of step with the model; rebuilt"); std::vector<IInspectable> boxed; for (auto id : ids) boxed.push_back(box_value(id)); messageItems.ReplaceAll(boxed); }
    // A role given, taken or recoloured repaints every name and the delete
    // and pin buttons (D12c), in place: the list does not move.
    if (roles != rowsRoles) for (uint32_t i = 0; i < messageItems.Size(); ++i) RefreshAt(int(i));
  }
  rowsRoles = roles;
  FinishMessageJump();
  if (landing.waiting && ((loaded && ids.empty()) || !failure.empty())) landing = Landing{};
  if (landing.waiting && loaded && !ids.empty()) {
    // The first page is in: the first unread line if there is one, the end otherwise.
    landing.waiting = false;
    service->ChatModel().Inspect([&](app::ChatState const& s) {
      if (auto t = s.timelines.find(chatActive); t != s.timelines.end()) landing.target = app::ChatUnreadAt(t->second, openedRead, s.owner);
    });
    landing.until = GetTickCount64() + 1500;
    stickToBottom = !landing.target;
    EndLandingLater();
  }
  if (landing.Active()) Land();
  SettleSend(sending, failure);
  SettleEdit(failure);
  // Reading at the bottom marks what arrives as read, not only the moment
  // the bottom is reached (App.tsx marks on new messages too); never while
  // the window is hidden.
  if (!ids.empty() && ids.back() != lastSeenNewest) {
    lastSeenNewest = ids.back();
    if (stickToBottom && chatShown) service->ChatWork([c = chatActive](app::Chat& chat) { chat.BottomVisible(c, true); });
  }
  chatSeen = sequence;
  historyStart.Visibility(loaded && !hasMore && !ids.empty() ? Visibility::Visible : Visibility::Collapsed);
  emptyLine.Visibility(loaded && messageItems.Size() == 0 ? Visibility::Visible : Visibility::Collapsed);
  if (typers.empty()) typingLine.Visibility(Visibility::Collapsed);
  else {
    typingLine.Text(typers.size() == 1 ? typers[0] + L" está digitando…" : typers.size() == 2 ? typers[0] + L" e " + typers[1] + L" estão digitando…" : std::to_wstring(typers.size()) + L" pessoas estão digitando…");
    typingLine.Visibility(Visibility::Visible);
  }
  ShowComposerError(net::Wide(failure));
}

int IndexOf(int64_t id) {
  for (uint32_t i = 0; i < messageItems.Size(); ++i) if (unbox_value<int64_t>(messageItems.GetAt(i)) == id) return int(i);
  return -1;
}
std::vector<int64_t> IdsAt(size_t index, size_t count) {
  std::vector<int64_t> out;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    if (auto t = s.timelines.find(chatActive); t != s.timelines.end())
      for (size_t i = index; i < index + count && i < t->second.messages.size(); ++i) out.push_back(t->second.messages[i].id);
  });
  return out;
}
void ApplyChange(app::ChatChange const& c) {
  using K = app::ChatChangeKind;
  if (c.kind == K::InsertEnd) {
    bool atEnd = stickToBottom;
    for (auto id : IdsAt(c.index, c.count)) if (IndexOf(id) < 0) messageItems.Append(box_value(id));
    // The row above a new one may stop being the last of its block.
    if (c.index > 0 && messageItems.Size() > c.count) RefreshAt(int(messageItems.Size() - c.count - 1));
    if (atEnd) ScrollToEnd();
  } else if (c.kind == K::InsertStart) {
    auto ids = IdsAt(c.index, c.count);
    for (size_t i = ids.size(); i-- > 0;) if (IndexOf(ids[i]) < 0) messageItems.InsertAt(0, box_value(ids[i]));
    RefreshAt(int(ids.size()));
  } else if (c.kind == K::Update) {
    int i = IndexOf(c.id);
    if (i < 0) { for (auto id : IdsAt(c.index, 1)) if (id == c.id) { messageItems.InsertAt(uint32_t(std::min<size_t>(c.index, messageItems.Size())), box_value(id)); } return; }
    // A row that grows at the bottom (a link card lands 0.4 s after its
    // message, a reaction) grows out of sight: follow it, as InsertEnd does
    // (the owner, 30/10/2026: "sent at once, no card").
    bool atEnd = stickToBottom && uint32_t(i) + 1 == messageItems.Size();
    RefreshAt(i); RefreshAt(i + 1);
    if (atEnd) ScrollToEnd();
  } else if (c.kind == K::Remove) {
    // One message by id, or a range the model let go of (Bound: index+count).
    if (c.id) { int i = IndexOf(c.id); if (i >= 0) { messageItems.RemoveAt(uint32_t(i)); RefreshAt(i); } return; }
    for (size_t n = 0; n < c.count && c.index < messageItems.Size(); ++n) messageItems.RemoveAt(uint32_t(c.index));
    RefreshAt(int(c.index));
  }
}
void RefreshAt(int i) {
  if (i < 0 || uint32_t(i) >= messageItems.Size()) return;
  messageItems.SetAt(uint32_t(i), messageItems.GetAt(uint32_t(i)));
}
void ScrollToEnd() {
  if (messageItems.Size() == 0) return;
  SpentMs spent{scrollMs};
  messageList.UpdateLayout();
  messageList.ScrollIntoView(messageItems.GetAt(messageItems.Size() - 1), ScrollIntoViewAlignment::Leading);
  if (messageScroll) messageScroll.ChangeView(nullptr, messageScroll.ScrollableHeight(), nullptr, true);
}
// Opening a conversation lands on its first unread line, or on its end. Rows
// are measured as they are realized and pictures arrive later, each one
// growing what is below it, so one scroll on the first layout lands short
// (the owner, 30/10/2026: it "nao ta ficando na ultima mensagem"). As the
// web does (Messages.tsx, going), the place is re-applied while the list
// settles, and the reader's own wheel or press ends it.
struct Landing { bool waiting = false; int64_t target = 0; ULONGLONG until = 0;
  bool Active() const { return !waiting && until && GetTickCount64() < until; } };
Landing landing;
bool landingQueued = false;
void Land() {
  if (!landing.Active()) return;
  if (!landing.target) { ScrollToEnd(); return; }
  int i = IndexOf(landing.target);
  if (i < 0) { landing = Landing{}; return; }
  messageList.UpdateLayout();
  messageList.ScrollIntoView(messageItems.GetAt(uint32_t(i)), ScrollIntoViewAlignment::Leading);
}
// When the window closes the reader's place is wherever it settled: few
// unread, all in view, is the end, and the end marks them read.
winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer landingEnd{nullptr};
void EndLandingLater() {
  if (!landingEnd) {
    landingEnd = window.DispatcherQueue().CreateTimer(); landingEnd.IsRepeating(false);
    landingEnd.Tick([this](auto const&, auto const&) { if (service && messageScroll) StopLanding(); });
  }
  landingEnd.Stop(); landingEnd.Interval(std::chrono::milliseconds(1600)); landingEnd.Start();
}
// Layout events cannot scroll from inside themselves; the next turn can.
void LandSoon() {
  if (!landing.Active() || landingQueued) return;
  landingQueued = true;
  auto weak = get_weak();
  window.DispatcherQueue().TryEnqueue([weak] { if (auto self = weak.get(); self && !self->exiting) { self->landingQueued = false; self->Land(); } });
}
void StopLanding() {
  if (!landing.until) return;
  landing = Landing{};
  auto offset = messageScroll.VerticalOffset(), max = messageScroll.ScrollableHeight();
  stickToBottom = max - offset < 24;
  if (service) service->ChatWork([c = chatActive, b = stickToBottom](app::Chat& chat) { chat.BottomVisible(c, b); });
}
void HookScroll() {
  if (messageScroll) return;
  messageScroll = FindDesignPart(messageList, L"ScrollViewer").try_as<ScrollViewer>();
  if (!messageScroll) return;
  ImmediateMouseWheel(messageScroll);
  if (auto panel = messageList.ItemsPanelRoot().try_as<ItemsStackPanel>()) {
    panel.ItemsUpdatingScrollMode(ItemsUpdatingScrollMode::KeepItemsInView);
    // A hittable background keeps the gaps on the same input route as rows.
    panel.Background(Tint(0,0));
    panel.SizeChanged([this](auto const&, auto const&) { LandSoon(); });
  }
  // The reader's hand ends the landing: the wheel, a press on the bar, a key.
  messageScroll.AddHandler(UIElement::PointerWheelChangedEvent(), box_value(Input::PointerEventHandler([this](auto const&, auto const&) { StopLanding(); })), true);
  messageScroll.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler([this](auto const&, auto const&) { StopLanding(); })), true);
  messageScroll.AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, auto const&) { StopLanding(); })), true);
  messageScroll.ViewChanged([this](auto const&, ScrollViewerViewChangedEventArgs const&) {
    if (!service) return; // quitting
    auto offset = messageScroll.VerticalOffset(), max = messageScroll.ScrollableHeight();
    // While landing, the moves are ours and the end is not the reader's yet.
    if (landing.waiting || landing.Active()) return;
    if (landing.until) { landing = Landing{}; }
    bool bottom = max - offset < 24;
    if (bottom != stickToBottom) { stickToBottom = bottom; service->ChatWork([c = chatActive, bottom](app::Chat& chat) { chat.BottomVisible(c, bottom); }); }
    // Past 2000 messages back, the model drops the newest end (Chat::Bound):
    // reaching the bottom brings them back, as Messages.tsx does.
    if (max - offset < 600) {
      bool newer = false, loading = false;
      service->ChatModel().Inspect([&](app::ChatState const& s) {
        if (auto t = s.timelines.find(chatActive); t != s.timelines.end()) { newer = t->second.hasNewer; loading = t->second.loading; }
      });
      if (newer && !loading) service->ChatWork([c = chatActive](app::Chat& chat) { chat.Newer(c); });
    }
    // Older pages come in before the top is reached, so the scroll never stops.
    if (offset < 600) {
      bool more = false, loading = false;
      service->ChatModel().Inspect([&](app::ChatState const& s) {
        if (auto t = s.timelines.find(chatActive); t != s.timelines.end()) { more = t->second.hasMore; loading = t->second.loading; }
      });
      if (more && !loading) service->ChatWork([c = chatActive](app::Chat& chat) { chat.Older(c); });
    }
  });
}

// ---------------------------------------------------------------------------
// One message row, the web's grid: a 48 column for the face and the rest for
// the name line and the text. Rows that continue a block (same person, under
// five minutes, same day, not a reply) have no face and no name.
// ---------------------------------------------------------------------------
struct RowData { app::ChatMessage m; std::optional<app::ChatMessage> before; std::string me; bool canEdit = false, canDelete = false, canPin = false, firstUnread = false;
                 std::optional<uint32_t> nameColor; /* the author's highest role with a colour (D12c) */ };
std::optional<RowData> ReadRow(int64_t id) {
  std::optional<RowData> out;
  auto& chat = service->ChatModel();
  chat.Inspect([&](app::ChatState const& s) {
    auto t = s.timelines.find(chatActive); if (t == s.timelines.end()) return;
    auto const& list = t->second.messages;
    for (size_t i = 0; i < list.size(); ++i) if (list[i].id == id) {
      RowData r; r.m = list[i]; r.me = s.owner;
      r.firstUnread = id == app::ChatUnreadAt(t->second, openedRead, s.owner);
      if (i > 0) r.before = list[i - 1];
      r.canEdit = r.m.author.username == s.owner && !r.m.deleted;
      r.canDelete = (r.m.author.username == s.owner || s.Moderates(chatActive)) && !r.m.deleted;
      // Chat::CanPin's rule, read here because Inspect holds the model's lock:
      // a conversation of two, either person; a channel, Gerenciar mensagens (D12c).
      r.canPin = !r.m.deleted && r.m.kind.empty() && s.Pins(chatActive);
      r.nameColor = app::ChatColorValue(app::ChatColorOf(r.m.author.username, s.roles));
      out = r; return;
    }
  });
  return out;
}
static int64_t Seconds(std::string const& stamp) { return app::ChatSeconds(stamp); }
static std::tm Local(int64_t utc) { return app::ChatLocal(utc); }
static std::wstring Clock(std::string const& stamp) { return app::ChatClock(stamp); }
static std::wstring DayLabel(std::string const& stamp) {
  auto label = app::ChatDay(stamp);
  CharUpperBuffW(label.data(), DWORD(label.size())); // Messages.tsx uppercase day separator
  return label;
}
static int DayKey(std::string const& stamp) { auto t = Local(Seconds(stamp)); return (t.tm_year + 1900) * 400 + t.tm_yday; }

void FillContainer(ListViewBase const&, ContainerContentChangingEventArgs const& args) {
  if (args.InRecycleQueue()) return;
  auto container = args.ItemContainer();
  auto slot = container.ContentTemplateRoot().try_as<Grid>();
  if (!service || exiting) return; // quitting: Quit() has handed the service away
  if (!slot) { args.RegisterUpdateCallback([this](auto const& s, auto const& next) { FillContainer(s, next); }); return; }
  SpentMs spent{fillMs}; ++fills;
  auto id = unbox_value<int64_t>(args.Item());
  slot.Children().Clear();
  if (auto row = ReadRow(id)) slot.Children().Append(MessageRow(*row));
  args.Handled(true);
}

UIElement DayLine(std::string const& stamp) {
  // The label at 12/16 600 .06em between two 1px rules, 12 apart; 12 above
  // and 4 below, which with the message's 4 leaves 8 (it was 16: the owner,
  // 25/10/2026, "less, not none").
  Grid line; line.Padding(Thickness{16, 12, 16, 4}); line.ColumnSpacing(12);
  for (auto w : {GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}, GridLength{1, GridUnitType::Star}}) {
    ColumnDefinition c; c.Width(w); line.ColumnDefinitions().Append(c);
  }
  for (int col : {0, 2}) { Border rule; rule.Height(1); rule.Background(Brush(BorderLine)); rule.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(rule, col); line.Children().Append(rule); }
  auto label = Line(DayLabel(stamp), 12, Foreground, 600); label.CharacterSpacing(60); Grid::SetColumn(label, 1); line.Children().Append(label);
  return line;
}

// The text with mentions lit: "@todos" and your own name in the foreground,
// anyone else's in the accent's soft blue, both on accent-soft and at 500.
UIElement MessageBody(app::ChatMessage const& m, std::string const& me) {
  RichTextBlock body; HarmonySelection(body); body.FontSize(16); body.LineHeight(24); body.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
  body.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); body.Foreground(Brush(Foreground)); body.CharacterSpacing(0);
  body.IsTextSelectionEnabled(true); body.TextWrapping(TextWrapping::Wrap);
  Documents::Paragraph p;
  auto text = m.body; size_t last = 0; int32_t at = 0;
  std::vector<Documents::Run> lit;
  // Text runs, with every emoji in them as the web's 22px picture on the
  // bottom of the 24px line (Twemoji.tsx withEmoji, img.emoji).
  auto plain = [&](std::string const& piece) {
    if (piece.empty()) return;
    for (auto const& part : app::SplitEmoji(piece)) {
      if (part.text.empty()) continue;
      if (!part.picture) { Documents::Run r; auto w = net::Wide(part.text); r.Text(w); p.Inlines().Append(r); at += int32_t(w.size()); continue; }
      Documents::InlineUIContainer box; auto pic = EmojiPicture(part.text, part.file, 22).as<FrameworkElement>();
      pic.Margin(Thickness{0, 0, 0, -5}); box.Child(pic); p.Inlines().Append(box); at += 1; // a picture is one position
    }
  };
  // Mentions between the links: an "@" inside an address is the address's.
  auto mentions = [&](size_t from, size_t to) {
    for (auto const& token : app::ChatMentions(text.substr(from, to - from))) {
      plain(text.substr(last, from + token.at - last));
      auto w = net::Wide(text.substr(from + token.at, token.length));
      Documents::Run r; r.Text(w); r.FontWeight(Weight(500));
      auto lower = token.username; for (auto& ch : lower) ch = char(tolower(static_cast<unsigned char>(ch)));
      bool self = lower == me || lower == "todos";
      r.Foreground(Brush(self ? Foreground : AccentSoftFg)); p.Inlines().Append(r);
      lit.push_back(r); at += int32_t(w.size());
      last = from + token.at + token.length;
    }
    plain(text.substr(last, to - last)); last = to;
  };
  for (auto const& link : app::ChatLinks(text)) {
    mentions(last, link.at);
    auto address = text.substr(link.at, link.length); auto w = net::Wide(address);
    Documents::Hyperlink h; h.UnderlineStyle(Documents::UnderlineStyle::None); h.Foreground(Brush(AccentSoftFg));
    Documents::Run r; r.Text(w); h.Inlines().Append(r); p.Inlines().Append(h);
    ToolTipService::SetToolTip(h, box_value(w));
    h.Click([w](auto const&, auto const&) { ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    at += int32_t(w.size()); last = link.at + link.length;
  }
  mentions(std::min(last, text.size()), text.size());
  if (!m.editedAt.empty()) {
    Documents::Run r; r.Text(L" (editada)"); r.FontSize(13); r.Foreground(Brush(Muted)); p.Inlines().Append(r);
  }
  body.Blocks().Append(p);
  if (lit.empty()) return body;
  // The tint under a mention is a rounded block behind the text (the owner,
  // 02/11/2026: rounded, and a little stronger), not a TextHighlighter,
  // which only paints square. Measured from each run's own ends, so it
  // follows wrapping and emoji, and the words stay selectable and copyable.
  Grid holder; Canvas marks; marks.IsHitTestVisible(false); holder.Children().Append(marks); holder.Children().Append(body);
  auto place = [marks, weak = make_weak(body), lit] {
    auto text = weak.get(); if (!text) return;
    marks.Children().Clear();
    auto block = [&](double left, double top, double right, double height) {
      if (right - left < 1) return;
      Border b; b.Background(Tint(Accent, 0.22)); b.CornerRadius(CornerRadius{4, 4, 4, 4});
      b.Width(right - left + 4); b.Height(height); Canvas::SetLeft(b, left - 2); Canvas::SetTop(b, top); marks.Children().Append(b);
    };
    for (auto const& r : lit) {
      auto from = r.ContentStart().GetCharacterRect(Documents::LogicalDirection::Forward);
      auto to = r.ContentEnd().GetCharacterRect(Documents::LogicalDirection::Backward);
      double h = 22, top = from.Y + (from.Height - h) / 2;
      if (std::abs(from.Y - to.Y) < 1) { block(from.X, top, to.X, h); continue; }
      // Broken across lines: the first piece to the end, the last from the start.
      block(from.X, top, text.ActualWidth(), h); block(0, to.Y + (to.Height - h) / 2, to.X, h);
    }
  };
  body.SizeChanged([place](auto const&, auto const&) { place(); });
  body.Loaded([place](auto const&, auto const&) { place(); });
  return holder;
}

UIElement MessageRow(RowData const& r) {
  auto const& m = r.m;
  bool blocked = false;
  if (service) service->ChatModel().Inspect([&](app::ChatState const& s) { blocked = s.Blocked(m.author.username); });
  if (blocked && !personalRevealed.contains(m.id)) {
    StackPanel folded; folded.Name(L"PersonalFold-" + std::to_wstring(m.id)); folded.Spacing(8); folded.Margin(Thickness{16, 8, 24, 8});
    folded.Children().Append(Line(L"Mensagem de pessoa bloqueada", 14, Muted, 500));
    auto show = RoleButton("", L"Mostrar", false, [this, id = m.id, weak = make_weak(folded)] {
      personalRevealed.insert(id);
      // Replacing the same boxed id does not replace an already realized row.
      if (auto host = weak.get()) if (auto row = ReadRow(id)) {
        host.Name(L"PersonalShown-" + std::to_wstring(id)); host.Margin(Thickness{0}); host.Spacing(0);
        host.Children().Clear(); host.Children().Append(MessageRow(*row));
        logf("personal message shown");
      }
    });
    show.Name(L"PersonalReveal-" + std::to_wstring(m.id)); folded.Children().Append(show); return folded;
  }
  bool newDay = !r.before || DayKey(r.before->createdAt) != DayKey(m.createdAt);
  bool grouped = !newDay && r.before && r.before->author.username == m.author.username && r.before->kind.empty() &&
                 app::ChatMilliseconds(m.createdAt) - app::ChatMilliseconds(r.before->createdAt) < 5 * 60 * 1000;
  bool quoted = m.reply.has_value();
  bool mentioned = std::find(m.mentions.begin(), m.mentions.end(), r.me) != m.mentions.end();

  StackPanel outer; outer.Background(Tint(0,0));
  if (newDay) outer.Children().Append(DayLine(m.createdAt));
  if (r.firstUnread) {
    Grid line; line.Name(L"UnreadLine"); line.Padding(Thickness{16,12,24,4}); line.ColumnSpacing(12);
    ColumnDefinition fill; fill.Width(GridLength{1,GridUnitType::Star}); line.ColumnDefinitions().Append(fill);
    ColumnDefinition words; words.Width(GridLength{0,GridUnitType::Auto}); line.ColumnDefinitions().Append(words);
    Border rule; rule.Height(1); rule.Background(Brush(0xdb3b3e)); rule.VerticalAlignment(VerticalAlignment::Center); line.Children().Append(rule);
    auto label = Line(L"NÃO LIDAS",13,DangerSoftFg,600); label.CharacterSpacing(60); Grid::SetColumn(label,1); line.Children().Append(label);
    outer.Children().Append(line);
  }
  if (m.kind == "pin") {
    // The line a new pin leaves (web PinNotice, 30/10/2026): the words come
    // from the kind, the body is only for apps that do not know it.
    Grid note; note.Margin(Thickness{16,4,24,4});
    ColumnDefinition markColumn; markColumn.Width(GridLength{48,GridUnitType::Pixel}); note.ColumnDefinitions().Append(markColumn);
    ColumnDefinition contentColumn; contentColumn.Width(GridLength{1,GridUnitType::Star}); note.ColumnDefinitions().Append(contentColumn);
    auto mark = VoiceIcon("pin",Muted,20).as<FrameworkElement>(); mark.VerticalAlignment(VerticalAlignment::Top); note.Children().Append(mark);
    RichTextBlock body; body.FontSize(16); body.Foreground(Brush(Muted)); body.TextWrapping(TextWrapping::Wrap);
    Documents::Paragraph paragraph; Documents::Run words;
    words.Text(app::ChatPinText(m.author.username == r.me,PersonName(m.author))); paragraph.Inlines().Append(words);
    Documents::Run when; when.Text(L"  " + Clock(m.createdAt)); when.FontSize(13); paragraph.Inlines().Append(when);
    body.Blocks().Append(paragraph); ToolTipService::SetToolTip(body,box_value(app::ChatFullStamp(m.createdAt)));
    Grid::SetColumn(body,1); note.Children().Append(body); outer.Children().Append(note); return outer;
  }
  if (m.kind == "call") {
    Grid note; note.Margin(Thickness{16,4,24,4});
    ColumnDefinition markColumn; markColumn.Width(GridLength{48,GridUnitType::Pixel}); note.ColumnDefinitions().Append(markColumn);
    ColumnDefinition contentColumn; contentColumn.Width(GridLength{1,GridUnitType::Star}); note.ColumnDefinitions().Append(contentColumn);
    bool rejected = m.body == "rejected";
    auto mark = VoiceIcon(rejected ? "ban" : "handset",Muted,20).as<FrameworkElement>(); mark.VerticalAlignment(VerticalAlignment::Top); note.Children().Append(mark);
    RichTextBlock body; body.FontSize(16); body.Foreground(Brush(Muted)); body.TextWrapping(TextWrapping::Wrap);
    Documents::Paragraph paragraph; Documents::Run words;
    words.Text(app::ChatCallText(m.author.username == r.me,rejected,PersonName(m.author)));
    words.Foreground(Brush(rejected ? Muted : DangerSoftFg)); paragraph.Inlines().Append(words);
    Documents::Run when; when.Text(L"  " + Clock(m.createdAt)); when.FontSize(13); paragraph.Inlines().Append(when);
    body.Blocks().Append(paragraph); ToolTipService::SetToolTip(body,box_value(app::ChatFullStamp(m.createdAt)));
    Grid::SetColumn(body,1); note.Children().Append(body); outer.Children().Append(note); return outer;
  }
  // The block gap is a margin, not padding, so the band under the pointer
  // stays centred on the text: 12 between blocks, 4 under a day line or the
  // unread line (8 in all with theirs; it was 16 under "NÃO LIDAS"), 0
  // inside a block (D4 rounds of 23–24/09/2026, the owner 25/10/2026).
  Grid frame; frame.Margin(Thickness{0, (newDay || r.firstUnread) ? 4.0 : (grouped && !quoted) ? 0.0 : 12.0, 0, 0});
  Border band; band.Name(L"MessageHoverBand"); band.CornerRadius(CornerRadius{8, 8, 8, 8}); band.Margin(Thickness{8, 0, 16, 0});
  band.Background(mentioned ? Tint(Accent, 0.12) : Tint(0, 0)); frame.Children().Append(band);
  if (mentioned) { Border bar; bar.Width(3); bar.Background(Brush(Accent)); bar.HorizontalAlignment(HorizontalAlignment::Left); bar.Margin(Thickness{0, 0, 0, 0}); frame.Children().Append(bar); }

  Grid row; row.Margin(Thickness{8, 0, 16, 0}); row.Padding(Thickness{8, 4, 8, 4});
  ColumnDefinition gutter; gutter.Width(GridLength{48, GridUnitType::Pixel}); row.ColumnDefinitions().Append(gutter);
  ColumnDefinition rest; rest.Width(GridLength{1, GridUnitType::Star}); row.ColumnDefinitions().Append(rest);
  RowDefinition q; q.Height(GridLength{0, GridUnitType::Auto}); row.RowDefinitions().Append(q);
  RowDefinition main; main.Height(GridLength{0, GridUnitType::Auto}); row.RowDefinitions().Append(main);

  if (quoted) {
    // The line being answered, on a row of its own above the face and name.
    StackPanel quote; quote.Orientation(Orientation::Horizontal); quote.Spacing(8); quote.Padding(Thickness{0, 0, 0, 2});
    auto arrow = VoiceIcon("arrow-shape-turn-up-left", Muted, 14).as<FrameworkElement>(); arrow.VerticalAlignment(VerticalAlignment::Center); quote.Children().Append(arrow);
    auto face = PhotoFace(m.reply->author, 28); face.VerticalAlignment(VerticalAlignment::Center); quote.Children().Append(face);
    auto who = Line(PersonName(m.reply->author), 15, Muted, 600); who.VerticalAlignment(VerticalAlignment::Center); quote.Children().Append(who);
    auto what = Line(m.reply->deleted ? L"Mensagem removida" : net::Wide(m.reply->body.empty() ? std::string("Imagem") : m.reply->body), 15, Muted, 500);
    what.MaxWidth(520); what.VerticalAlignment(VerticalAlignment::Center); if (m.reply->deleted) what.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
    quote.Children().Append(what);
    HarmonyPointer(quote); quote.Tapped([this, id = m.reply->id](auto const&, auto const&) {
      JumpToMessage(chatActive,id);
    });
    Grid::SetColumn(quote, 1); row.Children().Append(quote);
  }
  if (!grouped || quoted) {
    // 36 against a 24 name line plus a 24 first line: 6 down centres it.
    auto face = PhotoFace(m.author, 36); face.VerticalAlignment(VerticalAlignment::Top); face.HorizontalAlignment(HorizontalAlignment::Left);
    BindPerson(face,m.author); face.Margin(Thickness{0, 6, 0, 0}); Grid::SetRow(face, 1); row.Children().Append(face);
  }
  StackPanel text; Grid::SetRow(text, 1); Grid::SetColumn(text, 1);
  if (!grouped || quoted) {
    StackPanel head; head.Orientation(Orientation::Horizontal); head.Spacing(8); head.Height(24);
    auto name = Line(PersonName(m.author), 16, r.nameColor.value_or(Foreground), 600); name.Name(L"MessageAuthor"); BindPerson(name,m.author);
    name.PointerEntered([](auto const& eventSender,auto const&) { auto name = eventSender.template as<TextBlock>(); AuthorHover(name,true); });
    name.PointerMoved([](auto const& eventSender,auto const&) { auto name = eventSender.template as<TextBlock>(); AuthorHover(name,true); });
    name.PointerExited([](auto const& eventSender,auto const&) { auto name = eventSender.template as<TextBlock>(); AuthorHover(name,false); }); head.Children().Append(name);
    auto when = Line(Clock(m.createdAt), 13, Muted, 400); ToolTipService::SetToolTip(when, box_value(app::ChatFullStamp(m.createdAt))); when.CharacterSpacing(-20); when.Margin(Thickness{0, 4, 0, 0}); head.Children().Append(when);
    text.Children().Append(head);
  }
  if (m.deleted) {
    auto gone = Text(L"Mensagem removida" + (m.deletedBy.empty() ? std::wstring() : L" por @" + net::Wide(m.deletedBy)), 16, Muted);
    gone.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic); text.Children().Append(gone);
  } else {
    if (editingId == m.id) text.Children().Append(InlineEditBox());
    else if (!m.body.empty() || !m.editedAt.empty()) text.Children().Append(MessageBody(m, r.me));
    if (auto extra = MessageExtras(m, r.me)) text.Children().Append(extra);
  }
  row.Children().Append(text);
  frame.Children().Append(row);

  // The actions float over the row's top edge (-top-4 right-2), on surface-3
  // with p-1, 32 squares; remove is red at rest. Only under the pointer.
  if (!m.deleted && editingId != m.id) {
    Border holder; auto weakHolder = winrt::make_weak(holder);
    // The buttons are made on the row's first hover, not with the row: their
    // icons were ~2.4 of the 3.7 ms a row cost, and a conversation switch
    // builds every row it shows (W12: 35 rows, 130 ms, for 25 messages).
    auto fill = [this, m, weakHolder, canEdit = r.canEdit, canDelete = r.canDelete, canPin = r.canPin](Border const& h) {
      if (h.Child()) return;
      StackPanel actions; actions.Orientation(Orientation::Horizontal); actions.Spacing(2);
      actions.Children().Append(Square("face-smile", L"reagir", 32, 16, Muted, 0x3f3f46, [this, m, weakHolder] { if (auto h = weakHolder.get()) OpenReactionPanel(m, h); }));
      actions.Children().Append(Square("arrow-shape-turn-up-left", L"responder", 32, 16, Muted, 0x3f3f46, [this, id = m.id] { SetReply(id); }));
      if (canPin) {
        // Pressed while pinned, like a reaction (Messages.tsx data-pin).
        auto pin = Square("pin", m.pinned ? L"desafixar" : L"fixar", 32, 16, m.pinned ? Foreground : Muted, 0x3f3f46,
                          [this, c = chatActive, id = m.id, on = !m.pinned] { PinMessage(c, id, on); });
        pin.Name(L"PinAction"); if (m.pinned) pin.Background(Brush(0x3f3f46));
        actions.Children().Append(pin);
      }
      if (canEdit) actions.Children().Append(Square("pencil", L"editar", 32, 16, Muted, 0x3f3f46, [this, m] { StartEdit(m); }));
      if (canDelete) actions.Children().Append(Square("trash-bin", L"remover", 32, 16, DangerSoftFg, 0xdb3b3e, [this, m] { RemoveMessage(m); }));
      h.Child(actions);
    };
    holder.Name(L"Actions"); holder.Background(Brush(Surface3)); holder.CornerRadius(CornerRadius{8, 8, 8, 8}); holder.Padding(Thickness{4, 4, 4, 4});
    // Counted with the pin: without it, a bar with all five cut "remover" off
    // its end (the owner, 02/11/2026: "não tá aparecendo o de deletar").
    holder.Width((2 + (r.canPin ? 1 : 0) + (r.canEdit ? 1 : 0) + (r.canDelete ? 1 : 0))*34+6); holder.Height(40);
    Canvas overlay; overlay.Width(0); overlay.Height(0); overlay.HorizontalAlignment(HorizontalAlignment::Right); overlay.VerticalAlignment(VerticalAlignment::Top);
    Canvas::SetLeft(holder,-holder.Width()-24); Canvas::SetTop(holder,-16); overlay.Children().Append(holder);
    holder.Opacity(0); holder.IsHitTestVisible(false); Canvas::SetZIndex(holder, 10);
    // Shown some other way than the pointer (the audits, a property watcher)
    // the bar still carries its buttons.
    holder.RegisterPropertyChangedCallback(UIElement::OpacityProperty(), [fill](auto const& sender, auto const&) { auto h = sender.template as<Border>(); if (h.Opacity() > 0) fill(h); });
    // And made anyway once the window is idle, one row a turn, so the first
    // hover finds them ready (round 6 holds the hover under 16 ms).
    window.DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weakHolder, fill] { if (auto h = weakHolder.get()) fill(h); });
    frame.Children().Append(overlay);
    frame.PointerEntered([band, holder, mentioned, fill](auto const&, auto const&) { fill(holder); MessageHoverVisual(band,holder,mentioned,true); });
    frame.PointerExited([band, holder, mentioned](auto const&, auto const&) { MessageHoverVisual(band,holder,mentioned,false); });
  }
  outer.Children().Append(frame);
  Automation::AutomationProperties::SetName(outer, PersonName(m.author) + L", " + Clock(m.createdAt) + L": " + net::Wide(m.deleted ? std::string("mensagem removida") : m.body));
  return outer;
}

// W5b fills these in: pictures, files and reactions under the text, the
// emoji panel, the reaction panel, and attaching a file.
// ---------------------------------------------------------------------------
// Under the text: pictures, files, files the server no longer has, and the
// reactions, in that order, each pt-1 (Messages.tsx Images/Files/Gone).
// ---------------------------------------------------------------------------
static std::wstring PrettySize(int64_t bytes) {
  wchar_t b[32];
  if (bytes >= (1 << 20)) { swprintf_s(b, L"%.1f MB", bytes / double(1 << 20)); for (auto& c : b) if (c == L'.') c = L','; return b; }
  if (bytes >= (1 << 10)) return std::to_wstring(int64_t(std::llround(bytes / 1024.0))) + L" KB";
  return std::to_wstring(bytes) + L" B";
}
UIElement MessageExtras(app::ChatMessage const& m, std::string const& me) {
  std::vector<app::ChatAttachment> pictures, files, gone;
  for (auto const& f : m.attachments) (f.missing ? gone : f.mime.rfind("image/", 0) == 0 ? pictures : files).push_back(f);
  auto const& embeds = m.deleted ? std::vector<app::ChatEmbed>{} : m.embeds;
  if (pictures.empty() && files.empty() && gone.empty() && embeds.empty() && m.reactions.empty()) return nullptr;
  StackPanel extras;
  if (pictures.size() == 1) {
    // Alone, the box is the picture: up to 384 tall and the column wide.
    auto const& f = pictures[0];
    Grid availableSpace; availableSpace.Margin(Thickness{0,4,0,0});
    Border frame; frame.CornerRadius(CornerRadius{8, 8, 8, 8}); frame.HorizontalAlignment(HorizontalAlignment::Left);
    Image image; image.Name(L"SingleMessageImage"); image.Stretch(Stretch::Uniform); image.MaxHeight(384); image.HorizontalAlignment(HorizontalAlignment::Left);
    availableSpace.SizeChanged([image, f](auto const&, SizeChangedEventArgs const& e) {
      double available = e.NewSize().Width;
      if (available <= 0) return;
      image.MaxWidth(available);
      if (f.width > 0 && f.height > 0) {
        double scale = std::min({1.0,384.0 / f.height,available / f.width});
        image.Width(f.width * scale); image.Height(f.height * scale);
      }
    });
    Automation::AutomationProperties::SetName(image, net::Wide(f.name)); ToolTipService::SetToolTip(frame, box_value(net::Wide(f.name)));
    LoadPicture(f.url, false, [image](ImageSource s) { if (s) image.Source(s); }, PictureDecodeHeight(f, 384));
    frame.Child(image); image.Tapped([this, f, m](auto const&, auto const&) { OpenLightbox(f, m); }); MenuAtPointer(frame, PictureMenu(f));
    availableSpace.Children().Append(frame); extras.Children().Append(availableSpace);
  } else if (!pictures.empty()) {
    // In company, a row of squares, 4 apart and 400 across, cropped.
    Grid row; row.Width(400); row.HorizontalAlignment(HorizontalAlignment::Left); row.ColumnSpacing(4); row.Margin(Thickness{0, 4, 0, 0});
    double side = (400.0 - 4 * double(pictures.size() - 1)) / double(pictures.size());
    for (size_t i = 0; i < pictures.size(); ++i) {
      auto const& f = pictures[i];
      ColumnDefinition c; c.Width(GridLength{1, GridUnitType::Star}); row.ColumnDefinitions().Append(c);
      Border cell; cell.Height(side); cell.CornerRadius(CornerRadius{8, 8, 8, 8}); cell.Background(Brush(Surface2));
      Image image; image.Stretch(Stretch::UniformToFill); image.HorizontalAlignment(HorizontalAlignment::Center); image.VerticalAlignment(VerticalAlignment::Center);
      Automation::AutomationProperties::SetName(image, net::Wide(f.name));
      LoadPicture(f.url, false, [image](ImageSource s) { if (s) image.Source(s); }, PictureDecodeHeight(f, side, side));
      cell.Child(image); ToolTipService::SetToolTip(cell, box_value(net::Wide(f.name)));
      HarmonyPointer(cell); cell.Tapped([this, f, m](auto const&, auto const&) { OpenLightbox(f, m); }); MenuAtPointer(cell, PictureMenu(f));
      Grid::SetColumn(cell, int32_t(i)); row.Children().Append(cell);
    }
    extras.Children().Append(row);
  }
  for (auto const& f : files) {
    // The card: 320 across, the kind of file at 28 on the left, the name
    // over its size, the download square on the right; the card is the button.
    Border card; card.Width(320); card.HorizontalAlignment(HorizontalAlignment::Left); card.Background(Brush(Surface));
    card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Padding(Thickness{12, 12, 12, 12}); card.Margin(Thickness{0, 4, 0, 0});
    Grid g; g.ColumnSpacing(12);
    for (auto w : {GridLength{28, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{40, GridUnitType::Pixel}}) { ColumnDefinition c; c.Width(w); g.ColumnDefinitions().Append(c); }
    auto kind = f.mime == "application/pdf" ? "file-text" : f.mime == "application/zip" ? "file-zipper" : "file";
    auto icon = VoiceIcon(kind, Muted, 28).as<FrameworkElement>(); icon.VerticalAlignment(VerticalAlignment::Center); g.Children().Append(icon);
    StackPanel words; words.VerticalAlignment(VerticalAlignment::Center);
    auto title = Line(net::Wide(f.name), 15, Foreground, 500); title.LineHeight(22); words.Children().Append(title);
    auto weight = Line(PrettySize(f.size), 13, Muted, 400); weight.LineHeight(18); words.Children().Append(weight);
    Grid::SetColumn(words, 1); g.Children().Append(words);
    Border arrow; arrow.Width(40); arrow.Height(40); arrow.CornerRadius(CornerRadius{8, 8, 8, 8}); arrow.Background(Brush(Default));
    auto glyph = VoiceIcon("file-arrow-down", Foreground, 16).as<FrameworkElement>(); glyph.HorizontalAlignment(HorizontalAlignment::Center); glyph.VerticalAlignment(VerticalAlignment::Center);
    arrow.Child(glyph); Grid::SetColumn(arrow, 2); g.Children().Append(arrow);
    card.Child(g); ToolTipService::SetToolTip(card, box_value(L"Baixar " + net::Wide(f.name)));
    Automation::AutomationProperties::SetName(card, L"Baixar " + net::Wide(f.name));
    // The sender, not a capture: a control holding itself in its own handler never frees.
    card.PointerEntered([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface2)); });
    card.PointerExited([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface)); });
    card.Tapped([this, f](auto const&, auto const&) { Download(f); });
    extras.Children().Append(card);
  }
  for (auto const& f : gone) {
    Border card; card.MaxWidth(320); card.HorizontalAlignment(HorizontalAlignment::Left); card.Background(Brush(Surface2));
    card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Padding(Thickness{12, 12, 12, 12}); card.Margin(Thickness{0, 4, 0, 0});
    Grid g; g.ColumnSpacing(12);
    for (auto w : {GridLength{24, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}}) { ColumnDefinition c; c.Width(w); g.ColumnDefinitions().Append(c); }
    auto icon = VoiceIcon("file", Muted, 24).as<FrameworkElement>(); icon.VerticalAlignment(VerticalAlignment::Center); g.Children().Append(icon);
    StackPanel words; words.Children().Append(Line(net::Wide(f.name), 13, Muted, 400)); words.Children().Append(Line(L"Este arquivo não está mais no servidor.", 13, Muted, 400));
    Grid::SetColumn(words, 1); g.Children().Append(words); card.Child(g); extras.Children().Append(card);
  }
  for (auto const& e : embeds) {
    // The web's card (Messages.tsx Embeds): 432 across, the site over the
    // title over the description; a video or a wide picture goes under them
    // at the card's width, anything else is an 80 square on the right. The
    // card opens the page in the browser; nothing plays here.
    bool wide = !e.image.empty() && (e.kind == "video" || e.width >= 400);
    Border card; card.MaxWidth(432); card.HorizontalAlignment(HorizontalAlignment::Left); card.Background(Brush(Surface2));
    card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Padding(Thickness{12, 12, 12, 12}); card.Margin(Thickness{0, 4, 0, 0});
    Grid g; g.ColumnSpacing(12);
    ColumnDefinition text; text.Width(GridLength{1, GridUnitType::Star}); g.ColumnDefinitions().Append(text);
    StackPanel words; words.Spacing(4);
    if (!e.site.empty()) { auto site = Line(net::Wide(e.site), 13, Muted, 400); site.LineHeight(18); site.TextTrimming(TextTrimming::CharacterEllipsis); words.Children().Append(site); }
    if (!e.title.empty()) {
      auto title = Line(net::Wide(e.title), 15, AccentSoftFg, 600); title.LineHeight(22);
      title.TextWrapping(TextWrapping::Wrap); title.MaxLines(2); title.TextTrimming(TextTrimming::CharacterEllipsis); words.Children().Append(title);
    }
    if (!e.description.empty()) {
      auto about = Line(net::Wide(e.description), 13, Muted, 400); about.LineHeight(18);
      about.TextWrapping(TextWrapping::Wrap); about.MaxLines(3); about.TextTrimming(TextTrimming::CharacterEllipsis); words.Children().Append(about);
    }
    if (wide) {
      double width = 408, height = e.width > 0 && e.height > 0 ? std::min(288.0, width * double(e.height) / double(e.width)) : 229.5;
      Grid frame; frame.Width(width); frame.Height(height); frame.Margin(Thickness{0, 4, 0, 0}); frame.HorizontalAlignment(HorizontalAlignment::Left);
      Border clip; clip.CornerRadius(CornerRadius{8, 8, 8, 8}); clip.Background(Brush(Surface3));
      Image image; image.Stretch(Stretch::UniformToFill); image.HorizontalAlignment(HorizontalAlignment::Center); image.VerticalAlignment(VerticalAlignment::Center);
      LoadPicture(e.image, false, [image](ImageSource s) { if (s) image.Source(s); });
      clip.Child(image); frame.Children().Append(clip);
      if (e.kind == "video") {
        Shapes::Ellipse disc; disc.Width(56); disc.Height(56); disc.Fill(Tint(0, 0.6));
        disc.HorizontalAlignment(HorizontalAlignment::Center); disc.VerticalAlignment(VerticalAlignment::Center); frame.Children().Append(disc);
        Shapes::Polygon play; play.Fill(Brush(0xffffff)); play.Width(56); play.Height(56);
        for (auto [x, y] : {std::pair{23., 18.}, std::pair{39., 28.}, std::pair{23., 38.}}) play.Points().Append(winrt::Windows::Foundation::Point{float(x), float(y)});
        play.HorizontalAlignment(HorizontalAlignment::Center); play.VerticalAlignment(VerticalAlignment::Center); frame.Children().Append(play);
      }
      words.Children().Append(frame);
    }
    g.Children().Append(words);
    if (!wide && !e.image.empty()) {
      ColumnDefinition side; side.Width(GridLength{80, GridUnitType::Pixel}); g.ColumnDefinitions().Append(side);
      Border thumb; thumb.Width(80); thumb.Height(80); thumb.CornerRadius(CornerRadius{8, 8, 8, 8}); thumb.Background(Brush(Surface3)); thumb.VerticalAlignment(VerticalAlignment::Top);
      Image image; image.Stretch(Stretch::UniformToFill); image.HorizontalAlignment(HorizontalAlignment::Center); image.VerticalAlignment(VerticalAlignment::Center);
      LoadPicture(e.image, false, [image](ImageSource s) { if (s) image.Source(s); });
      thumb.Child(image); Grid::SetColumn(thumb, 1); g.Children().Append(thumb);
    }
    card.Child(g); card.Name(L"LinkEmbed");
    auto address = net::Wide(e.url); ToolTipService::SetToolTip(card, box_value(address));
    Automation::AutomationProperties::SetName(card, net::Wide(e.title.empty() ? e.url : e.title));
    card.PointerEntered([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface3)); });
    card.PointerExited([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface2)); });
    HarmonyPointer(card); card.Tapped([address](auto const&, auto const&) { ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    extras.Children().Append(card);
  }
  if (!m.reactions.empty()) {
    // 24 tall chips, 4 apart; yours on accent-soft with a 1px accent ring,
    // the others on surface-2, the count at 600.
    StackPanel chips; chips.Orientation(Orientation::Horizontal); chips.Spacing(4); chips.Margin(Thickness{0, 4, 0, 0});
    for (auto const& r : m.reactions) {
      bool mine = r.Includes(me);
      Border chip; chip.Height(24); chip.CornerRadius(CornerRadius{8, 8, 8, 8}); chip.Padding(Thickness{8, 0, 8, 0});
      chip.Background(mine ? Tint(Accent, 0.12) : Brush(Surface2));
      // The web's ring is an inset shadow and takes no room; a WinUI border does,
      // so the padding gives the pixel back.
      if (mine) { chip.BorderBrush(Brush(Accent)); chip.BorderThickness(Thickness{1, 1, 1, 1}); chip.Padding(Thickness{7, 0, 7, 0}); }
      StackPanel inside; inside.Orientation(Orientation::Horizontal); inside.Spacing(4); inside.VerticalAlignment(VerticalAlignment::Center);
      auto pieces = app::SplitEmoji(r.emoji);
      if (pieces.size() == 1 && pieces[0].picture) inside.Children().Append(EmojiPicture(r.emoji, pieces[0].file, 16));
      else inside.Children().Append(Line(net::Wide(r.emoji), 13, Foreground, 400));
      auto count = Line(std::to_wstring(r.users.size()), 13, mine ? AccentSoftFg : Muted, 600); count.VerticalAlignment(VerticalAlignment::Center); inside.Children().Append(count);
      chip.Child(inside);
      std::wstring who; for (auto const& u : r.users) { if (!who.empty()) who += L", "; who += NameOf(u); }
      ToolTipService::SetToolTip(chip, box_value(who));
      Automation::AutomationProperties::SetName(chip, net::Wide(r.emoji) + L" " + std::to_wstring(r.users.size()));
      if (!mine) {
        chip.PointerEntered([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface3)); });
        chip.PointerExited([](IInspectable const& s, auto const&) { s.as<Border>().Background(Brush(Surface2)); });
      }
      HarmonyPointer(chip); chip.Tapped([this, id = m.id, emoji = r.emoji, mine](auto const&, auto const&) { React(id, emoji, !mine); });
      chips.Children().Append(chip);
    }
    extras.Children().Append(chips);
  }
  return extras;
}
std::wstring NameOf(std::string const& username) {
  std::wstring out; if (!service) return net::Wide(username);
  service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& p : s.members) if (p.username == username) out = PersonName(p); });
  return out.empty() ? net::Wide(username) : out;
}
void React(int64_t id, std::string const& emoji, bool on) {
  if (on) RememberRecent(emoji);
  if (!service) return;
  service->ChatWork([c = chatActive, id, emoji, on](app::Chat& chat) { chat.React(c, id, emoji, on); });
}

// The picture on its own, over the app: Esc, the X or a click on the black
// all come back; "Baixar" keeps the file (Messages.tsx Lightbox).
Grid lightbox{nullptr};
std::vector<std::function<void()>> lightboxCleanup;
std::filesystem::path lastDownload;
std::function<void(double,double,double)> lightboxWheel;
std::function<void(winrt::Windows::Foundation::Point)> lightboxDragBegin,lightboxDragTo;
std::function<void()> lightboxDragEnd;
void OpenLightbox(app::ChatAttachment const& f, app::ChatMessage const& m) {
  CloseLightbox();
  lightbox = Grid(); lightbox.Background(Brush(Background)); lightbox.Padding(Thickness{32, 32, 32, 32});
  Grid::SetColumnSpan(lightbox, 3); Canvas::SetZIndex(lightbox, 100);
  // The picture at its own size, never stretched past it (object-contain,
  // max-w-full, max-h 100dvh - 160), centred, with the control's corners.
  Border frame; frame.Name(L"LightboxFrame"); frame.CornerRadius(CornerRadius{8, 8, 8, 8}); frame.HorizontalAlignment(HorizontalAlignment::Center); frame.VerticalAlignment(VerticalAlignment::Center);
  Image image; image.Name(L"LightboxImage"); image.Stretch(Stretch::Uniform);
  auto available = root.ActualSize();
  image.MaxHeight(std::max(0.0, double(available.y) - 160)); image.MaxWidth(std::max(0.0, double(available.x) - 64));
  if (f.width > 0 && f.height > 0) { image.MaxWidth(std::min(image.MaxWidth(), double(f.width))); image.MaxHeight(std::min(image.MaxHeight(), double(f.height))); }
  Automation::AutomationProperties::SetName(image, net::Wide(f.name));
  LoadPicture(f.url, false, [image](ImageSource s) { if (s) image.Source(s); });
  frame.Child(image);
  Grid viewport; viewport.Name(L"ImageViewport"); viewport.Margin(Thickness{0,48,0,48}); viewport.Children().Append(frame);
  auto clip=RectangleGeometry(); viewport.Clip(clip);
  auto viewerSizeChanged=viewport.SizeChanged([clip](auto const&,SizeChangedEventArgs const& e) {clip.Rect({0,0,float(e.NewSize().Width),float(e.NewSize().Height)});});
  lightboxCleanup.push_back([viewport,viewerSizeChanged] {viewport.SizeChanged(viewerSizeChanged);});
  lightbox.Children().Append(viewport);
  auto transform=CompositeTransform(); frame.RenderTransform(transform); frame.RenderTransformOrigin({.5f,.5f});
  auto zoom=[transform,image](double scale,double x=0,double y=0) {
    double limit=16;
    if(auto source=image.Source().try_as<Imaging::BitmapSource>();source && image.ActualWidth()>0) limit=std::max(limit,source.PixelWidth()/image.ActualWidth());
    double next=std::clamp(scale,.25,limit),ratio=next/transform.ScaleX();
    transform.TranslateX(x-(x-transform.TranslateX())*ratio); transform.TranslateY(y-(y-transform.TranslateY())*ratio);
    transform.ScaleX(next);transform.ScaleY(next);
  };
  lightboxWheel=[viewport,transform,zoom](double delta,double x,double y) {
    zoom(transform.ScaleX()*std::exp(delta*.002),x-viewport.ActualWidth()/2,y-viewport.ActualHeight()/2);
  };
  auto viewerPointerWheelChanged=viewport.PointerWheelChanged([this](auto const& eventSender,Input::PointerRoutedEventArgs const& e) { auto viewport = eventSender.template as<Grid>();
    auto point=e.GetCurrentPoint(viewport); auto p=point.Position();
    lightboxWheel(point.Properties().MouseWheelDelta(),p.X,p.Y);e.Handled(true);
  });
  lightboxCleanup.push_back([viewport,viewerPointerWheelChanged] {viewport.PointerWheelChanged(viewerPointerWheelChanged);});
  auto viewerDoubleTapped=viewport.DoubleTapped([image,transform,zoom](auto const&,Input::DoubleTappedRoutedEventArgs const& e) {
    auto source=image.Source().try_as<Imaging::BitmapSource>();
    if(transform.ScaleX()!=1) {transform.TranslateX(0);transform.TranslateY(0);zoom(1);}
    else if(source && image.ActualWidth()>0) zoom(source.PixelWidth()/image.ActualWidth());e.Handled(true);
  });
  lightboxCleanup.push_back([viewport,viewerDoubleTapped] {viewport.DoubleTapped(viewerDoubleTapped);});
  auto drag=std::make_shared<std::optional<winrt::Windows::Foundation::Point>>();
  auto imageGesture=std::make_shared<bool>(false);
  auto viewerTapped=viewport.Tapped([imageGesture](auto const&,Input::TappedRoutedEventArgs const& e) {if(*imageGesture) {*imageGesture=false;e.Handled(true);}});
  lightboxCleanup.push_back([viewport,viewerTapped] {viewport.Tapped(viewerTapped);});
  lightboxDragBegin=[drag,imageGesture](winrt::Windows::Foundation::Point point) {*imageGesture=true;*drag=point;};
  lightboxDragTo=[drag,transform](winrt::Windows::Foundation::Point point) {
    if(!*drag) return;
    if(transform.ScaleX()>1) {transform.TranslateX(transform.TranslateX()+point.X-(**drag).X);transform.TranslateY(transform.TranslateY()+point.Y-(**drag).Y);}
    *drag=point;
  };
  lightboxDragEnd=[drag] {drag->reset();};
  auto viewerPointerPressed=viewport.PointerPressed([this, drag, image, imageGesture](auto const& eventSender,Input::PointerRoutedEventArgs const& e) { auto viewport = eventSender.template as<Grid>();
    auto point=e.GetCurrentPoint(viewport);if(!point.Properties().IsLeftButtonPressed() || e.OriginalSource()!=image.as<IInspectable>()) return;
    lightboxDragBegin(point.Position());viewport.CapturePointer(e.Pointer());e.Handled(true);
  });
  lightboxCleanup.push_back([viewport,viewerPointerPressed] {viewport.PointerPressed(viewerPointerPressed);});
  auto viewerPointerMoved=viewport.PointerMoved([this, drag](auto const& eventSender,Input::PointerRoutedEventArgs const& e) { auto viewport = eventSender.template as<Grid>();
    if(!*drag) return;auto point=e.GetCurrentPoint(viewport).Position();
    lightboxDragTo(point);e.Handled(true);
  });
  lightboxCleanup.push_back([viewport,viewerPointerMoved] {viewport.PointerMoved(viewerPointerMoved);});
  auto viewerPointerReleased=viewport.PointerReleased([this](auto const& eventSender,Input::PointerRoutedEventArgs const& e) { auto viewport = eventSender.template as<Grid>();lightboxDragEnd();viewport.ReleasePointerCapture(e.Pointer());});
  lightboxCleanup.push_back([viewport,viewerPointerReleased] {viewport.PointerReleased(viewerPointerReleased);});
  auto viewerPointerCaptureLost=viewport.PointerCaptureLost([this](auto const&,auto const&) {lightboxDragEnd();});
  lightboxCleanup.push_back([viewport,viewerPointerCaptureLost] {viewport.PointerCaptureLost(viewerPointerCaptureLost);});
  image.Tapped([](auto const&,Input::TappedRoutedEventArgs const& e) {e.Handled(true);});
  StackPanel zoomBar;zoomBar.Name(L"ImageZoomControls");zoomBar.Orientation(Orientation::Horizontal);zoomBar.Spacing(8);zoomBar.HorizontalAlignment(HorizontalAlignment::Center);zoomBar.VerticalAlignment(VerticalAlignment::Bottom);zoomBar.Margin(Thickness{0,0,0,12});
  for(int i=0;i<3;++i) {
    Button button;button.Name(i==0?L"ImageZoomOut":i==1?L"ImageFit":L"ImageZoomIn");HarmonyPointer(button);button.Height(36);button.MinWidth(36);button.Padding(Thickness{12,0,12,0});button.Background(Brush(Surface));button.CornerRadius(CornerRadius{8,8,8,8});button.Content(Line(i==0?L"\u2212":i==1?L"Ajustar":L"+",15,Foreground,500));
    Automation::AutomationProperties::SetName(button,i==0?L"Diminuir zoom":i==1?L"Tamanho ajustado":L"Aumentar zoom");
    button.Click([transform,zoom,i](auto const&,auto const&) {if(i==1) {transform.TranslateX(0);transform.TranslateY(0);zoom(1);}else zoom(transform.ScaleX()*(i==0?.8:1.25));});zoomBar.Children().Append(button);
  }
  lightbox.Children().Append(zoomBar);
  // Top left: the Esc key drawn as a key, then the words (lightbox-hint).
  Border hint; hint.Height(32); hint.Padding(Thickness{12, 0, 12, 0}); hint.CornerRadius(CornerRadius{8, 8, 8, 8}); hint.Background(Brush(Surface));
  hint.HorizontalAlignment(HorizontalAlignment::Left); hint.VerticalAlignment(VerticalAlignment::Top); hint.Margin(Thickness{-16, -16, 0, 0});
  StackPanel hintRow; hintRow.Orientation(Orientation::Horizontal); hintRow.Spacing(8); hintRow.VerticalAlignment(VerticalAlignment::Center);
  Border key; key.Height(18); key.Padding(Thickness{6, 0, 6, 0}); key.CornerRadius(CornerRadius{8, 8, 8, 8}); key.Background(Brush(Surface2));
  auto keyText = Line(L"Esc", 13, Muted, 400); keyText.VerticalAlignment(VerticalAlignment::Center); key.Child(keyText); hintRow.Children().Append(key);
  auto hintText = Line(L"ou um clique no fundo", 13, Muted, 400); hintText.VerticalAlignment(VerticalAlignment::Center); hintRow.Children().Append(hintText);
  hint.Child(hintRow); lightbox.Children().Append(hint);
  // Top right: "Baixar" 44 tall with its arrow, px-4 gap-2 at 15/500, and the X square.
  StackPanel corner; corner.Orientation(Orientation::Horizontal); corner.Spacing(8); corner.HorizontalAlignment(HorizontalAlignment::Right); corner.VerticalAlignment(VerticalAlignment::Top); corner.Margin(Thickness{0, -8, -8, 0});
  Button save; HarmonyPointer(save); save.Height(44); save.MinHeight(0); save.Padding(Thickness{16, 0, 16, 0}); save.CornerRadius(CornerRadius{8, 8, 8, 8}); save.BorderThickness(Thickness{0, 0, 0, 0});
  save.Background(Brush(Surface)); save.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Surface2)); save.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Surface2));
  StackPanel saveRow; saveRow.Orientation(Orientation::Horizontal); saveRow.Spacing(8);
  auto arrow = VoiceIcon("arrow-down-to-line", Foreground, 16).as<FrameworkElement>(); arrow.VerticalAlignment(VerticalAlignment::Center); saveRow.Children().Append(arrow);
  auto saveText = Line(L"Baixar", 15, Foreground, 500); saveText.VerticalAlignment(VerticalAlignment::Center); saveRow.Children().Append(saveText);
  save.Content(saveRow); Automation::AutomationProperties::SetName(save, L"Baixar");
  save.Click([this, f](auto const&, auto const&) { if (!exiting) Download(f); });
  corner.Children().Append(save);
  auto close = Square("xmark", L"fechar", 44, 20, Foreground, Surface2, [this] { CloseLightbox(); }); close.Name(L"LightboxClose"); close.Background(Brush(Surface)); corner.Children().Append(close);
  lightbox.Children().Append(corner);
  std::wstring line = PersonName(m.author) + L" · " + Clock(m.createdAt) + L" · " + net::Wide(f.name) + L" · " + PrettySize(f.size);
  auto caption = Line(line, 13, Muted, 400); caption.LineHeight(18); caption.HorizontalAlignment(HorizontalAlignment::Center); caption.VerticalAlignment(VerticalAlignment::Bottom); caption.Margin(Thickness{0, 0, 0, -16});
  lightbox.Children().Append(caption);
  HarmonyPointer(lightbox); lightbox.Tapped([this](auto const&, Input::TappedRoutedEventArgs const& e) {
    if (e.OriginalSource() == lightbox.as<IInspectable>() || e.OriginalSource().try_as<Grid>() == FindDesignPart(lightbox,L"ImageViewport")) CloseLightbox();
  });
  lightbox.KeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) { if (e.Key() == winrt::Windows::System::VirtualKey::Escape) { CloseLightbox(); e.Handled(true); } });
  lightbox.IsTabStop(true); root.Children().Append(lightbox); lightbox.Focus(FocusState::Programmatic);
}
void CloseLightbox() {
  lightboxWheel={};lightboxDragBegin={};lightboxDragTo={};lightboxDragEnd={};
  for(auto& cleanup:lightboxCleanup) cleanup();
  lightboxCleanup.clear();
  if (!lightbox) return;
  uint32_t at{}; if (root.Children().IndexOf(lightbox, at)) root.Children().RemoveAt(at);
  lightbox = nullptr; if (composer) composer.Focus(FocusState::Programmatic);
}

// The server's name for a file, made safe for this disk: no separators or
// characters Windows refuses, no trailing dots or spaces, and never a device
// name (CON, NUL, COM1... with or without an extension).
static std::wstring SafeFileName(std::wstring name) {
  for (auto& c : name) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
  while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
  if (name.empty()) return L"arquivo";
  auto stem = name.substr(0, name.find(L'.'));
  for (auto& c : stem) c = wchar_t(towupper(c));
  while (!stem.empty() && stem.back() == L' ') stem.pop_back();
  static const wchar_t* devices[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
                                     L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
  for (auto d : devices) if (stem == d) return L"_" + name;
  return name;
}
// The right button on a picture (the owner, 02/11/2026): copy it, to paste
// anywhere, or keep it, as "Baixar" in the big view does.
MenuFlyout PictureMenu(app::ChatAttachment const& f) {
  MenuFlyout menu;
  menu.Items().Append(ColumnItem(L"Copiar imagem", [this, f] { CopyPicture(f); }));
  menu.Items().Append(ColumnItem(L"Baixar imagem", [this, f] { Download(f); }));
  return menu;
}
// The bytes as they came, through the same bounded download, on the
// clipboard as a picture: Paint, Discord, WhatsApp and Word take it.
void CopyPicture(app::ChatAttachment const& f) {
  if (!service) return;
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  service->ChatImage(f.url, [dispatcher, weak](std::shared_ptr<const std::string> bytes) {
    dispatcher.TryEnqueue([weak, bytes] { if (auto self = weak.get(); self && !self->exiting) self->PutPicture(bytes); });
  });
}
winrt::fire_and_forget PutPicture(std::shared_ptr<const std::string> bytes) {
  auto self = get_strong(); bool copied = false;
  if (bytes && !bytes->empty()) try {
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    winrt::Windows::Storage::Streams::DataWriter writer(stream);
    auto data = reinterpret_cast<uint8_t const*>(bytes->data());
    writer.WriteBytes(array_view<uint8_t const>(data, data + bytes->size()));
    co_await writer.StoreAsync(); writer.DetachStream(); stream.Seek(0);
    winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
    package.SetBitmap(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(stream));
    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush(); copied = true;
  } catch (...) {}
  logf("picture copied %d", copied);
  if (!copied && !exiting) { composerError.Text(L"Não foi possível copiar a imagem."); composerError.Visibility(Visibility::Visible); }
}
// "Baixar": into Downloads under the file's own name (a number added when it
// is taken), then shown selected in Explorer. The bytes come through the
// same bounded download as the pictures (the server caps a file at 32 MB).
void Download(app::ChatAttachment const& f) {
  if (!service) return;
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  // The audit saves into its own folder and opens nothing: a test never
  // writes into the owner's Downloads or puts an Explorer window up.
  auto testFolder = chatOutput.empty() ? std::filesystem::path() : chatOutput / L"baixados";
  service->ChatImage(f.url, [dispatcher, weak, name = f.name, mime = f.mime, testFolder](std::shared_ptr<const std::string> bytes) {
    std::filesystem::path saved;
    PWSTR folder{};
    if (!testFolder.empty()) { std::error_code ec; std::filesystem::create_directories(testFolder, ec); }
    if (bytes && !bytes->empty() && (!testFolder.empty() || SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &folder)))) {
      std::filesystem::path dir = testFolder.empty() ? std::filesystem::path(folder) : testFolder;
      auto target = dir / SafeFileName(net::Wide(app::ChatDownloadName(name, mime, *bytes))); auto stem = target.stem().wstring(), ext = target.extension().wstring();
      // CREATE_NEW reserves the name: two downloads of the same file side by
      // side each get their own, and nothing that was there is ever cut short.
      for (int n = 1; n <= 1000; ++n) {
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
          DWORD wrote = 0; bool ok = WriteFile(h, bytes->data(), DWORD(bytes->size()), &wrote, nullptr) && wrote == bytes->size();
          CloseHandle(h); if (ok) saved = target; else DeleteFileW(target.c_str());
          break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
        target = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
      }
    }
    if (folder) CoTaskMemFree(folder);
    dispatcher.TryEnqueue([weak, saved, testing = !testFolder.empty()] {
      auto self = weak.get(); if (!self) return;
      self->lastDownload = saved;
      if (testing) return;
      if (saved.empty()) { self->composerError.Text(L"Não foi possível baixar o arquivo."); self->composerError.Visibility(Visibility::Visible); return; }
      std::wstring args = L"/select,\"" + saved.wstring() + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    });
  });
}

// The emoji in reach: "usados aqui" first, per device like the web's
// localStorage["harmony.emoji"], one per line next to the other settings.
std::vector<std::string> recentEmoji; bool recentLoaded = false;
std::filesystem::path RecentFile() { return app::DataDirectory() / L"emoji-recentes.txt"; }
std::vector<std::string> const& Recent() {
  if (!recentLoaded) {
    recentLoaded = true; std::vector<std::string> stored;
    if (chatOutput.empty()) { std::ifstream in(RecentFile(), std::ios::binary); for (std::string l; std::getline(in, l);) if (!l.empty()) stored.push_back(l); }
    recentEmoji = app::EmojiRecent(stored);
  }
  return recentEmoji;
}
void RememberRecent(std::string const& emoji) {
  recentEmoji = app::RememberEmoji(emoji, Recent());
  if (!chatOutput.empty()) return; // the audit never writes into the owner's data
  std::error_code ec; std::filesystem::create_directories(RecentFile().parent_path(), ec);
  std::ofstream out(RecentFile(), std::ios::binary | std::ios::trunc); for (auto const& e : recentEmoji) out << e << "\n";
}
// ---------------------------------------------------------------------------
// The emoji panel (Reactions.tsx Picker): 384 x 420 on the surface, p-2 and
// gap 2; the search row 44 tall on surface-2; a rail of 32 squares that jumps
// to a group and lights the one on top; every group in one list that scrolls
// down, eight 36 cells with 4 between. A group's cells are only made when it
// comes within 400 px of the visible part, as the web's IntersectionObserver
// does: 1879 pictures at once would be the whole set fetched to open a panel.
// ---------------------------------------------------------------------------
Controls::Primitives::Popup picker{nullptr};
struct PickerSection { std::string key; std::wstring name; std::vector<std::string> list; StackPanel box{nullptr}; Grid cells{nullptr}; std::vector<bool> rowsFilled; double top = 0, height = 0; };
std::vector<PickerSection> pickerSections;
PickerSection pickerFound;
ScrollViewer pickerList{nullptr};
StackPanel pickerRail{nullptr};
std::string pickerShown;
std::function<void(std::string const&)> pickerChoose;

void OpenEmojiPanel() {
  // From the composer: left-2 over the island, 8 above it.
  auto island = composer ? FindAncestor(composer, L"ComposerIsland") : nullptr;
  OpenPicker(island, false, [this](std::string const& e) { InsertEmoji(e); });
}
void OpenReactionPanel(app::ChatMessage const& m, FrameworkElement anchor) {
  OpenPicker(anchor, true, [this, id = m.id, m](std::string const& e) {
    bool mine = false; for (auto const& r : m.reactions) if (r.emoji == e && r.Includes(ReadOwner())) mine = true;
    React(id, e, !mine);
  });
}
static FrameworkElement FindAncestor(DependencyObject from, std::wstring const& name) {
  for (auto at = from; at; at = Media::VisualTreeHelper::GetParent(at))
    if (auto f = at.try_as<FrameworkElement>(); f && f.Name() == name) return f;
  return nullptr;
}
std::string ReadOwner() { std::string me; if (!service) return me; service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; }); return me; }
void InsertEmoji(std::string const& e) {
  if (!composer) return;
  auto at = std::max(0, composer.SelectionStart()); auto text = std::wstring(composer.Text()); auto w = net::Wide(e);
  size_t place = std::min<size_t>(size_t(at), text.size());
  text.replace(place, std::min<size_t>(size_t(std::max(0, composer.SelectionLength())), text.size() - place), w);
  composer.Text(text); composer.SelectionStart(int32_t(place + w.size())); composer.SelectionLength(0);
  composer.Focus(FocusState::Programmatic);
}
// Everything the panel made is let go here, not when the App goes: a XAML
// object released after the framework has shut down takes the process with it.
void ForgetPicker() { pickerList = nullptr; pickerRail = nullptr; pickerSections.clear(); pickerFound = {}; pickerChoose = nullptr; }
void ClosePicker() { if (picker) { auto p = picker; picker = nullptr; p.IsOpen(false); p.Child(nullptr); ForgetPicker(); } }

void OpenPicker(FrameworkElement anchor, bool fromMessage, std::function<void(std::string const&)> choose) {
  ClosePicker();
  if (!anchor || !root.XamlRoot()) return;
  pickerChoose = std::move(choose);
  // Room: inside the conversation's list, 8 from its edges, as the web
  // measures against [data-history]; the composer's panel always opens up.
  // In the window's own coordinates, which is where a Popup's offsets are:
  // the page starts under the caption, so measuring from the page put the
  // panel 32 too high.
  auto rootSize = root.XamlRoot().Size();
  auto a = anchor.TransformToVisual(nullptr).TransformBounds({0, 0, float(anchor.ActualWidth()), float(anchor.ActualHeight())});
  winrt::Windows::Foundation::Rect pane{8, 8, rootSize.Width - 16, rootSize.Height - 16};
  if (messageList) pane = messageList.TransformToVisual(nullptr).TransformBounds({0, 0, float(messageList.ActualWidth()), float(messageList.ActualHeight())});
  double top = std::max(8.0, double(pane.Y)), bottom = std::min(double(rootSize.Height) - 8.0, double(pane.Y + pane.Height));
  double above = std::max(0.0, a.Y - top - 8), below = std::max(0.0, bottom - (a.Y + a.Height) - 8);
  bool up = !fromMessage || (below < 420 && above > below);
  double height = std::floor(std::min(420.0, fromMessage ? (up ? above : below) : std::max(0.0, double(a.Y) - 16)));
  double left = std::max(8.0, double(pane.X));
  double width = std::floor(std::max(0.0, std::min(384.0, fromMessage ? double(a.X + a.Width) - left - 8 : double(rootSize.Width) - 32.0)));
  if (height < 120 || width < 200) return;

  Border card; card.Width(width); card.Height(height); card.Background(Brush(Surface)); card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Padding(Thickness{8, 8, 8, 8});
  card.Name(L"EmojiPanel"); Automation::AutomationProperties::SetName(card, L"reagir");
  Grid layout; layout.RowSpacing(8);
  for (auto h : {GridLength{44, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}}) { RowDefinition r; r.Height(h); layout.RowDefinitions().Append(r); }

  // Search: magnifier 16, the box at text-item, the clear square 32 once typed.
  Border searchRow; searchRow.Background(Brush(Surface2)); searchRow.CornerRadius(CornerRadius{8, 8, 8, 8}); searchRow.Name(L"EmojiSearchContainer"); searchRow.Padding(Thickness{10,0,4,0}); searchRow.BorderThickness(Thickness{2,2,2,2}); searchRow.BorderBrush(Brush(Surface2));
  Grid sr; sr.ColumnSpacing(8);
  for (auto w : {GridLength{16, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); sr.ColumnDefinitions().Append(c); }
  auto glass = VoiceIcon("magnifier", Foreground, 16).as<FrameworkElement>(); glass.VerticalAlignment(VerticalAlignment::Center); sr.Children().Append(glass);
  TextBox query=HarmonyTextBox(); query.Name(L"EmojiSearch"); query.Style(Resources().Lookup(box_value(L"HarmonySearch")).as<Style>()); query.Height(40);
  query.Background(Tint(0,0));
  BindFieldRing(query,searchRow);
  query.PlaceholderText(L"Fogo, coracao, risada"); query.FontSize(15); query.Padding(Thickness{0}); query.MinHeight(0);
  query.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); query.BorderThickness(Thickness{0, 0, 0, 0}); query.VerticalAlignment(VerticalAlignment::Center);
  for (auto key : {L"TextControlBackgroundPointerOver", L"TextControlBackgroundFocused", L"TextControlBackground"}) query.Resources().Insert(box_value(key), Tint(0, 0));
  for (auto key : {L"TextControlBorderBrushFocused", L"TextControlBorderBrushPointerOver", L"TextControlBorderBrush"}) query.Resources().Insert(box_value(key), Tint(0, 0));
  query.Resources().Insert(box_value(L"TextControlPlaceholderForeground"), Brush(Muted)); query.Resources().Insert(box_value(L"TextControlPlaceholderForegroundFocused"), Brush(Muted));
  Automation::AutomationProperties::SetName(query, L"procurar emoji");
  // The box's own clear button goes: the row has the web's, a 32 square.

  Grid::SetColumn(query, 1); sr.Children().Append(query);
  auto clear = Square("xmark", L"limpar busca de emoji", 32, 12, Foreground, Surface3, [query] { query.Text(L""); });
  clear.Background(Brush(Default)); clear.VerticalAlignment(VerticalAlignment::Center); clear.Visibility(Visibility::Collapsed);
  Grid::SetColumn(clear, 2); sr.Children().Append(clear);
  searchRow.Child(sr); layout.Children().Append(searchRow);

  Grid body; body.ColumnSpacing(8); Grid::SetRow(body, 1);
  for (auto w : {GridLength{32, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}}) { ColumnDefinition c; c.Width(w); body.ColumnDefinitions().Append(c); }
  ScrollViewer railScroll; railScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
  pickerRail = StackPanel(); pickerRail.Spacing(4); railScroll.Content(pickerRail); ImmediateMouseWheel(railScroll); body.Children().Append(railScroll);
  pickerList = ScrollViewer(); pickerList.Name(L"EmojiList"); pickerList.Padding(Thickness{0, 0, 12, 8});
  Grid::SetColumn(pickerList, 1); body.Children().Append(pickerList);
  layout.Children().Append(body); card.Child(layout);

  // The sections: "usados aqui" when there is any, then the groups.
  pickerSections.clear();
  if (!Recent().empty()) pickerSections.push_back({"recent", L"usados aqui", Recent()});
  auto const& groups = app::EmojiGroups();
  for (size_t i = 0; i < groups.size(); ++i) {
    PickerSection s{std::to_string(i), net::Wide(groups[i].name), {}};
    for (auto const& e : groups[i].emoji) s.list.push_back(e.emoji);
    pickerSections.push_back(std::move(s));
  }
  for (auto& s : pickerSections) {
    Button tab; HarmonyPointer(tab); tab.Width(32); tab.Height(32); tab.MinWidth(0); tab.MinHeight(0); tab.Padding(Thickness{0, 0, 0, 0});
    tab.CornerRadius(CornerRadius{8, 8, 8, 8}); tab.BorderThickness(Thickness{0, 0, 0, 0}); tab.Background(Tint(0, 0)); tab.Tag(box_value(net::Wide(s.key)));
    tab.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); tab.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
    if (s.key == "recent") tab.Content(VoiceIcon("clock", Muted, 18));
    else { auto pieces = app::SplitEmoji(s.list[0]); tab.Content(EmojiPicture(s.list[0], pieces.empty() ? std::string() : pieces[0].file, 18)); }
    Automation::AutomationProperties::SetName(tab, s.name); ToolTipService::SetToolTip(tab, box_value(s.name));
    tab.Click([this, query, key = s.key](auto const&, auto const&) { query.Text(L""); JumpPicker(key); });
    pickerRail.Children().Append(tab);
  }
  FillPickerList(L"");
  query.TextChanged([this, weakClear = winrt::make_weak(clear)](IInspectable const& sender, auto const&) {
    auto query = sender.as<TextBox>(); auto clear = weakClear.get(); if (!clear) return;
    // The box's own clear button goes (it exists once there is text): the
    // row has the web's, a 32 square.
    if (auto x = FindDesignPart(query, L"DeleteButton")) { x.Width(0); x.MinWidth(0); x.Opacity(0); x.IsHitTestVisible(false); }
    clear.Visibility(query.Text().empty() ? Visibility::Collapsed : Visibility::Visible); FillPickerList(std::wstring(query.Text()));
  });
  pickerList.ViewChanged([this](auto const&, auto const&) { FollowPicker(); });
  card.KeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) {
    if (e.Key() == winrt::Windows::System::VirtualKey::Escape) { e.Handled(true); ClosePicker(); }
  });

  picker = Controls::Primitives::Popup(); picker.XamlRoot(root.XamlRoot()); picker.IsLightDismissEnabled(true); picker.Child(card);
  double x = fromMessage ? a.X + a.Width - width : a.X + 8;
  double y = up ? a.Y - 8 - height : a.Y + a.Height + 8;
  picker.HorizontalOffset(std::max(8.0, x)); picker.VerticalOffset(std::max(8.0, y));
  picker.Closed([this, back = fromMessage ? FrameworkElement(nullptr) : FrameworkElement(composer)](auto const&, auto const&) {
    if (picker) { picker.Child(nullptr); picker = nullptr; ForgetPicker(); }
    if (back) back.as<Control>().Focus(FocusState::Programmatic);
  });
  picker.IsOpen(true); query.Focus(FocusState::Programmatic);
  pickerShown = pickerSections.empty() ? "" : pickerSections[0].key; LightRail(true);
}

// One group: its heading (label, uppercase, pt-2 pb-1) over a grid whose
// height is known before its cells exist, so the scroll bar is right at once.
StackPanel PickerGroup(PickerSection& s) {
  StackPanel box; box.Tag(box_value(net::Wide(s.key)));
  auto head = Line(s.name, 12, Muted, 600); head.CharacterSpacing(60); head.LineHeight(16); head.Margin(Thickness{0, 8, 0, 4});
  std::wstring upper = s.name; for (auto& c : upper) c = wchar_t(towupper(c)); head.Text(upper);
  box.Children().Append(head);
  Grid cells; cells.ColumnSpacing(4); cells.RowSpacing(4); cells.HorizontalAlignment(HorizontalAlignment::Left);
  size_t rows = (s.list.size() + 7) / 8;
  for (int i = 0; i < 8; ++i) { ColumnDefinition c; c.Width(GridLength{36, GridUnitType::Pixel}); cells.ColumnDefinitions().Append(c); }
  for (size_t r = 0; r < rows; ++r) { RowDefinition d; d.Height(GridLength{36, GridUnitType::Pixel}); cells.RowDefinitions().Append(d); }
  box.Children().Append(cells); s.box = box; s.cells = cells; s.rowsFilled.assign(rows, false);
  // Known before layout: the heading's 8 + 16 + 4 and rows of 36 with 4 between.
  s.height = 28 + (rows ? rows * 40.0 - 4 : 0);
  return box;
}
// The rows of a group that fall between `from` and `to` (the list's own
// coordinates) get their cells; a group of 400 is never made at once, which
// held the window for a third of a second on opening.
void FillRows(PickerSection& s, double from, double to) {
  if (!s.cells) return;
  for (size_t r = 0; r < s.rowsFilled.size(); ++r) {
    double y = s.top + 28 + r * 40.0;
    if (s.rowsFilled[r] || y + 36 < from || y > to) continue;
    s.rowsFilled[r] = true;
    for (size_t i = r * 8; i < std::min(s.list.size(), r * 8 + 8); ++i) {
      auto const& e = s.list[i]; auto pieces = app::SplitEmoji(e);
      Button cell; HarmonyPointer(cell); cell.Width(36); cell.Height(36); cell.MinWidth(0); cell.MinHeight(0); cell.Padding(Thickness{0, 0, 0, 0});
      cell.CornerRadius(CornerRadius{8, 8, 8, 8}); cell.BorderThickness(Thickness{0, 0, 0, 0}); cell.Background(Tint(0, 0));
      cell.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); cell.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
      cell.Content(EmojiPicture(e, pieces.size() == 1 && pieces[0].picture ? pieces[0].file : app::EmojiFile(std::string_view(e)), 28));
      Automation::AutomationProperties::SetName(cell, net::Wide(e));
      cell.Click([this, e](auto const&, auto const&) { auto choose = pickerChoose; RememberRecent(e); ClosePicker(); if (choose) choose(e); });
      Grid::SetRow(cell, int32_t(i / 8)); Grid::SetColumn(cell, int32_t(i % 8)); s.cells.Children().Append(cell);
    }
  }
}
void FillPickerList(std::wstring const& query) {
  if (!pickerList) return;
  StackPanel content;
  bool searching = false; for (auto c : query) if (!iswspace(c)) searching = true;
  if (searching) {
    auto found = app::EmojiSearch(net::Narrow(query));
    auto& results = pickerFound; results = PickerSection{"found", L"encontrados", found};
    if (found.empty()) {
      StackPanel none; none.Padding(Thickness{0, 16, 0, 16});
      auto a = Line(L"Nenhum emoji com esse nome", 15, Foreground, 400); a.TextWrapping(TextWrapping::Wrap); none.Children().Append(a);
      auto b = Line(L"Tente uma palavra só, como abacaxi ou pizza.", 13, Muted, 400); b.TextWrapping(TextWrapping::Wrap); none.Children().Append(b);
      content.Children().Append(none);
    } else { content.Children().Append(PickerGroup(results)); results.top = 0; FillRows(results, 0, 600); }
  } else {
    double at = 0; pickerFound = {};
    for (auto& s : pickerSections) { content.Children().Append(PickerGroup(s)); s.top = at; at += s.height; }
  }
  pickerList.Content(content); ImmediateMouseWheel(pickerList); pickerList.ChangeView(nullptr, 0.0, nullptr, true);
  LightRail(!searching);
  FollowPicker();
}
// The groups within 400 px of what shows get their cells, and the one at
// the top lights on the rail.
void FollowPicker() {
  if (!pickerList) return;
  double offset = pickerList.VerticalOffset(), view = pickerList.ViewportHeight();
  if (view <= 0) view = 420; // not laid out yet: the panel's own height
  if (pickerFound.box) { FillRows(pickerFound, offset - 200, offset + view + 200); return; }
  std::string top = pickerSections.empty() ? "" : pickerSections[0].key;
  for (auto& s : pickerSections) {
    if (!s.box) continue;
    if (s.top + s.height >= offset - 200 && s.top <= offset + view + 200) FillRows(s, offset - 200, offset + view + 200);
    if (s.top <= offset + 8) top = s.key;
  }
  if (top != pickerShown) { pickerShown = top; LightRail(true); }
}
void JumpPicker(std::string const& key) {
  if (!pickerList) return;
  for (auto& s : pickerSections) if (s.key == key && s.box) { FillRows(s, s.top, s.top + 620); pickerList.ChangeView(nullptr, s.top, nullptr, true); }
  pickerShown = key; LightRail(true);
}
void LightRail(bool on) {
  if (!pickerRail) return;
  for (auto child : pickerRail.Children()) {
    auto tab = child.as<Button>(); bool lit = on && net::Narrow(std::wstring(unbox_value<hstring>(tab.Tag()))) == pickerShown;
    tab.Background(lit ? Brush(Default) : Tint(0, 0));
  }
}
// ---------------------------------------------------------------------------
// @ completion (Composer.tsx pick/matches/complete): looking back from the
// caret for an @ still being typed, the people of the server and "@todos",
// six at most, in a 348 panel 8 above the composer's block. Arrows choose,
// Enter or Tab complete, Esc closes; the box keeps the focus throughout.
// ---------------------------------------------------------------------------
Controls::Primitives::Popup mentionPopup{nullptr};
std::vector<app::ChatPerson> mentionMatches;
size_t mentionHighlight = 0;
std::optional<app::ChatMentionToken> mentionToken;

void UpdateMentions() {
  if (!composer || !service) return;
  auto wide = std::wstring(composer.Text());
  auto caret = std::min<size_t>(size_t(std::max(0, composer.SelectionStart())), wide.size());
  auto before = net::Narrow(wide.substr(0, caret));
  auto token = app::ChatMentionAt(before, before.size());
  if (!token) { CloseMentions(); return; }
  std::vector<app::ChatPerson> members;
  service->ChatModel().Inspect([&](app::ChatState const& s) { members = s.members; });
  auto matches = app::ChatAutocomplete(members, token->username);
  bool same = mentionPopup && mentionToken && mentionToken->username == token->username && mentionMatches == matches;
  mentionToken = token;
  if (same) return;
  mentionMatches = std::move(matches); mentionHighlight = 0;
  DrawMentions();
}
void CloseMentions() {
  mentionToken.reset(); mentionMatches.clear(); mentionHighlight = 0;
  if (mentionPopup) { auto p = mentionPopup; mentionPopup = nullptr; p.IsOpen(false); p.Child(nullptr); }
}
void DrawMentions() {
  auto area = composer ? FindAncestor(composer, L"ComposerArea") : nullptr;
  if (!area || !root.XamlRoot()) return;
  Border card; card.Name(L"MentionPanel"); card.Width(348); card.Background(Brush(Surface)); card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Padding(Thickness{8, 8, 8, 8});
  StackPanel list;
  auto head = Line(L"PESSOAS NESTA CONVERSA", 12, Muted, 600); head.CharacterSpacing(60); head.LineHeight(16); head.Margin(Thickness{8, 0, 8, 8});
  list.Children().Append(head);
  if (mentionMatches.empty()) {
    auto none = Line(L"Ninguém com esse nome.", 15, Muted, 400); none.Margin(Thickness{8, 12, 8, 12}); list.Children().Append(none);
  }
  for (size_t i = 0; i < mentionMatches.size(); ++i) {
    auto const& m = mentionMatches[i]; bool lit = i == mentionHighlight;
    Grid option; option.Height(44);
    Border pill; pill.CornerRadius(CornerRadius{8, 8, 8, 8}); pill.Background(lit ? Brush(Default) : Tint(0, 0)); pill.Padding(Thickness{8, 0, 8, 0});
    Grid g; g.ColumnSpacing(12);
    for (auto w : {GridLength{28, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); g.ColumnDefinitions().Append(c); }
    // The web's two letters of the display name here (or "@" for @todos).
    std::wstring letters = m.username == "todos" ? L"@" : net::Wide(m.displayName.empty() ? m.username : m.displayName).substr(0, 2);
    for (auto& c : letters) c = wchar_t(towupper(c));
    Border face; face.Width(28); face.Height(28); face.CornerRadius(CornerRadius{14, 14, 14, 14}); face.Background(Brush(Surface3)); face.VerticalAlignment(VerticalAlignment::Center);
    auto initials = Line(letters, 13, Foreground, 400); initials.HorizontalAlignment(HorizontalAlignment::Center); initials.VerticalAlignment(VerticalAlignment::Center); face.Child(initials);
    g.Children().Append(face);
    auto name = Line(net::Wide(m.displayName.empty() ? m.username : m.displayName), 15, Foreground, 400); name.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(name, 1); g.Children().Append(name);
    auto handle = Line(L"@" + net::Wide(m.username), 13, Muted, 400); handle.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(handle, 2); g.Children().Append(handle);
    pill.Child(g); option.Children().Append(pill);
    if (lit) {
      // The 3 px accent bar in the panel's padding, at -8 (style.css mention-option.is-selected).
      Border bar; bar.Width(3); bar.Background(Brush(Accent)); bar.HorizontalAlignment(HorizontalAlignment::Left); bar.Margin(Thickness{-8, 0, 0, 0}); bar.CornerRadius(CornerRadius{0,2,2,0});
      option.Children().Append(bar);
    }
    Automation::AutomationProperties::SetName(option, net::Wide(m.displayName) + L" @" + net::Wide(m.username));
    option.PointerEntered([](IInspectable const& s, auto const&) { auto b = s.as<Grid>().Children().GetAt(0).as<Border>(); b.Background(Brush(Default)); });
    option.PointerExited([this, i](IInspectable const& s, auto const&) { if (i != mentionHighlight) s.as<Grid>().Children().GetAt(0).as<Border>().Background(Tint(0, 0)); });
    option.PointerPressed([this, user = m.username](auto const&, Input::PointerRoutedEventArgs const& e) { e.Handled(true); CompleteMention(user); });
    list.Children().Append(option);
  }
  card.Child(list);
  card.Measure({348, 1e6}); auto height = card.DesiredSize().Height;
  auto a = area.TransformToVisual(nullptr).TransformPoint({0, 0});
  if (!mentionPopup) {
    mentionPopup = Controls::Primitives::Popup(); mentionPopup.XamlRoot(root.XamlRoot()); mentionPopup.IsLightDismissEnabled(false);
  }
  mentionPopup.Child(card);
  mentionPopup.HorizontalOffset(a.X + 8); mentionPopup.VerticalOffset(std::max(8.0, double(a.Y) - 8 - height));
  mentionPopup.IsOpen(true);
}
void CompleteMention(std::string const& username) {
  if (!mentionToken || !composer) return;
  auto wide = std::wstring(composer.Text());
  auto caret = std::min<size_t>(size_t(std::max(0, composer.SelectionStart())), wide.size());
  auto before = net::Narrow(wide.substr(0, caret));
  auto at = net::Wide(before.substr(0, std::min(mentionToken->at, before.size()))).size();
  auto insert = L"@" + net::Wide(username) + L" ";
  wide.replace(at, caret - at, insert);
  CloseMentions();
  composer.Text(wide); composer.SelectionStart(int32_t(at + insert.size())); composer.SelectionLength(0);
  composer.Focus(FocusState::Programmatic);
}
// The keys the open list takes before the box sees them (PreviewKeyDown:
// Tab would otherwise move the focus away, and Up would edit a message).
bool MentionKey(winrt::Windows::System::VirtualKey key) {
  using winrt::Windows::System::VirtualKey;
  if (!mentionPopup) return false;
  if (key == VirtualKey::Escape) { CloseMentions(); return true; }
  if (mentionMatches.empty()) return false;
  if (key == VirtualKey::Down) { mentionHighlight = (mentionHighlight + 1) % mentionMatches.size(); DrawMentions(); return true; }
  if (key == VirtualKey::Up) { mentionHighlight = (mentionHighlight + mentionMatches.size() - 1) % mentionMatches.size(); DrawMentions(); return true; }
  if (key == VirtualKey::Enter || key == VirtualKey::Tab) { CompleteMention(mentionMatches[mentionHighlight].username); return true; }
  return false;
}

// ---------------------------------------------------------------------------
// Files (Composer.tsx attach/put/forget): a file goes up the moment it is
// chosen or dropped, before the message exists, and the composer keeps the id
// the server gave back. Four at most, the server's own rule; too big is said
// here and not 32 MB up the wire. The x cancels for real while it climbs.
// ---------------------------------------------------------------------------
struct PendingFile {
  int key = 0; std::wstring name; double sent = 0;
  std::optional<app::ChatAttachment> stored;
  std::shared_ptr<std::atomic<bool>> cancel;
};
std::vector<PendingFile> pendingFiles; int pendingSeq = 0;
std::set<int> sendingFiles;          // the keys that left with the message on its way
StackPanel uploadRow{nullptr}; bool dropHooked = false;
std::wstring uploadError;
static constexpr size_t MaxFiles = 4;
static constexpr uintmax_t MaxUploadBytes = 32u << 20;

bool Climbing() { for (auto const& f : pendingFiles) if (!f.stored) return true; return false; }
size_t ReadyFiles() { size_t n = 0; for (auto const& f : pendingFiles) if (f.stored) ++n; return n; }
void ShowComposerError(std::wstring const& failure) {
  auto text = !uploadError.empty() ? uploadError : failure;
  composerError.Text(text); composerError.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void PickAttachment() {
  if(chatActive.empty()) return;
  PickFiles(L"Mandar arquivo",false,[this](auto files) {if(!exiting) Attach(files);});
}
void Attach(std::vector<std::filesystem::path> const& chosen) {
  if (chosen.empty() || chatActive.empty() || !service) return;
  size_t room = pendingFiles.size() >= MaxFiles ? 0 : MaxFiles - pendingFiles.size();
  uploadError.clear();
  // All of them start at once; four bars that all move is the honest picture.
  for (size_t i = 0; i < chosen.size() && i < room; ++i) Put(chosen[i]);
  if (chosen.size() > room) uploadError = L"Uma mensagem leva 4 arquivos, não mais.";
  ShowComposerError(L""); DrawUploads(); UpdateSend();
}
void Put(std::filesystem::path const& file) {
  std::error_code ec; auto size = std::filesystem::file_size(file, ec);
  auto name = file.filename().wstring();
  if (ec) { uploadError = name + L": não foi possível ler o arquivo."; return; }
  if (size > MaxUploadBytes) { uploadError = name + L": O arquivo é grande demais (o limite é 32 MB)."; return; }
  PendingFile f; f.key = ++pendingSeq; f.name = name; f.cancel = std::make_shared<std::atomic<bool>>(false);
  pendingFiles.push_back(f);
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak(); int key = f.key;
  // Progress arrives per 64 KB written; the screen hears about whole percents.
  auto shown = std::make_shared<std::atomic<int>>(-1);
  service->ChatUpload(chatActive, file, [dispatcher, weak, key, shown](uint64_t sent, uint64_t total) {
    int percent = total ? int(sent * 100 / total) : 0;
    if (shown->exchange(percent) == percent) return;
    dispatcher.TryEnqueue([weak, key, sent, total] {
      auto self = weak.get(); if (!self || self->exiting) return;
      for (auto& p : self->pendingFiles) if (p.key == key && !p.stored) { p.sent = total ? double(sent) / double(total) : 0; self->DrawUploads(); }
    });
  }, f.cancel, [dispatcher, weak, key, name](net::Response r) {
    dispatcher.TryEnqueue([weak, key, name, r] {
      auto self = weak.get(); if (!self || self->exiting) return;
      auto at = std::find_if(self->pendingFiles.begin(), self->pendingFiles.end(), [&](auto const& p) { return p.key == key; });
      if (at == self->pendingFiles.end()) return; // taken off meanwhile
      auto v = r.Json();
      if (r.status >= 200 && r.status < 300 && v["id"].IsString()) {
        app::ChatAttachment a; a.id = v["id"].Str(); a.name = v["name"].Str(); a.mime = v["mime"].Str(); a.url = v["url"].Str();
        a.size = v["size"].Int(); a.width = v["width"].Int(); a.height = v["height"].Int();
        at->sent = 1; at->stored = a;
      } else {
        // Changing your mind is not a failure to report: the tile simply goes.
        if (v["error"].Str() != "canceled") self->uploadError = name + L": " + net::Wide(app::ChatError(r));
        self->pendingFiles.erase(at); self->ShowComposerError(L"");
      }
      self->DrawUploads(); self->UpdateSend();
    });
  });
}
void Forget(int key) {
  for (auto it = pendingFiles.begin(); it != pendingFiles.end(); ++it) if (it->key == key) { it->cancel->store(true); pendingFiles.erase(it); break; }
  DrawUploads(); UpdateSend();
}
void ForgetAllUploads() {
  for (auto& f : pendingFiles) f.cancel->store(true);
  pendingFiles.clear(); sendingFiles.clear(); uploadError.clear(); DrawUploads();
}
void OpenPendingImage(app::ChatAttachment const& file) {
  app::ChatMessage message; OpenLightbox(file, message);
}
// The tiles over the box (mb-2, gap 2): a stored picture is itself at 64; the
// rest a 160 x 64 card with the name over a 4 px bar and the percent, then
// "Guardando..." while the server reads the bytes, then "Pronto para enviar".
void DrawUploads() {
  if (!uploadRow) return;
  uploadRow.Children().Clear();
  uploadRow.Visibility(pendingFiles.empty() ? Visibility::Collapsed : Visibility::Visible);
  for (auto const& f : pendingFiles) {
    Grid tile; tile.Margin(Thickness{0, 0, 8, 0}); Automation::AutomationProperties::SetName(tile, f.name);
    if (f.stored && f.stored->mime.rfind("image/", 0) == 0) {
      Border pic; pic.Width(64); pic.Height(64); pic.CornerRadius(CornerRadius{8, 8, 8, 8}); pic.Background(Brush(Surface));
      pic.Name(L"PendingImage"+std::to_wstring(f.key));
      Image image; image.Stretch(Stretch::UniformToFill); pic.Child(image);
      LoadPicture(f.stored->url, false, [image](ImageSource s) { if (s) image.Source(s); });
      HarmonyPointer(pic); pic.Tapped([this,file=*f.stored](auto const&,Input::TappedRoutedEventArgs const& e) {e.Handled(true); OpenPendingImage(file);});
      tile.Children().Append(pic);
    } else {
      Border card; card.Width(160); card.Height(64); card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Background(Brush(Surface)); card.Padding(Thickness{12, 0, 12, 0});
      StackPanel words; words.Spacing(4); words.VerticalAlignment(VerticalAlignment::Center);
      words.Children().Append(Line(f.name, 13, Foreground, 400));
      if (f.stored) words.Children().Append(Line(L"Pronto para enviar", 13, Muted, 400));
      else {
        ProgressBar bar; bar.Minimum(0); bar.Maximum(100); bar.Value(std::round(f.sent * 100)); bar.Height(4); bar.MinHeight(4);
        bar.Foreground(Brush(Accent)); bar.Background(Brush(Surface2)); bar.CornerRadius(CornerRadius{2, 2, 2, 2});
        Automation::AutomationProperties::SetName(bar, L"enviando " + f.name);
        words.Children().Append(bar);
        words.Children().Append(Line(f.sent < 1 ? std::to_wstring(int(std::round(f.sent * 100))) + L"%" : L"Guardando…", 13, Muted, 400));
      }
      card.Child(words); ToolTipService::SetToolTip(card, box_value(f.name)); tile.Children().Append(card);
    }
    auto x = Square("xmark", f.stored ? L"tirar" : L"cancelar", 24, 12, DangerSoftFg, 0xe15451, [this, key = f.key] { Forget(key); });
    NegativeButton(x); x.HorizontalAlignment(HorizontalAlignment::Right); x.VerticalAlignment(VerticalAlignment::Top); x.Margin(Thickness{0, -6, -6, 0});
    ToolTipService::SetToolTip(x, box_value(f.stored ? L"Tirar" : L"Cancelar"));
    tile.Children().Append(x);
    uploadRow.Children().Append(tile);
  }
}
// Ctrl+V of a picture (Composer.tsx onPaste): a screenshot or a copied image
// becomes a file like any other, imagem.png as the browser names it. The
// clipboard's bitmap is re-encoded as PNG because the server takes pictures
// by their bytes and a DIB is not one of them.
void HookPaste() {
  composer.Paste([this](auto const&, TextControlPasteEventArgs const& e) {
    using namespace winrt::Windows::ApplicationModel::DataTransfer;
    auto content = Clipboard::GetContent();
    if (!content.Contains(StandardDataFormats::Bitmap()) && !content.Contains(StandardDataFormats::StorageItems())) return;
    if (content.Contains(StandardDataFormats::Text())) return; // text wins, as a browser does
    e.Handled(true); PastePicture(content);
  });
}
winrt::fire_and_forget PastePicture(winrt::Windows::ApplicationModel::DataTransfer::DataPackageView content) {
  using namespace winrt::Windows::ApplicationModel::DataTransfer;
  using namespace winrt::Windows::Graphics::Imaging;
  auto lifetime = get_strong(); winrt::apartment_context ui;
  std::vector<std::filesystem::path> paths;
  try {
    if (content.Contains(StandardDataFormats::StorageItems())) {
      for (auto const& item : co_await content.GetStorageItemsAsync()) {
        auto path = std::filesystem::path(std::wstring(item.Path())); auto ext = path.extension().wstring();
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File) && (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" || ext == L".webp")) paths.push_back(path);
      }
    } else {
      auto stream = co_await (co_await content.GetBitmapAsync()).OpenReadAsync();
      auto decoder = co_await BitmapDecoder::CreateAsync(stream);
      auto bitmap = co_await decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied);
      // A folder of its own per paste, under %TEMP%, emptied once nothing is climbing.
      auto base = std::filesystem::temp_directory_path() / L"harmony-colar"; std::error_code ec;
      if (!Climbing()) std::filesystem::remove_all(base, ec);
      auto dir = base / std::to_wstring(GetTickCount64()); std::filesystem::create_directories(dir, ec);
      auto folder = co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(dir.wstring());
      auto file = co_await folder.CreateFileAsync(L"imagem.png", winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);
      {
        auto out = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite);
        auto encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), out);
        encoder.SetSoftwareBitmap(bitmap); co_await encoder.FlushAsync();
      }
      paths.push_back(dir / L"imagem.png");
    }
  } catch (...) { logf("pasted picture could not be read"); }
  co_await ui;
  if (!exiting) Attach(paths);
}
// A file dropped anywhere on the window lands in the box of the conversation
// on screen, as the web's window-wide drop does.
void HookDrop(UIElement target) {
  target.AllowDrop(true);
  target.DragOver([this](auto const&, DragEventArgs const& e) {
    if (!chatActive.empty() && e.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())) {
      e.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
      e.DragUIOverride().Caption(L"Mandar para a conversa");
    }
  });
  target.Drop([this](auto const&, DragEventArgs const& e) -> winrt::fire_and_forget {
    auto lifetime = get_strong(); auto view = e.DataView();
    if (!view.Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())) co_return;
    auto deferral = e.GetDeferral();
    std::vector<std::filesystem::path> paths;
    try {
      for (auto const& item : co_await view.GetStorageItemsAsync())
        if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File)) paths.emplace_back(std::wstring(item.Path()));
    } catch (...) {}
    deferral.Complete();
    if (!exiting) Attach(paths);
  });
}

// ---------------------------------------------------------------------------
// People: the island on the right, "PESSOAS · N", then online and offline.
// ---------------------------------------------------------------------------
Grid peopleIsland{nullptr};
Grid PeopleColumn() {
  Grid column;peopleIsland=column;
  RowDefinition list;list.Height(GridLength{1,GridUnitType::Star});column.RowDefinitions().Append(list);
  RowDefinition version;version.Height(GridLength{1,GridUnitType::Auto});column.RowDefinitions().Append(version);
  auto island = Island(); island.Name(L"PeopleIsland"); island.Margin(Thickness{0, 8, 8, 8}); island.Padding(Thickness{8, 8, 8, 8});
  // The lane only when the list scrolls, as the channel list: a permanent
  // one left the person's highlight 16 short on the right (the owner, 25/10).
  peopleList = StackPanel(); peopleSignature.clear(); peopleRows.clear(); peopleMetadata.clear(); ScrollViewer scroll; scroll.Content(peopleList); ImmediateMouseWheel(scroll); ScrollLaneWhenNeeded(scroll); island.Child(scroll);
  column.Children().Append(island);auto update=VersionIsland();Grid::SetRow(update,1);column.Children().Append(update);return column;
}
struct PeopleRow { Border band{nullptr}; Grid face{nullptr}; Shapes::Ellipse dot{nullptr}; TextBlock name{nullptr}; FrameworkElement call{nullptr}; std::string status; MenuFlyout menu{nullptr}; };
std::map<std::string,PeopleRow> peopleRows;
std::string peopleMetadata;
void DrawPeople(app::State const& state) {
  std::vector<app::ChatPerson> members; std::vector<app::ChatRole> roles; uint64_t rolesVersion=0;
  service->ChatModel().Inspect([&](app::ChatState const& s) {members=s.members;roles=s.roles;rolesVersion=s.rolesVersion;});
  DrawPeopleSnapshot(state,members,roles,rolesVersion);
}
void DrawPeopleSnapshot(app::State const& state, std::vector<app::ChatPerson> const& members, std::vector<app::ChatRole> const& roles, uint64_t rolesVersion) {
  std::string signature = "roles" + std::to_string(rolesVersion) + "|";
  for (auto const& p : members) signature += p.username + "/" + p.avatar + "/" + net::Narrow(PersonName(p)) + "|";
  auto metadata=signature;
  for (auto const& p : members) signature += state.model.StateOf(p.username) + "|";
  if (signature == peopleSignature) return;
  peopleSignature = signature;
  if (metadata!=peopleMetadata) { peopleRows.clear(); peopleMetadata=metadata; }
  std::vector<UIElement> desired;
  auto label = [&](std::wstring const& t, double top) {
    Border box; box.Padding(Thickness{8, top, 8, top == 8 ? 4.0 : 2.0}); auto text = Line(t, 13, Muted, 600); text.CharacterSpacing(60); box.Child(text); desired.push_back(box);
  };
  label(L"PESSOAS \u00b7 " + std::to_wstring(members.size()), 8);
  // A role marked "mostrar separado" (D12c) gets a group of its own above
  // Online, for whoever has it and is not offline, in the group of their
  // highest such role (App.tsx); offline is offline whatever the role.
  struct Group { std::wstring label; std::vector<app::ChatPerson> people; };
  std::vector<Group> groups; std::vector<std::string> apart;
  for (auto const& r : roles) if (r.hoist) { apart.push_back(r.id); auto upper = net::Wide(r.name); CharUpperBuffW(upper.data(), DWORD(upper.size())); groups.push_back({upper, {}}); }
  for (auto word : {L"ONLINE", L"AUSENTE", L"OFFLINE"}) groups.push_back({word, {}});
  for (auto const& p : members) {
    auto status = state.model.StateOf(p.username);
    auto hoisted = status == "offline" ? std::nullopt : app::ChatHoistOf(p.username, roles);
    size_t at = hoisted ? size_t(std::find(apart.begin(), apart.end(), hoisted->id) - apart.begin())
                        : apart.size() + (status == "away" ? 1 : status == "offline" ? 2 : 0);
    groups[at].people.push_back(p);
  }
  for (auto const& group : groups) {
    auto const& people = group.people;
    if (people.empty()) continue;
    label(group.label + L" \u00b7 " + std::to_wstring(people.size()), 16);
    for (auto const& p : people) {
      auto status = state.model.StateOf(p.username); bool here = status != "offline";
      if (auto found=peopleRows.find(p.username);found!=peopleRows.end()) {
        auto& cached=found->second;
        if(cached.status!=status) {
          if((cached.status!="offline")!=here) cached.face.Children().SetAt(0,PhotoFace(p,32,here,true));
          cached.dot.Fill(here ? Brush(status=="away" ? 0xf5a524 : Success) : Tint(Muted,.4));
          cached.name.Foreground(Brush(app::ChatColorValue(app::ChatColorOf(p.username,roles)).value_or(here?Foreground:Muted)));
          cached.call.Visibility(status=="in_call"?Visibility::Visible:Visibility::Collapsed);
          ToolTipService::SetToolTip(cached.band,box_value(status=="in_call"?L"Em chamada":status=="away"?L"Ausente":here?L"Online":L"Offline"));
          auto menu=PersonMenu(p);cached.menu.Items().Clear();
          while(menu.Items().Size()) {auto item=menu.Items().GetAt(0);menu.Items().RemoveAt(0);cached.menu.Items().Append(item);}
          cached.status=status;
        }
        desired.push_back(cached.band); continue;
      }
      Grid row; row.Name(L"PersonRow-"+net::Wide(p.username)); row.Height(48); row.Padding(Thickness{8,0,8,0}); row.ColumnSpacing(6);
      row.Margin(Thickness{0,2,0,0});
      for (auto w : {GridLength{32, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c); }
      Grid face; face.Name(L"PersonAvatar-"+net::Wide(p.username)); face.Width(32); face.Height(32); face.VerticalAlignment(VerticalAlignment::Center);
      face.Children().Append(PhotoFace(p, 32, here, true));
      Shapes::Ellipse dot; dot.Width(10); dot.Height(10);
      dot.Fill(here ? Brush(status == "away" ? 0xf5a524 : Success) : Tint(Muted,.4));
      dot.HorizontalAlignment(HorizontalAlignment::Left); dot.VerticalAlignment(VerticalAlignment::Top);
      TranslateTransform position; position.X(22.328); position.Y(22.328); dot.RenderTransform(position); face.Children().Append(dot);
      row.Children().Append(face);
      auto color = app::ChatColorValue(app::ChatColorOf(p.username, roles));
      auto name = Line(PersonName(p), 16, color.value_or(here ? Foreground : Muted), 500); name.Name(L"PersonName-"+net::Wide(p.username)); name.CharacterSpacing(-15); name.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(name, 1); row.Children().Append(name);
      auto mark = VoiceIcon("handset",Success,14).as<FrameworkElement>(); mark.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(mark,2); mark.Visibility(status=="in_call"?Visibility::Visible:Visibility::Collapsed); row.Children().Append(mark);
      Border band; band.CornerRadius(CornerRadius{8,8,8,8}); band.Child(row);
      band.PointerEntered([](auto const& eventSender, auto const&) { auto band = eventSender.template as<Border>(); band.Background(Brush(Surface2)); });
      band.PointerExited([](auto const& eventSender, auto const&) { auto band = eventSender.template as<Border>(); band.Background(Tint(0,0)); });
      band.Name(net::Wide("Person-" + p.username));
      HarmonyPointer(band);band.Tapped([this,p](auto const&,Input::TappedRoutedEventArgs const& e){e.Handled(true);OpenProfile(p);});
      auto menu=PersonMenu(p);MenuAtPointer(band,menu);
      ToolTipService::SetToolTip(band, box_value(status == "in_call" ? L"Em chamada" : status == "away" ? L"Ausente" : here ? L"Online" : L"Offline"));
      peopleRows.emplace(p.username,PeopleRow{band,face,dot,name,mark,status,menu});
      desired.push_back(band);
    }
  }
  // Move existing rows between presence groups without discarding their visuals.
  auto children=peopleList.Children();
  for(uint32_t i=0;i<desired.size();++i) {
    if(i<children.Size() && children.GetAt(i)==desired[i]) continue;
    uint32_t previous;
    if(children.IndexOf(desired[i],previous)) children.RemoveAt(previous);
    children.InsertAt(i,desired[i]);
  }
  while(children.Size()>desired.size()) children.RemoveAtEnd();
}

#ifndef HARMONY_PUBLISH
void CheckPeoplePresence() {
  service=std::make_unique<app::Service>("http://127.0.0.1:9",std::function<void()>{},false);
  peopleList=StackPanel(); peopleSignature.clear(); peopleMetadata.clear(); peopleRows.clear();
  app::State state; state.model.presence={{"fixture-a","online"},{"fixture-b","online"}};
  std::vector<app::ChatPerson> people{{"fixture-a","Fixture A"},{"fixture-b","Fixture B"}};
  DrawPeopleSnapshot(state,people,{},0);
  auto first=peopleRows.at("fixture-a").band, other=peopleRows.at("fixture-b").band;
  for(auto status:{"away","in_call","offline","online"}) {
    state.model.presence["fixture-a"]=status; DrawPeopleSnapshot(state,people,{},0);
    check_bool(peopleRows.at("fixture-a").band==first && peopleRows.at("fixture-b").band==other);
    check_bool(peopleRows.at("fixture-a").status==status);
    check_bool(peopleRows.at("fixture-a").call.Visibility()==(std::string(status)=="in_call"?Visibility::Visible:Visibility::Collapsed));
    uint32_t at;check_bool(peopleList.Children().IndexOf(first,at));
  }
  peopleList=nullptr;peopleRows.clear();peopleSignature.clear();peopleMetadata.clear();service.reset();
}
#endif

std::string modelActive;   // the chat model's open conversation, last seen
bool followDirect = false; // OpenDirect: the model picks the conversation
void OpenDirect(std::string const& who) {
  chatTab = "dm"; channelsSignature.clear(); followDirect = true;
  service->ChatWork([who](app::Chat& c) { c.OpenDM(who); });
}

void DrawAccount(app::State const& state) {
  std::string me; std::wstring name; app::ChatPerson self;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    me = s.owner; for (auto const& p : s.members) if (p.username == me) { name = PersonName(p); self = p; }
  });
  if (name.empty()) { name = net::Wide(me); self.username = me; }
  if (accountName.Text() == name && accountPhoto == self.avatar) return;
  accountName.Text(name); accountPhoto = self.avatar;
  accountFace.Children().Clear(); accountFace.Children().Append(PhotoFace(self, 32, true, true));
  Border dot; dot.Width(10); dot.Height(10); dot.CornerRadius(CornerRadius{5, 5, 5, 5}); dot.Background(Brush(Success)); dot.HorizontalAlignment(HorizontalAlignment::Left); dot.VerticalAlignment(VerticalAlignment::Top); TranslateTransform position; position.X(22.328); position.Y(22.328); dot.RenderTransform(position); accountFace.Children().Append(dot);
  (void)state;
}

// Called from Render() whenever the service or the chat says something changed.
void DrawChat(app::State const& state) {
  DrawChannels(state); DrawPeople(state); DrawAccount(state); RefreshRoles(); RefreshPersonalUI();
  // The chat may have opened a DM for us (OpenDM): follow it, and only that.
  // Following any difference sent a pick back: the model takes the new
  // conversation only when its worker gets to it, and a redraw in between
  // (a call redraws for every word) reopened the old one, then the new, on
  // and on (Fulano, 24/10/2026; the chat audit's busy-worker switches).
  std::string active;
  service->ChatModel().Inspect([&](app::ChatState const& s) { active = s.active; });
  if (followDirect && !active.empty() && active != modelActive) {
    followDirect = false;
    if (active != chatActive) OpenConversation(active);
  }
  modelActive = active;
  if (!chatActive.empty() && headerTitle.Text().empty()) RefreshHeader();
  DrawMessages();
  std::string metadataError;
  service->ChatModel().Inspect([&](app::ChatState const& s) { metadataError = s.error; });
  if (!metadataError.empty()) ShowComposerError(net::Wide(metadataError));
}

#ifndef HARMONY_PUBLISH
// ---------------------------------------------------------------------------
// --chat-audit <dir>: the W5 photo-and-measure run, the chat's counterpart of
// --design-audit. It signs in with HARMONY_TEST_USER/PASSWORD on a LOCAL
// server only, opens the first channel, and saves the picture and the laid-
// out tree (x, y, size, font, colour of every part) for the comparison with
// the web's medidas-web.md. Then it writes one message through the box, as a
// person would, and saves again.
// ---------------------------------------------------------------------------
// A run asked to stop early (HARMONY_AUDIT_COLUMN) ends here, passed.
struct AuditStop {};
// Position without activation or a z-order change; prefer the second monitor.
void ChatAuditPosition() {
  MONITORINFO monitor{sizeof(MONITORINFO)};
  ParityRequire(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
    MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(m, &info) || (info.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
    *reinterpret_cast<MONITORINFO*>(data) = info; return FALSE;
  }, reinterpret_cast<LPARAM>(&monitor));
  window.AppWindow().Move({monitor.rcWork.left + 32, monitor.rcWork.top + 32});
}
// Only optional physical-delivery stages may activate or move the pointer.
winrt::Windows::Foundation::IAsyncAction ChatAuditForeground(char const* label,std::source_location caller=std::source_location::current()) {
  AuditRequireRealPointer(caller);
  auto lifetime = get_strong(); winrt::apartment_context ui;
  struct Restore {
    HWND window; POINT cursor{};
    Restore(HWND w) : window(w) { GetCursorPos(&cursor); }
    ~Restore() {
      SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      AuditSetCursorPos(cursor.x, cursor.y);
    }
  } restore(hwnd);
  window.Activate();
  MONITORINFO monitor{sizeof(MONITORINFO)};
  ParityRequire(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor));
  // Tests run on the second monitor (the owner's rule): the first is theirs,
  // and a system prompt dims all of it (25/10/2026: a security prompt's
  // Shell_SystemDim took every click of the audit).
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
    MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(m, &info) || (info.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
    *reinterpret_cast<MONITORINFO*>(data) = info; return FALSE;
  }, reinterpret_cast<LPARAM>(&monitor));
  // Repeated audit launches cascade on Windows and can leave the composer
  // below the monitor. Put the whole fixture back inside its work area.
  window.AppWindow().Move({monitor.rcWork.left+32,monitor.rcWork.top+32});
  ParityRequire(SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
  // ConversationColumn creates this Border with only a mark and a TextBlock,
  // no input handlers or buttons. Its right padding is empty (8 DIP); click
  // 4 DIP from that edge, vertically centred. The root's PointerPressed only
  // records lastActivity; it performs no command (main.cpp).
  auto header = FindDesignPart(root, L"HeaderIsland");
  ParityRequire(header && header.ActualWidth() > 8 && header.ActualHeight() == 48);
  auto at = header.TransformToVisual(nullptr).TransformPoint({float(header.ActualWidth() - 4), 24});
  double scale = GetDpiForWindow(hwnd) / 96.0;
  POINT p{LONG(at.X * scale), LONG(at.Y * scale)}; ParityRequire(ClientToScreen(hwnd, &p));
  if (auto top = GetAncestor(WindowFromPoint(p), GA_ROOT); top != hwnd) {
    // Name what sits on the point (class and executable only, no title).
    wchar_t cls[128]{}; GetClassNameW(top, cls, 128); DWORD pid = 0; GetWindowThreadProcessId(top, &pid);
    wchar_t exe[MAX_PATH]{}; DWORD size = MAX_PATH;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { QueryFullProcessImageNameW(h, 0, exe, &size); CloseHandle(h); }
    RECT r{}; GetWindowRect(top, &r);
    logf("%s: point %ld,%ld is under pid %lu %s [%s] %ld,%ld-%ld,%ld", label, p.x, p.y, pid, net::Narrow(std::filesystem::path(exe).filename().wstring()).c_str(), net::Narrow(cls).c_str(), r.left, r.top, r.right, r.bottom);
  }
  ParityRequire(GetAncestor(WindowFromPoint(p), GA_ROOT) == hwnd);
  INPUT input[3]{}; for (auto& in : input) in.type = INPUT_MOUSE;
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  input[0].mi.dx = LONG((p.x - vx) * 65535.0 / (vw - 1)); input[0].mi.dy = LONG((p.y - vy) * 65535.0 / (vh - 1));
  input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  ParityRequire(AuditSendInput(3, input, sizeof(INPUT)) == 3);
  co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
  logf("%s activation: neutral header click at %ld,%ld, foreground %d", label, p.x, p.y, GetForegroundWindow() == hwnd);
  ParityRequire(GetForegroundWindow() == hwnd);
}

winrt::Windows::Foundation::IAsyncAction ChatAudit() {
  auto lifetime = get_strong(); winrt::apartment_context ui;
  std::string stage = "local-only"; bool passed = false; size_t rows = 0;
  std::vector<std::string> auditSecrets{app::Environment(L"HARMONY_TEST_PASSWORD"),app::Environment(L"HARMONY_TEST_EMAIL_CODE"),app::Environment(L"HARMONY_TEST_INVITE")};
  try {
    auto base = app::Environment(L"HARMONY_URL");
    ParityRequire(base.rfind("http://127.0.0.1:", 0) == 0 || base.rfind("http://localhost:", 0) == 0);
    ChatAuditPosition();
    window.AppWindow().ResizeClient({1280, 802}); // measured: the page is the client less 2, so 800 as on the web // 800 of page under the 32 of caption, as the web was measured
    // D12e: the roles run (testes/roles-audit.mjs) signs in on its own and stops.
    if (!app::Environment(L"HARMONY_AUDIT_ROLES").empty()) { co_await RolesAudit(stage); throw AuditStop{}; }
    if (!app::Environment(L"HARMONY_AUDIT_PERSONAL").empty()) { co_await PersonalAudit(stage); throw AuditStop{}; }
    stage = "login";
    // Login refuses while the service is busy, and it starts busy until
    // Restore has looked for a saved session (none here: persist is off).
    service->Restore();
    for (int i = 0; i < 100 && ReadState().busy; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    stage="recovery-open";
    ForgotPassword();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    ParityRequire(bool(forgotDialog));
    co_await SaveDesignSurface(forgotDialog,chatOutput,"nativo-esqueci-senha");
    auto forgotIdentifier=FindDesignPart(forgotDialog,L"ForgotIdentifier").as<TextBox>();
    forgotIdentifier.Text(net::Wide(app::Environment(L"HARMONY_TEST_USER")));
    co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
    stage="recovery-send";
    auto sendLink=FindDesignPart(forgotDialog,L"SendRecoveryLink").as<Button>();
    ParityRequire(sendLink.IsEnabled());
    Automation::Peers::ButtonAutomationPeer(sendLink).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
    for (int i=0;i<100 && sendLink.Visibility()!=Visibility::Collapsed;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(sendLink.Visibility()==Visibility::Collapsed && forgotIdentifier.Text().empty());
    stage="recovery-result";
    auto forgotStatus=FindDesignPart(forgotDialog,L"ForgotStatus").as<TextBlock>();
    ParityRequire(forgotStatus.Text()==L"Se a conta tem e-mail confirmado, enviaremos um link. Confira também o spam.");
    co_await SaveDesignSurface(forgotDialog,chatOutput,"nativo-esqueci-senha-enviado"); forgotDialog.Hide();
    co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    ParityRequire(!forgotDialog);
    stage="invite-signup";
    {
      auto press=[](FrameworkElement e) { Automation::Peers::ButtonAutomationPeer(e.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke(); };
      press(FindDesignPart(root,L"OpenInvite"));
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui; ParityRequire(bool(signupDialog));
      auto invite=FindDesignPart(signupDialog,L"SignupInvite").as<TextBox>();
      auto address=FindDesignPart(signupDialog,L"SignupEmail").as<TextBox>();
      auto name=FindDesignPart(signupDialog,L"SignupUsername").as<TextBox>();
      auto display=FindDesignPart(signupDialog,L"SignupDisplay").as<TextBox>();
      auto secret=FindDesignPart(signupDialog,L"SignupPassword").as<PasswordBox>();
      invite.Text(L"invalid-fixture"); name.Text(net::Wide(app::Environment(L"HARMONY_TEST_SIGNUP_USER")));
      address.Text(name.Text()+L"@example.invalid"); display.Text(L"Cadastro local"); secret.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD")));
      co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
      auto create=FindDesignPart(signupDialog,L"SignupCreate").as<Button>(); press(create);
      for(int i=0;i<100 && !invite.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(!ReadState().signedIn && FindDesignPart(signupDialog,L"SignupStatus").as<TextBlock>().Text()==L"Esse convite não vale mais.");
      co_await SaveDesignSurface(signupDialog,chatOutput,"nativo-convite"); invite.Text(net::Wide(app::Environment(L"HARMONY_TEST_INVITE")));
      co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; press(create);
      for(int i=0;i<100 && (!ReadState().signedIn || signupDialog || !FindDesignPart(root,L"EmailSkip"));++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(ReadState().signedIn && ReadState().needsEmail && !signupDialog && secret.Password().empty() && address.Text().empty() && invite.Text().empty());
      co_await SaveDesignSurface(root,chatOutput,"nativo-convite-confirmar-email"); press(FindDesignPart(root,L"EmailSkip"));
      for(int i=0;i<100 && ReadState().needsEmail;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      service->Logout();
      // The worker signs out before the queued Render mounts fresh login fields.
      // Never fill the detached controls retained from the previous login page.
      for(int i=0;i<100 && (ReadState().signedIn || showingRooms || emailGateShowing);++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(!ReadState().signedIn && !showingRooms && !emailGateShowing);
    }
    stage="login";
    user.Text(net::Wide(app::Environment(L"HARMONY_TEST_USER")));
    password.Password(L"refused-fixture-only");
    Submit();
    for (int i = 0; i < 100 && ReadState().busy; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(!ReadState().signedIn && !ReadState().error.empty());
    ParityRequire(!user.Text().empty() && password.Password() == L"refused-fixture-only");
    co_await SaveDesignSurface(root, chatOutput, "nativo-login-recusado");
    password.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD"))); Submit();
    for (int i = 0; i < 300 && !ReadState().signedIn; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(ReadState().signedIn);
    ParityRequire(password.Password().empty());
    stage="email-gate";
    for(int i=0;i<100 && !FindDesignPart(root,L"EmailCode");++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(ReadState().needsEmail && bool(FindDesignPart(root,L"EmailSkip")));
    co_await SaveDesignSurface(root,chatOutput,"nativo-confirmar-email");
    auto press=[](FrameworkElement e) { Automation::Peers::ButtonAutomationPeer(e.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke(); };
    press(FindDesignPart(root,L"EmailSkip"));
    for(int i=0;i<100 && ReadState().needsEmail;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(!ReadState().needsEmail); service->Logout();
    for(int i=0;i<100 && (ReadState().signedIn || showingRooms || emailGateShowing);++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(!ReadState().signedIn && !showingRooms && !emailGateShowing);
    user.Text(net::Wide(app::Environment(L"HARMONY_TEST_USER"))); password.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD"))); Submit();
    for(int i=0;i<100 && !FindDesignPart(root,L"EmailCode");++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(ReadState().signedIn && ReadState().needsEmail);
    auto emailCode=FindDesignPart(root,L"EmailCode").as<TextBox>(); emailCode.Text(L"invalid fixture");
    co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; press(FindDesignPart(root,L"EmailConfirm"));
    for(int i=0;i<100 && !emailView->region.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(ReadState().needsEmail && !FindDesignPart(root,L"EmailStatus").as<TextBlock>().Text().empty());
    emailCode.Text(net::Wide(app::Environment(L"HARMONY_TEST_EMAIL_CODE")));
    co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; press(FindDesignPart(root,L"EmailConfirm"));
    for(int i=0;i<100 && (ReadState().needsEmail || emailGateShowing);++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(!ReadState().needsEmail && !emailGateShowing && emailCode.Text().empty());
    stage = "history";
    for (int i = 0; i < 300; ++i) {
      bool ready = false;
      service->ChatModel().Inspect([&](app::ChatState const& s) {
        auto t = s.timelines.find(chatActive); ready = !chatActive.empty() && t != s.timelines.end() && t->second.loaded;
      });
      if (ready && messageItems.Size() > 0) break;
      co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
    }
    ParityRequire(messageItems.Size() > 0);
    if(app::Environment(L"HARMONY_PARITY6")=="1") {
      stage="tray-menu";
      ChatAuditPosition();
      for(bool hidden:{false,true}) {
        if(hidden) ShowWindow(hwnd,SW_HIDE);
        auto sent=std::make_shared<std::atomic<int>>(0);
        std::thread([target=control,sent] { DWORD_PTR result{}; sent->store(SendMessageTimeoutW(target,TrayMessage,0,MAKELPARAM(WM_CONTEXTMENU,1),SMTO_ABORTIFHUNG,2000,&result)?1:-1); }).detach();
        co_await winrt::resume_after(std::chrono::milliseconds(350)); co_await ui;
        logf("round6 tray: hidden=%d sent=%d main_visible=%d popup_visible=%d",hidden,sent->load(),bool(IsWindowVisible(hwnd)),trayPopup?bool(IsWindowVisible(trayPopup)):false);
        ParityRequire(sent->load()==1 && bool(IsWindowVisible(hwnd))==!hidden && trayPopup && IsWindowVisible(trayPopup));
        co_await SaveDesignWindow(trayPopup,chatOutput,hidden?L"round6-tray-menu-hidden.png":L"round6-tray-menu.png");
        // Windows delivers this when an outside click deactivates the menu.
        SendMessageW(trayPopup, WM_ACTIVATE, WA_INACTIVE, 0);
        co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
        ParityRequire(!IsWindowVisible(trayPopup) && bool(IsWindowVisible(hwnd))==!hidden);
        OpenTrayMenu();
        // Choose synchronously through the native menu's shared handler.
        // A queued click can reopen the main window after the next iteration hides it.
        ChooseTrayItem(0);
        ParityRequire(IsWindowVisible(hwnd) && !IsWindowVisible(trayPopup));
        ChatAuditPosition();
        auto previous=composer.Text(); composer.Text(L"tray responsiveness fixture");
        ParityRequire(composer.Text()==L"tray responsiveness fixture"); composer.Text(previous);
      }
      logf("PASS: tray menu preserves hidden window, dismisses outside and opens on request");
      co_await Parity6Audit(); co_await ui;
      if(app::Environment(L"HARMONY_PARITY7B")=="1") {stage="parity7b";co_await Parity7bAudit();co_await ui;}
      if(app::Environment(L"HARMONY_PARITY7")=="1") {stage="parity7";co_await Parity7Audit();co_await ui;}
    }
    co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
    co_await SaveDesignSurface(root, chatOutput, "nativo-conversa");
    co_await SaveDesignWindow(hwnd, chatOutput); std::filesystem::rename(chatOutput / L"nativo-janela-inteira.png", chatOutput / L"nativo-janela.png");
    if (app::Environment(L"HARMONY_PARITY5")=="1") { stage="parity5"; co_await Parity5Audit(); co_await ui; }
    if (app::Environment(L"HARMONY_AUDIT_COLUMN")=="1") {
      // D12b: the column with a folder, closed and open, the naming panel and
      // the pinned list. The fixture (chat-audit.mjs) made "Jogos" holding
      // #jogos and the voice room, and pinned a line of #geral. The run stops
      // here, before the voice stage, so nothing reaches the speakers.
      stage = "column";
      std::string folder, room;
      for (int i = 0; i < 50 && folder.empty(); ++i) {
        service->ChatModel().Inspect([&](app::ChatState const& s) {
          if (!s.categories.empty()) folder = s.categories[0].id;
          for (auto const& c : s.conversations) if (c.name == "jogos") room = c.id;
        });
        if (folder.empty()) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      }
      ParityRequire(!folder.empty() && !room.empty());
      co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
      ParityRequire(bool(FindDesignPart(root, L"FolderHeading")));
      auto island = FindDesignPart(root, L"RoomsIsland").as<FrameworkElement>();
      co_await SaveDesignSurface(island, chatOutput, "d12b-coluna");
      ToggleFolder(folder); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      co_await SaveDesignSurface(island, chatOutput, "d12b-coluna-fechada");
      ToggleFolder(folder); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      RenameRoom(room, "jogos"); co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
      ParityRequire(bool(nameDialog));
      co_await SaveDesignSurface(root, chatOutput, "d12b-renomear"); CloseNameDialog();
      // Exercise the shared production drag with laid-out points: #filmes onto JOGOS.
      {
        std::string filmes;
        service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.name == "filmes") filmes = c.id; });
        FrameworkElement from{nullptr}, into{nullptr};
        for (auto const& spot : dragSpots) { if (!spot.folder && spot.id == filmes) from = spot.el; if (spot.folder && spot.id == folder) into = spot.el; }
        ParityRequire(from && into);
        auto at = [&](FrameworkElement const& e) {
          return e.TransformToVisual(channelList).TransformPoint({float(e.ActualWidth() / 2), float(e.ActualHeight() / 2)});
        };
        auto a = at(from), b = at(into);
        ChannelDragBegin(from, false, filmes, a);
        for (int k = 1; k <= 12; ++k) {
          ChannelDragTo({a.X + (b.X - a.X) * k / 12, a.Y + (b.Y - a.Y) * k / 12});
          co_await winrt::resume_after(std::chrono::milliseconds(30)); co_await ui;
        }
        co_await winrt::resume_after(std::chrono::milliseconds(150)); co_await ui;
        co_await SaveDesignSurface(island, chatOutput, "d12b-arrastando");
        ChannelDragEnd();
        std::string landed;
        for (int i = 0; i < 50 && landed != folder; ++i) {
          service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.id == filmes) landed = c.category; });
          if (landed != folder) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
        }
        logf("d12b drag: #filmes %s the folder", landed == folder ? "landed in" : "did not land in");
        ParityRequire(landed == folder);
        co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
        co_await SaveDesignSurface(island, chatOutput, "d12b-arrastado");
      }
      OpenPins();
      for (int i = 0; i < 50 && searchResults && searchResults.Children().Size() == 0; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(searchResults && searchResults.Children().Size() == 1);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      co_await SaveDesignSurface(root, chatOutput, "d12b-fixadas");
      // The search among the pinned (02/11/2026): words that are in none hide
      // the line and say so; a word of it, in capitals, finds it again.
      {
        auto pinned = searchResults.Children().GetAt(0).as<Button>(); auto folded = std::wstring(unbox_value<hstring>(pinned.Tag()));
        auto word = folded.substr(0, folded.find(L' ')); for (auto& ch : word) ch = wchar_t(std::towupper(ch));
        searchBox.Text(L"palavra-que-nao-existe"); co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
        ParityRequire(pinned.Visibility() == Visibility::Collapsed);
        co_await SaveDesignSurface(root, chatOutput, "fixadas-busca-vazia");
        searchBox.Text(word); co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
        ParityRequire(pinned.Visibility() == Visibility::Visible);
        co_await SaveDesignSurface(root, chatOutput, "fixadas-busca");
        logf("pins search: \"%s\" finds the pinned line", net::Narrow(word).c_str());
      }
      CloseSearch();
      // The call's clock keeps one width whatever the digits (call-ui.h SteadyClock).
      {
        auto measure = [this](wchar_t const* digits) {
          auto t = SteadyClock(Line(digits, 13, Muted, 400)); t.Measure({1000, 100}); return t.DesiredSize().Width; };
        double narrow = measure(L"11:11"), wide = measure(L"44:44");
        logf("steady clock: 11:11 %.2f, 44:44 %.2f", narrow, wide);
        ParityRequire(std::abs(narrow - wide) < 0.01);
      }
      // The program pins by itself, through the same request the pin button
      // makes (it went out as a GET and the server said 404, 02/11/2026).
      {
        int64_t target = 0;
        service->ChatModel().Inspect([&](app::ChatState const& s) {
          auto t = s.timelines.find(chatActive); if (t == s.timelines.end()) return;
          for (auto it = t->second.messages.rbegin(); it != t->second.messages.rend() && !target; ++it) if (it->kind.empty() && !it->deleted && !it->pinned) target = it->id;
        });
        ParityRequire(target != 0);
        auto answer = std::make_shared<std::optional<std::string>>();
        service->ChatWork([answer, channel = chatActive, target](app::Chat& chat) { *answer = chat.Pin(channel, target, true); });
        for (int i = 0; i < 50 && !*answer; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
        logf("pin from the program: %s", !*answer ? "no answer" : (*answer)->empty() ? "pinned" : (*answer)->c_str());
        ParityRequire(*answer && (*answer)->empty());
        auto undo = std::make_shared<std::optional<std::string>>();
        service->ChatWork([undo, channel = chatActive, target](app::Chat& chat) { *undo = chat.Pin(channel, target, false); });
        for (int i = 0; i < 50 && !*undo; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
        ParityRequire(*undo && (*undo)->empty());
      }
      // A message's actions: here bruna's or carlos's line, which the owner
      // may pin and remove but not edit, four buttons; the bar is as wide as
      // its buttons, "remover" last and whole (it was cut off: the width
      // forgot the pin).
      {
        Border bar{nullptr};
        for (int i = 0; i < 30 && !bar; ++i) {
          std::vector<DependencyObject> todo{messageList};
          while (!todo.empty() && !bar) {
            auto at = todo.back(); todo.pop_back();
            if (auto b = at.try_as<Border>(); b && b.Name() == L"Actions" && b.Width() >= 4 * 34 + 6) bar = b;
            for (int c = 0; c < VisualTreeHelper::GetChildrenCount(at); ++c) todo.push_back(VisualTreeHelper::GetChild(at, c));
          }
          if (!bar) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
        }
        ParityRequire(bool(bar));
        bar.Opacity(1); co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
        auto buttons = bar.Child().as<StackPanel>(); ParityRequire(buttons.Children().Size() * 34 + 6 == bar.Width());
        ParityRequire(buttons.Children().GetAt(buttons.Children().Size() - 1).as<Button>().Name() != L"PinAction");
        ParityRequire(buttons.ActualWidth() + 8 <= bar.Width() + 0.5);
        logf("message actions: %u buttons in a %.0f bar", buttons.Children().Size(), bar.Width());
        // The pin's own square: its glyph was not drawn at 16 (main.cpp VoiceIcon).
        co_await SaveDesignSurface(bar, chatOutput, "acoes-barra-sozinha");
        co_await SaveDesignSurface(root, chatOutput, "acoes-mensagem");
        // The mention's rounded tint (02/11/2026): at the start, mid-line,
        // after an emoji, twice in a line, and broken across two lines.
        StackPanel lines; lines.Background(Brush(Surface)); lines.Padding(Thickness{16, 16, 16, 16}); lines.Spacing(8); lines.Width(360);
        lines.HorizontalAlignment(HorizontalAlignment::Left); lines.VerticalAlignment(VerticalAlignment::Top);
        for (auto text : {"@bruna cade meu amigo", "oi @observador tudo bem", "\xF0\x9F\x98\x80 @carlos depois do emoji", "@todos e @bruna juntos", "uma frase comprida que empurra a menção para quebrar @bruna"}) {
          app::ChatMessage line; line.body = text; lines.Children().Append(MessageBody(line, ReadOwner()));
        }
        Grid::SetColumnSpan(lines, (std::max)(1, int(root.ColumnDefinitions().Size()))); Grid::SetRowSpan(lines, (std::max)(1, int(root.RowDefinitions().Size())));
        Canvas::SetZIndex(lines, 2000); root.Children().Append(lines);
        co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
        co_await SaveDesignSurface(lines, chatOutput, "mencoes-arredondadas");
        { uint32_t at; if (root.Children().IndexOf(lines, at)) root.Children().RemoveAt(at); }
        // Where a click on a button's edge lands (the owner, 02/11/2026: "clique
        // na borda falha", the profile's x first): the topmost element under
        // points along each side and at each corner, asked of the window
        // itself, so the owner's mouse never moves.
        auto probe = [this](Button const& b, char const* label) {
          int misses = 0, total = 0; std::string where;
          double w = b.ActualWidth(), h = b.ActualHeight();
          auto to = b.TransformToVisual(nullptr);
          for (double d : {0.5, 1.5, 3.0}) for (double fx : {0.0, 0.5, 1.0}) for (double fy : {0.0, 0.5, 1.0}) {
            if (fx == 0.5 && fy == 0.5) continue;
            double x = fx == 0 ? d : fx == 1 ? w - d : w / 2, y = fy == 0 ? d : fy == 1 ? h - d : h / 2;
            auto at = to.TransformPoint({float(x), float(y)}); ++total;
            auto hits = Media::VisualTreeHelper::FindElementsInHostCoordinates(at, window.Content(), false);
            UIElement top{nullptr}; for (auto const& e : hits) { top = e; break; }
            bool inside = false;
            for (DependencyObject n = top; n; n = VisualTreeHelper::GetParent(n)) if (n == b) { inside = true; break; }
            if (!inside) { ++misses; if (where.size() < 2000) { char s[96]; snprintf(s, sizeof s, " (%.1f,%.1f)->%s:%s", x, y, top ? to_string(winrt::get_class_name(top)).c_str() : "none", top ? to_string(top.as<FrameworkElement>().Name()).c_str() : ""); where += s; } }
          }
          logf("edge probe %s %.0fx%.0f: %d of %d edge points miss%s", label, w, h, misses, total, where.c_str());
          // Only the tips of the rounded corners, outside the drawn shape: 2
          // insets x 4 corners. The idle scroll bar over the profile's right
          // edge took 4 more (wheel-scroll.h ScrollLaneWhenNeeded).
          return misses;
        };
        { app::ChatPerson peer; peer.username = "bruna"; OpenProfile(peer); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
          auto x = FindDesignPart(profileOverlay, L"CloseProfile").try_as<Button>(); ParityRequire(bool(x) && probe(x, "profile-x") <= 8);
          CloseProfile(); co_await ui; }
        if (auto pins = FindDesignPart(root, L"HeaderPins").try_as<Button>()) ParityRequire(probe(pins, "header-pin") <= 8);
        bar.Opacity(1); bar.IsHitTestVisible(true); co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
        { auto buttons = bar.Child().as<StackPanel>(); ParityRequire(probe(buttons.Children().GetAt(0).as<Button>(), "action-react") <= 8);
          ParityRequire(probe(buttons.Children().GetAt(buttons.Children().Size() - 1).as<Button>(), "action-remove") <= 8); }
        bar.IsHitTestVisible(false);
        bar.Opacity(0);
      }
      // Opening lands (30/10/2026): #memes, never read, on its end; #avisos,
      // read up to its tenth line, with the unread line near the top. Read
      // after the list has had time to settle, from the scroll itself.
      stage = "landing";
      for (auto const& [name, unread] : {std::pair{"memes", false}, std::pair{"avisos", true}}) {
        std::string id;
        service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.name == name) id = c.id; });
        ParityRequire(!id.empty());
        OpenConversation(id);
        co_await winrt::resume_after(std::chrono::milliseconds(2500)); co_await ui;
        ParityRequire(messageScroll && messageItems.Size() >= 30);
        double offset = messageScroll.VerticalOffset(), max = messageScroll.ScrollableHeight();
        double line = -1;
        if (auto mark = FindDesignPart(messageList, L"UnreadLine").try_as<FrameworkElement>())
          line = mark.TransformToVisual(messageScroll).TransformPoint({0, 0}).Y;
        logf("landing #%s: offset %.0f of %.0f, unread line at %.0f", name, offset, max, line);
        co_await SaveDesignSurface(root, chatOutput, std::string("aterrissa-") + name);
        if (unread) ParityRequire(line >= 0 && line < 160 && max - offset > 200);
        else ParityRequire(max > 400 && max - offset < 24);
      }
      // The + beside CANAIS and VOZ (30/10/2026, "ta faltando ... criar
      // canais"): pressed, named as typed ("Novo Canal" becomes novo-canal),
      // and the text one opens; then Apagar canal on the open one moves on.
      stage = "new-room";
      {
        auto press = [](FrameworkElement e) { Automation::Peers::ButtonAutomationPeer(e.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke(); };
        auto named = [&](std::string const& kind, std::string const& name) {
          std::string id;
          service->ChatModel().Inspect([&](app::ChatState const& s) {
            for (auto const& c : kind == "voice" ? s.rooms : s.conversations) if (c.name == name) id = c.id;
          });
          return id;
        };
        auto plus = FindDesignPart(root, L"NewTextChannel").try_as<FrameworkElement>(); ParityRequire(bool(plus));
        press(plus); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
        ParityRequire(bool(nameDialog));
        FindDesignPart(nameDialog, L"NameBox").as<TextBox>().Text(L"Novo Canal");
        co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
        ParityRequire(FindDesignPart(nameDialog, L"NameBox").as<TextBox>().Text() == L"novo-canal");
        co_await SaveDesignSurface(root, chatOutput, "canal-novo");
        press(FindDesignPart(nameDialog, L"NameSave").as<FrameworkElement>());
        std::string created;
        for (int i = 0; i < 50 && (created.empty() || chatActive != created); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; created = named("text", "novo-canal"); }
        logf("new room: #novo-canal %s and %s", created.empty() ? "missing" : "made", chatActive == created ? "open" : "not open");
        ParityRequire(!created.empty() && chatActive == created && !nameDialog);
        co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
        co_await SaveDesignSurface(root, chatOutput, "canal-novo-aberto");
        auto voicePlus = FindDesignPart(root, L"NewVoiceChannel").try_as<FrameworkElement>(); ParityRequire(bool(voicePlus));
        press(voicePlus); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
        ParityRequire(bool(nameDialog));
        FindDesignPart(nameDialog, L"NameBox").as<TextBox>().Text(L"sala 2");
        press(FindDesignPart(nameDialog, L"NameSave").as<FrameworkElement>());
        std::string room;
        for (int i = 0; i < 50 && room.empty(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; room = named("voice", "sala-2"); }
        ParityRequire(!room.empty() && chatActive == created);
        co_await winrt::resume_after(std::chrono::milliseconds(600)); co_await ui;
        co_await SaveDesignSurface(FindDesignPart(root, L"RoomsIsland").as<FrameworkElement>(), chatOutput, "canal-voz-novo");
        // The question first, as the owner sees it; then the deletion itself.
        RemoveRoom(created, "novo-canal", false);
        co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
        for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
          if (auto dialog = popup.Child().try_as<ContentDialog>()) { co_await SaveDesignSurface(dialog, chatOutput, "canal-apagar"); dialog.Hide(); }
        auto removed = std::make_shared<std::atomic<int>>(-1); // -1 pending, 0 done, 1 refused
        service->ChatWork([created, room, removed](app::Chat& chat) { bool ok = chat.RemoveChannel(created).empty() && chat.RemoveChannel(room).empty(); removed->store(ok ? 0 : 1); });
        for (int i = 0; i < 50 && (removed->load() < 0 || chatActive == created || !named("voice", "sala-2").empty()); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
        logf("remove room: %d, open now %s", removed->load(), chatActive == created ? "the deleted one" : "another");
        ParityRequire(removed->load() == 0 && !chatActive.empty() && chatActive != created && named("text", "novo-canal").empty());
      }
      throw AuditStop{};
    }
    stage = "people-toggle";
    {
      auto button = FindDesignPart(root,L"ChatPeople").as<Button>();
      auto invoke = Automation::Peers::ButtonAutomationPeer(button).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>();
      invoke.Invoke();
      ParityRequire(!peopleShown && peopleIsland.Visibility() == Visibility::Collapsed && root.ColumnDefinitions().GetAt(2).Width().Value == 0);
      root.UpdateLayout(); co_await SaveDesignSurface(root,chatOutput,"nativo-pessoas-recolhidas");
      invoke.Invoke(); ParityRequire(peopleShown && peopleIsland.Visibility() == Visibility::Visible);
    }
    stage = "channel-badges";
    {
      StackPanel samples; samples.Spacing(2);
      auto plain = ChannelRow(L"jogos", "hashtag", false, 7, 0, false, [] {});
      auto mention = ChannelRow(L"filmes", "hashtag", false, 123, 120, false, [] {});
      auto muted = ChannelRow(L"musica", "hashtag", false, 0, 0, true, [] {});
      app::ChatPerson person; person.username = "bruna"; person.displayName = "Bruna";
      auto dm = ChannelRow(L"Bruna", "hashtag", false, 5, 2, false, [] {}, PhotoFace(person, 32));
      ParityRequire(FindDesignPart(plain, L"UnreadBadge").Height() == 24);
      ParityRequire(FindDesignPart(mention, L"MentionBadge").Height() == 24);
      ParityRequire(!FindDesignPart(dm, L"MentionBadge") && bool(FindDesignPart(dm, L"UnreadBadge")));
      for (auto row : {plain, mention, muted, dm}) samples.Children().Append(row);
      Border fixture; fixture.Width(244); fixture.Padding(Thickness{8}); fixture.Background(Brush(Surface)); fixture.Child(samples);
      fixture.HorizontalAlignment(HorizontalAlignment::Left); fixture.VerticalAlignment(VerticalAlignment::Top); Grid::SetColumn(fixture, 1);
      root.Children().Append(fixture);
      co_await SaveDesignSurface(fixture, chatOutput, "nativo-contadores-sinteticos");
      root.Children().RemoveAtEnd();
    }
    stage = "empty-conversation";
    {
      auto previous = chatActive; std::string empty;
      service->ChatModel().Inspect([&](app::ChatState const& s) {
        for (auto const& c : s.conversations) if (c.name == "prints" && c.kind == "text") empty = c.id;
      });
      ParityRequire(!empty.empty()); OpenConversation(empty);
      for (int i = 0; i < 100 && emptyLine.Visibility() != Visibility::Visible; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(emptyLine.Visibility() == Visibility::Visible && emptyLine.VerticalAlignment() == VerticalAlignment::Bottom);
      co_await SaveDesignSurface(root, chatOutput, "nativo-conversa-vazia");
      OpenConversation(previous);
      for (int i = 0; i < 100 && messageItems.Size() == 0; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(messageItems.Size() > 0);
    }
    stage = "idle";
    {
      // Sitting still, the window must be free: a busy one delivers the
      // pointer late and only where it ended up, so hover lags and skips rows.
      auto idleMeter = window.DispatcherQueue().CreateTimer(); idleMeter.Interval(std::chrono::milliseconds(10));
      auto last = std::make_shared<ULONGLONG>(GetTickCount64()); auto longest = std::make_shared<ULONGLONG>(0); auto ticks = std::make_shared<int>(0);
      idleMeter.Tick([last, longest, ticks](auto const&, auto const&) { auto now = GetTickCount64(); *longest = std::max(*longest, now - *last); *last = now; ++*ticks; });
      auto r0 = renders; auto ms0 = renderMs; *last = GetTickCount64(); idleMeter.Start();
      co_await winrt::resume_after(std::chrono::milliseconds(5000)); co_await ui; idleMeter.Stop();
      logf("idle 5 s: %llu renders, %.1f ms drawing, longest stall %llu ms, %d ticks", renders - r0, renderMs - ms0, *longest, *ticks);
    }
    stage = "conversation-switch";
    {
      // The owner's "travadinha" when changing conversations: how long the
      // click itself holds the window, how many times the list is rebuilt
      // and the longest stall until it settles.
      auto channel = chatActive; std::string direct;
      service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.kind == "dm" && !c.hidden) direct = c.id; });
      ParityRequire(!direct.empty());
      auto meter = window.DispatcherQueue().CreateTimer(); meter.Interval(std::chrono::milliseconds(10));
      auto last = std::make_shared<ULONGLONG>(GetTickCount64()); auto longest = std::make_shared<ULONGLONG>(0);
      meter.Tick([last, longest](auto const&, auto const&) { auto now = GetTickCount64(); *longest = std::max(*longest, now - *last); *last = now; });
      meter.Start();
      double clickWorst = 0, clickTotal = 0, filledWorst = 0; uint64_t rebuildWorst = 0, rebuildTotal = 0, stallWorst = 0, rendersTotal = 0;
      int samples = 10;
      for (int i = 0; i < samples; ++i) {
        auto target = i % 2 ? channel : direct;
        chatTab = target == direct ? "dm" : "server";
        co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
        auto b0 = listRebuilds; auto r0 = renders; *last = GetTickCount64(); *longest = 0;
        auto f0 = fillMs, c0 = channelsMs, s0 = scrollMs, x0 = clearMs; auto n0 = fills;
        LARGE_INTEGER t0, t1, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        // The chat worker is often still on a request (marking read, the last
        // page) when a conversation is picked: hold it as a slow network would.
        if (i >= 6) service->ChatWork([](app::Chat&) { Sleep(300); });
        OpenConversation(target);
        QueryPerformanceCounter(&t1); double click = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        // A call redraws all the time (who is speaking): a redraw before the
        // chat worker has opened the conversation must not send it back
        // (Fulano, 24/10/2026: #geral and #imagens alternating on their own).
        if (i >= 6) for (int k = 0; k < 5; ++k) { co_await winrt::resume_after(std::chrono::milliseconds(40)); co_await ui; Render(); }
        else Render();
        double filled = -1;
        for (int k = 0; k < 150; ++k) {
          if (filled < 0 && messageItems.Size() > 0) { QueryPerformanceCounter(&t1); filled = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart); }
          co_await winrt::resume_after(std::chrono::milliseconds(10)); co_await ui;
        }
        auto rebuilt = listRebuilds - b0;
        if (chatActive != target) logf("switch %d bounced: asked %s, showing %s", i + 1, target == direct ? "dm" : "channel", chatActive == direct ? "dm" : chatActive == channel ? "channel" : "other");
        ParityRequire(chatActive == target);
        logf("switch %d spent: %llu rows built in %.1f ms, channel list %.1f ms, scroll %.1f ms, clear %.1f ms", i + 1, fills - n0, fillMs - f0, channelsMs - c0, scrollMs - s0, clearMs - x0);
        logf("switch %d to %s: click %.1f ms, filled at %.1f ms, %llu list rebuilds, %llu renders, longest stall %llu ms", i + 1, target == direct ? "dm" : "channel", click, filled, rebuilt, renders - r0, *longest);
        clickWorst = std::max(clickWorst, click); clickTotal += click; filledWorst = std::max(filledWorst, filled);
        rebuildWorst = std::max(rebuildWorst, rebuilt); rebuildTotal += rebuilt; stallWorst = std::max(stallWorst, *longest); rendersTotal += renders - r0;
      }
      meter.Stop();
      logf("conversation switch: click %.1f ms average, %.1f ms worst; filled by %.1f ms; %.1f rebuilds average, %llu worst; %.1f renders average; longest stall %llu ms",
           clickTotal / samples, clickWorst, filledWorst, double(rebuildTotal) / samples, rebuildWorst, double(rendersTotal) / samples, stallWorst);
      if (chatActive != channel) { chatTab = "server"; OpenConversation(channel); co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui; }
    }
    stage = "people-band";
    {
      // The hover band of a person must reach across the island, the same
      // gap on both sides (the owner, 25/10/2026: a blank strip on the right).
      auto island = FindDesignPart(root, L"PeopleIsland");
      FrameworkElement band{nullptr};
      for (uint32_t i = 0; i < peopleList.Children().Size() && !band; ++i)
        if (auto b = peopleList.Children().GetAt(i).try_as<Border>(); b && std::wstring_view(b.Name()).starts_with(L"Person-")) band = b;
      ParityRequire(island && band);
      auto a = island.TransformToVisual(root).TransformBounds({0, 0, float(island.ActualWidth()), float(island.ActualHeight())});
      auto b = band.TransformToVisual(root).TransformBounds({0, 0, float(band.ActualWidth()), float(band.ActualHeight())});
      double left = b.X - a.X, right = (a.X + a.Width) - (b.X + b.Width);
      logf("people band: island %.1f wide, band %.1f, gap left %.1f right %.1f", a.Width, b.Width, left, right);
      ParityRequire(std::abs(left - right) < 1);
    }
    stage = "presence-renders";
    for (int i = 0; i < 6; ++i) {
      std::string desired = i % 2 ? "active" : "away";
      auto r0 = renders;
      logf("presence render begin: sample %d, requested %s", i + 1, desired.c_str());
      service->Presence(desired);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      auto st = ReadState(); auto found = st.model.presence.find(ReadOwner());
      logf("presence render end: sample %d, renders %llu, observed %s", i + 1, renders - r0,
           found == st.model.presence.end() ? "missing" : found->second.c_str());
    }
    stage = "hover-cost";
    {
      // What a pointer passing over the list costs the window: each row's
      // first hover lays out its action bar; and what one icon costs to make.
      auto ms = [](LARGE_INTEGER a) { LARGE_INTEGER b, f; QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f); return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart); };
      double worst = 0, total = 0; int rowsSeen = 0;
      for (uint32_t i = 0; i < messageItems.Size(); ++i) {
        auto row = messageList.ContainerFromIndex(i).try_as<FrameworkElement>(); if (!row) continue;
        auto bar = FindDesignPart(row, L"Actions"); if (!bar) continue;
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        bar.Opacity(1); bar.IsHitTestVisible(true); root.UpdateLayout();
        auto took = ms(t0); worst = std::max(worst, took); total += took; ++rowsSeen;
        bar.Opacity(0); bar.IsHitTestVisible(false);
      }
      LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
      for (int i = 0; i < 100; ++i) VoiceIcon("pencil", Muted, 16);
      auto icons = ms(t0);
      logf("hover cost: %d rows, first show %.1f ms average, %.1f ms worst; 100 icons made in %.1f ms", rowsSeen, rowsSeen ? total / rowsSeen : 0.0, worst, icons);
    }
    stage = "voice";
    {
      // The same stillness with the voice room open in the background and
      // someone speaking (bruna, on the web): every speaking change asks the
      // window for a redraw, and the pointer waits behind them.
      std::string roomId; { auto st = ReadState(); if (!st.model.rooms.empty()) roomId = st.model.rooms.front().id; }
      ParityRequire(!roomId.empty());
      service->JoinVoice(roomId);
      for (int i = 0; i < 150 && (ReadState().voice.room.empty() || ReadState().voice.connecting); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(!ReadState().voice.connecting);
      co_await winrt::resume_after(std::chrono::milliseconds(2000)); co_await ui;
      auto voiceMeter = window.DispatcherQueue().CreateTimer(); voiceMeter.Interval(std::chrono::milliseconds(10));
      auto last = std::make_shared<ULONGLONG>(GetTickCount64()); auto longest = std::make_shared<ULONGLONG>(0); auto ticks = std::make_shared<int>(0);
      voiceMeter.Tick([last, longest, ticks](auto const&, auto const&) { auto now = GetTickCount64(); *longest = std::max(*longest, now - *last); *last = now; ++*ticks; });
      auto r0 = renders; auto ms0 = renderMs; *last = GetTickCount64(); voiceMeter.Start();
      co_await winrt::resume_after(std::chrono::milliseconds(5000)); co_await ui; voiceMeter.Stop();
      logf("voice 5 s: %llu renders, %.1f ms drawing, longest stall %llu ms, %d ticks", renders - r0, renderMs - ms0, *longest, *ticks);
      if(app::Environment(L"HARMONY_PARITY5")=="1") {
        stage="voice-ring";
        for(int i=0;i<60 && (!voiceFaces.count("bruna") || voiceFaces.at("bruna").Visibility()!=Visibility::Visible);++i) {
          co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui;
        }
        ParityRequire(voiceFaces.count("bruna") && voiceFaces.at("bruna").Visibility()==Visibility::Visible);
      }
      co_await SaveDesignSurface(root, chatOutput, "nativo-voz-na-conversa");
      for(auto const& [who,ring]:voiceFaces) {
        auto stroke=ring.BorderThickness(); ParityRequire(stroke.Left==2 && stroke.Top==2 && stroke.Right==2 && stroke.Bottom==2);
      }
      showingCall = true; callSignature.clear(); Render();
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      co_await SaveDesignSurface(root, chatOutput, "nativo-sala-de-voz");
      showingCall = false; callSignature.clear(); Render();
      service->LeaveVoice();
      for (int i = 0; i < 50 && !ReadState().voice.room.empty(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    }
    // Pointer delivery is opt-in; ordinary runs never move the owner's pointer.
    bool realInput = app::Environment(L"HARMONY_AUDIT_REAL_POINTER") == "1";
    stage = "hover-real";
    if (realInput) {
      // The real pointer, through SendInput (SetCursorPos never reaches WinUI's
      // pointer events): the cursor is put on each channel
      // row, slowly and then at a fast sweep, and each row notes when it heard
      // the pointer arrive. Slow: the delay to light; fast: how many lit.
      stage = "hover-real-not-foreground";
      co_await ChatAuditForeground("hover real");
      logf("hover real start: foreground %d", GetForegroundWindow() == hwnd);
      stage = "hover-real";
      std::vector<FrameworkElement> lines; auto heard = std::make_shared<std::vector<LONGLONG>>();
      auto clock = [] { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return LONGLONG(t.QuadPart * 1000000 / f.QuadPart); };
      for (auto child : channelList.Children()) {
        auto g = child.try_as<Grid>(); if (!g || g.Height() != 36) continue;
        size_t k = lines.size(); lines.push_back(g); heard->push_back(0);
        g.PointerEntered([heard, k, clock](auto const&, auto const&) { (*heard)[k] = clock(); });
      }
      ParityRequire(lines.size() >= 4);
      double scale = GetDpiForWindow(hwnd) / 96.0;
      auto put = [&](FrameworkElement const& e) {
        auto at = e.TransformToVisual(nullptr).TransformPoint({float(e.ActualWidth() / 2), float(e.ActualHeight() / 2)});
        POINT p{LONG(at.X * scale), LONG(at.Y * scale)}; ClientToScreen(hwnd, &p);
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        in.mi.dx = LONG(((p.x - vx) * 65535.0) / (vw - 1)); in.mi.dy = LONG(((p.y - vy) * 65535.0) / (vh - 1)); AuditSendInput(1, &in, sizeof(in));
      };
      POINT away{}; GetCursorPos(&away);
      double worst = 0, total = 0; int lit = 0;
      for (size_t k = 0; k < lines.size(); ++k) {
        (*heard)[k] = 0; auto t0 = clock(); put(lines[k]);
        for (int i = 0; i < 50 && !(*heard)[k]; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(4)); co_await ui; }
        if ((*heard)[k]) { double ms = ((*heard)[k] - t0) / 1000.0; worst = std::max(worst, ms); total += ms; ++lit; }
        co_await winrt::resume_after(std::chrono::milliseconds(150)); co_await ui;
      }
      logf("hover real slow: %d of %zu rows lit, %.1f ms average, %.1f ms worst", lit, lines.size(), lit ? total / lit : 0.0, worst);
      // What the scheduler sees: a window that is not in front gets neither
      // the foreground quantum nor the input boost.
      logf("hover real context: foreground %d, priority class %lx, UI thread priority %d",
           GetForegroundWindow() == hwnd, GetPriorityClass(GetCurrentProcess()), GetThreadPriority(GetCurrentThread()));
      {
        // Whose window is in front, and whose is under the last row's centre.
        auto describe = [](HWND w) {
          wchar_t title[128]{}, cls[64]{}; GetWindowTextW(w, title, 128); GetClassNameW(GetAncestor(w, GA_ROOT), cls, 64);
          DWORD pid = 0; GetWindowThreadProcessId(w, &pid); wchar_t image[MAX_PATH]{}; DWORD n = MAX_PATH;
          if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { QueryFullProcessImageNameW(p, 0, image, &n); CloseHandle(p); }
          return net::Narrow(std::wstring(L"pid ") + std::to_wstring(pid) + L" " + std::filesystem::path(image).filename().wstring() + L" [" + cls + L"] \"" + title + L"\"");
        };
        auto at = lines.back().TransformToVisual(nullptr).TransformPoint({float(lines.back().ActualWidth() / 2), float(lines.back().ActualHeight() / 2)});
        double s = GetDpiForWindow(hwnd) / 96.0; POINT p{LONG(at.X * s), LONG(at.Y * s)}; ClientToScreen(hwnd, &p);
        logf("hover real windows: ours pid %lu, foreground %s, under the row %s", GetCurrentProcessId(),
             describe(GetForegroundWindow()).c_str(), describe(GetAncestor(WindowFromPoint(p), GA_ROOT)).c_str());
      }
      ParityRequire(lit == int(lines.size()));
      for (auto& h : *heard) h = 0;
      for (size_t k = 0; k < lines.size(); ++k) { put(lines[k]); co_await winrt::resume_after(std::chrono::milliseconds(16)); co_await ui; }
      co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
      int fast = 0; for (auto h : *heard) fast += h != 0;
      logf("hover real fast (16 ms a row): %d of %zu rows lit", fast, lines.size());
      AuditSetCursorPos(away.x, away.y);
    } else logf("hover real: skipped");
    stage = "send";
    ChatAuditPosition();
    composer.Text(L" \t\r\n\u00a0\u2003");
    ParityRequire(!sendButton.IsEnabled());
    composer.Text(L"");
    // TextBox inserts the newline itself. Prove only that our complete
    // preview handler neither consumes Shift+Enter nor sends a message.
    auto beforeNewline = messageItems.Size();
    ParityRequire(!ComposerKey(winrt::Windows::System::VirtualKey::Enter, true));
    co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
    ParityRequire(composer.Text().empty() && !sendWaiting && messageItems.Size() == beforeNewline);
    auto before = messageItems.Size();
    auto stamp = L"enviada pelo nativo " + std::to_wstring(GetTickCount64() % 100000);
    composer.Text(stamp); ParityRequire(ComposerKey(winrt::Windows::System::VirtualKey::Enter, false));
    for (int i = 0; i < 100 && messageItems.Size() == before; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(messageItems.Size() == before + 1);
    for (int i = 0; i < 50 && !composer.Text().empty(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(composer.Text().empty()); // cleared only after the server said yes
    // There must be an own message to edit: testing ArrowUp before sending
    // anything would also pass with the old, unwanted edit shortcut.
    ParityRequire(!ComposerKey(winrt::Windows::System::VirtualKey::Up, false));
    co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
    ParityRequire(!editingId && composer.Text().empty());
    co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
    co_await SaveDesignSurface(root, chatOutput, "nativo-enviada");
    stage = "link-card";
    {
      // Sent at once, the card comes 0.4 s after its row and must be seen
      // without scrolling (the owner, 30/10/2026: "sent at once, no card").
      // The local server reads Vimeo's oEmbed on the internet.
      auto sent = messageItems.Size();
      composer.Text(L"https://vimeo.com/76979871"); SendComposer();
      for (int i = 0; i < 100 && messageItems.Size() == sent; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(messageItems.Size() == sent + 1);
      auto carded = [&] { auto row = ReadRow(unbox_value<int64_t>(messageItems.GetAt(messageItems.Size() - 1))); return row && !row->m.embeds.empty(); };
      for (int i = 0; i < 150 && !carded(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(carded());
      co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
      ParityRequire(messageScroll && messageScroll.ScrollableHeight() - messageScroll.VerticalOffset() < 24);
      co_await SaveDesignSurface(root, chatOutput, "nativo-cartao-de-link");
    }
    rows = messageItems.Size();
    stage = "emoji-panel";
    // The panel must not hold the window: the longest gap between two ticks
    // of a 10 ms timer, while it opens and while it is scrolled to the end.
    auto meter = window.DispatcherQueue().CreateTimer(); meter.Interval(std::chrono::milliseconds(10));
    auto lastTick = std::make_shared<ULONGLONG>(GetTickCount64()); auto worst = std::make_shared<ULONGLONG>(0);
    meter.Tick([lastTick, worst](auto const&, auto const&) { auto now = GetTickCount64(); *worst = std::max(*worst, now - *lastTick); *lastTick = now; });
    *lastTick = GetTickCount64(); meter.Start();
    OpenEmojiPanel();
    for (int i = 0; i < 30 && !picker; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
    ParityRequire(bool(picker));
    co_await winrt::resume_after(std::chrono::milliseconds(3000)); co_await ui;
    auto openStall = *worst; *worst = 0;
    for (int i = 1; i <= 20; ++i) { pickerList.ChangeView(nullptr, pickerList.ScrollableHeight() * i / 20, nullptr, true); co_await winrt::resume_after(std::chrono::milliseconds(150)); co_await ui; }
    co_await winrt::resume_after(std::chrono::milliseconds(2000)); co_await ui;
    meter.Stop();
    logf("emoji panel: longest UI stall %llu ms opening, %llu ms scrolling to the end", openStall, *worst);
    ParityRequire(openStall < 250 && *worst < 250);
    pickerList.ChangeView(nullptr, 0.0, nullptr, true); co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
    co_await SaveDesignSurface(picker.Child().as<FrameworkElement>(), chatOutput, "nativo-emoji");
    {
      // Where the web puts it: 8 inside the composer's island, 8 above it.
      auto card = picker.Child().as<FrameworkElement>(); auto island = FindAncestor(composer, L"ComposerIsland");
      auto c = card.TransformToVisual(nullptr).TransformPoint({0, 0}); auto i = island.TransformToVisual(nullptr).TransformPoint({0, 0});
      logf("emoji panel at %.0f,%.0f %.0fx%.0f, island at %.0f,%.0f", c.X, c.Y, card.ActualWidth(), card.ActualHeight(), i.X, i.Y);
      ParityRequire(std::abs(c.X - (i.X + 8)) < 1 && std::abs(c.Y + card.ActualHeight() - (i.Y - 8)) < 1 && card.ActualWidth() == 384 && card.ActualHeight() == 420);
    }
    {
      stage="emoji-search-fill";
      auto search = FindDesignPart(picker.Child(), L"EmojiSearch").as<TextBox>(); search.Text(L"fogo");
      co_await winrt::resume_after(std::chrono::milliseconds(1200)); co_await ui;
      stage="emoji-search-snapshot";
      co_await SaveDesignSurface(picker.Child().as<FrameworkElement>(), chatOutput, "nativo-emoji-fogo");
      // The first result, as a click would pick it.
      ParityRequire(pickerFound.list.size() > 0); auto first = pickerFound.list[0];
      stage="emoji-choose";
      auto choose = pickerChoose; RememberRecent(first); ClosePicker(); choose(first);
      ParityRequire(std::wstring(composer.Text()).find(net::Wide(first)) != std::wstring::npos);
      composer.Text(L"");
    }
    stage = "attach";
    {
      // A picture (from HARMONY_TEST_PICTURE) and a text file go up, show as
      // tiles, and leave with the message; then nothing is left over.
      auto text = chatOutput / L"notas da viagem.txt"; { std::ofstream(text, std::ios::binary) << std::string(200000, 'x'); }
      std::vector<std::filesystem::path> both{text}; auto picture = app::Environment(L"HARMONY_TEST_PICTURE");
      if (!picture.empty()) both.insert(both.begin(), std::filesystem::path(net::Wide(picture)));
      stage = "attach-start"; Attach(both); ParityRequire(pendingFiles.size() == both.size());
      for (int i = 0; i < 100 && Climbing(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      stage = "attach-ready"; ParityRequire(!Climbing() && ReadyFiles() == both.size() && sendButton.IsEnabled());
      co_await winrt::resume_after(std::chrono::milliseconds(600)); co_await ui;
      co_await SaveDesignSurface(root, chatOutput, "nativo-anexos");
      if(app::Environment(L"HARMONY_PARITY6")=="1") {
        for(auto const& file:pendingFiles) if(file.stored && file.stored->mime.rfind("image/",0)==0) {
          auto tile=FindDesignPart(uploadRow,L"PendingImage"+std::to_wstring(file.key)); ParityRequire(bool(tile)); OpenPendingImage(*file.stored);
          co_await winrt::resume_after(std::chrono::milliseconds(200));co_await ui;
          ParityRequire(bool(lightbox)); co_await SaveDesignSurface(root,chatOutput,"round6-pending-image-viewer");
          Automation::Peers::ButtonAutomationPeer(FindDesignPart(lightbox,L"LightboxClose").as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
          co_await winrt::resume_after(std::chrono::milliseconds(100));co_await ui;
          ParityRequire(!lightbox && ReadyFiles()==both.size()); break;
        }
      }
      stage = "attach-send"; auto before = messageItems.Size(); SendComposer();
      for (int i = 0; i < 100 && (sendWaiting || messageItems.Size() == before); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(messageItems.Size() == before + 1 && pendingFiles.empty());
      size_t went = 0; auto last = unbox_value<int64_t>(messageItems.GetAt(messageItems.Size() - 1));
      service->ChatModel().Inspect([&](app::ChatState const& s) { auto t = s.timelines.find(chatActive);
        if (t != s.timelines.end()) for (auto const& m : t->second.messages) if (m.id == last) went = m.attachments.size(); });
      stage = "attach-went"; ParityRequire(went == both.size());
      // Too big is refused here, before a byte leaves.
      auto big = chatOutput / L"grande.bin"; { std::ofstream out(big, std::ios::binary); out.seekp((33 << 20) - 1); out.put(0); }
      stage = "attach-big"; Attach({big}); ParityRequire(pendingFiles.empty() && uploadError.find(L"grande demais") != std::wstring::npos);
      // A file taken off while it climbs goes without a word.
      auto slow = chatOutput / L"cancelado.bin"; { std::ofstream out(slow, std::ios::binary); out.seekp((30 << 20) - 1); out.put(0); }
      stage = "attach-cancel"; uploadError.clear(); Attach({slow}); ParityRequire(pendingFiles.size() == 1);
      // Cancel while the job is queued/in flight. Waiting 150 ms races the
      // local server rejecting this synthetic zero-filled file first.
      logf("cancel step: %zu pending, error %d", pendingFiles.size(), uploadError.empty() ? 0 : 1);
      ParityRequire(pendingFiles.size() == 1); Forget(pendingFiles[0].key);
      co_await winrt::resume_after(std::chrono::milliseconds(1500)); co_await ui;
      logf("cancel step after: %zu pending, error %d", pendingFiles.size(), uploadError.empty() ? 0 : 1);
      ParityRequire(pendingFiles.empty() && uploadError.empty());
      std::error_code ec; std::filesystem::remove(big, ec); std::filesystem::remove(slow, ec);
    }
    if (!app::Environment(L"HARMONY_TEST_PICTURE").empty()) {
      // A pasted bitmap, from a package of our own: the owner's clipboard is
      // never touched by a test.
      stage = "paste";
      using namespace winrt::Windows::ApplicationModel::DataTransfer;
      auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(net::Wide(app::Environment(L"HARMONY_TEST_PICTURE"))); co_await ui;
      DataPackage package; package.SetBitmap(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file));
      PastePicture(package.GetView());
      for (int i = 0; i < 100 && (pendingFiles.empty() || Climbing()); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(pendingFiles.size() == 1 && pendingFiles[0].stored && pendingFiles[0].stored->name == "imagem.png" && pendingFiles[0].stored->mime == "image/png");
      ForgetAllUploads();
    }
    stage = "react";
    {
      // The panel from a message: it sits right-aligned on its row, inside the
      // list, and a pick toggles the reaction on (and a second one off).
      ScrollToEnd(); co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      auto index = messageItems.Size() - 1; auto id = unbox_value<int64_t>(messageItems.GetAt(index));
      app::ChatMessage m; std::string me;
      service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; auto t = s.timelines.find(chatActive);
        if (t != s.timelines.end()) for (auto const& x : t->second.messages) if (x.id == id) m = x; });
      auto row = messageList.ContainerFromIndex(index).try_as<FrameworkElement>(); ParityRequire(bool(row) && m.id == id);
      // As a click finds it: the row's action bar, shown because the pointer is there.
      auto bar = FindDesignPart(row, L"Actions"); ParityRequire(bool(bar)); bar.Visibility(Visibility::Visible); bar.UpdateLayout();
      OpenReactionPanel(m, bar); ParityRequire(bool(picker));
      co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
      {
        auto card = picker.Child().as<FrameworkElement>(); auto c = card.TransformToVisual(nullptr).TransformBounds({0, 0, float(card.ActualWidth()), float(card.ActualHeight())});
        auto list = messageList.TransformToVisual(nullptr).TransformBounds({0, 0, float(messageList.ActualWidth()), float(messageList.ActualHeight())});
        auto r = bar.TransformToVisual(nullptr).TransformBounds({0, 0, float(bar.ActualWidth()), float(bar.ActualHeight())});
        logf("reaction panel %.0f,%.0f %.0fx%.0f; bar right %.0f top %.0f; list %.0f..%.0f", c.X, c.Y, c.Width, c.Height, r.X + r.Width, r.Y, list.Y, list.Y + list.Height);
        // Reactions.tsx position(): right on the bar, 8 above it, no higher than the list's top.
        ParityRequire(std::abs((c.X + c.Width) - (r.X + r.Width)) < 1 && c.Y >= list.Y - 1 && std::abs(c.Y + c.Height - (r.Y - 8)) < 1);
      }
      auto const party = net::Narrow(L"\U0001F389");
      auto choose = pickerChoose; RememberRecent(party); ClosePicker(); choose(party);
      auto has = [&] { bool on = false; service->ChatModel().Inspect([&](app::ChatState const& s) { auto t = s.timelines.find(chatActive);
        if (t != s.timelines.end()) for (auto const& x : t->second.messages) if (x.id == id) for (auto const& r : x.reactions) if (r.emoji == party && r.Includes(me)) on = true; }); return on; };
      for (int i = 0; i < 50 && !has(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(has()); bar.Visibility(Visibility::Collapsed);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      co_await SaveDesignSurface(root, chatOutput, "nativo-reacao");
      React(id, party, false);
      for (int i = 0; i < 50 && has(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(!has());
    }
    stage = "responsive-image";
    {
      app::ChatMessage pictureMessage;
      service->ChatModel().Inspect([&](app::ChatState const& s) {
        for (auto const& m : s.timelines.at(chatActive).messages) for (auto const& f : m.attachments)
          if (f.mime.rfind("image/",0) == 0 && !f.missing) { pictureMessage = m; pictureMessage.attachments = {f}; }
      });
      ParityRequire(!pictureMessage.attachments.empty());
      pictureMessage.attachments.resize(1); pictureMessage.attachments[0].width = 1600; pictureMessage.attachments[0].height = 900;
      Border fixture; fixture.Width(350); fixture.Background(Brush(Surface)); fixture.Child(MessageExtras(pictureMessage,ReadOwner()));
      Grid::SetColumnSpan(fixture,3); root.Children().Append(fixture);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      stage = "responsive-image-laid-out";
      auto image = FindDesignPart(fixture,L"SingleMessageImage").as<Image>();
      // The regular chat keeps its small icon fixture, so unrelated reaction
      // geometry stays unchanged. Only this sizing fixture gets wide pixels.
      auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(net::Wide(app::Environment(L"HARMONY_TEST_LANDSCAPE"))); co_await ui;
      auto stream = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::Read); co_await ui;
      Media::Imaging::BitmapImage landscape; co_await landscape.SetSourceAsync(stream); co_await ui; image.Source(landscape);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      logf("responsive image narrow %.3f x %.3f, fixture %.3f",image.ActualWidth(),image.ActualHeight(),fixture.ActualWidth());
      ParityRequire(image.ActualWidth() <= 350.5 && image.ActualWidth() > 300);
      co_await SaveDesignSurface(fixture,chatOutput,"nativo-imagem-estreita-sintetica");
      fixture.Width(900);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      logf("responsive image wide %.3f x %.3f, fixture %.3f",image.ActualWidth(),image.ActualHeight(),fixture.ActualWidth());
      ParityRequire(image.ActualWidth() > 676 && image.ActualWidth() <= 900.5 && image.ActualHeight() <= 384.5);
      co_await SaveDesignSurface(fixture,chatOutput,"nativo-imagem-larga-sintetica");
      root.Children().RemoveAtEnd();
    }
    stage = "conversation-menu";
    {
      app::ChatConversation c;
      service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& x : s.conversations) if (x.id == chatActive) c = x; });
      auto menu = ConversationMenu(c);
      ParityRequire(menu.Items().GetAt(0).as<MenuFlyoutItem>().Text() == L"Silenciar");
      menu.ShowAt(headerTitle);
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
        if (auto content = popup.Child().try_as<FrameworkElement>()) co_await SaveDesignSurface(content,chatOutput,"nativo-menu-canal");
      menu.Hide();
      auto dm = c; dm.kind = "dm"; dm.muted = true;
      auto directMenu = ConversationMenu(dm);
      ParityRequire(directMenu.Items().Size() == 2 && directMenu.Items().GetAt(0).as<MenuFlyoutItem>().Text() == L"Voltar a avisar" &&
                 directMenu.Items().GetAt(1).as<MenuFlyoutItem>().Text() == L"Fechar conversa");
    }
    stage = "search";
    {
      OpenSearch();
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      searchBox.Text(L"jogar");
      for (int i = 0; i < 70 && searchResults.Children().Size() == 0; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(searchResults.Children().Size() > 0);
      co_await SaveDesignSurface(searchDialog,chatOutput,"nativo-busca");
      auto hit = searchResults.Children().GetAt(0).as<Button>();
      Automation::Peers::ButtonAutomationPeer(hit).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      ParityRequire(!searchOpen && pendingJump == 0);
      auto original = chatActive;
      OpenSearch();
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      searchBox.Text(L"me chama");
      for (int i = 0; i < 70 && searchResults.Children().Size() == 0; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(searchResults.Children().Size() > 0);
      auto dmHit = searchResults.Children().GetAt(0).as<Button>();
      Automation::Peers::ButtonAutomationPeer(dmHit).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
      for (int i = 0; i < 70 && (pendingJump || chatTab != "dm"); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(chatTab == "dm" && chatActive != original && !pendingJump);
      chatTab = "server"; OpenConversation(original);
      for (int i = 0; i < 70 && messageItems.Size() == 0; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ScrollToEnd(); stickToBottom = true;
    }
    stage = "call-history";
    {
      Grid fixture; fixture.Background(Brush(Background)); fixture.Width(700); fixture.Padding(Thickness{16,16,16,16});
      StackPanel notes;
      for (bool mine : {true,false}) for (bool rejected : {false,true}) {
        RowData r; r.me = mine ? "alice" : "bob"; r.m.author.username = "alice"; r.m.author.displayName = "Alice";
        r.m.kind = "call"; r.m.body = rejected ? "rejected" : "missed"; r.m.createdAt = "2026-10-18T12:00:00Z";
        notes.Children().Append(MessageRow(r));
      }
      for (bool mine : {true,false}) {
        RowData r; r.me = mine ? "alice" : "bob"; r.m.author.username = "alice"; r.m.author.displayName = "Alice";
        r.m.kind = "pin"; r.m.body = "fixou uma mensagem."; r.m.createdAt = "2026-10-18T12:00:00Z";
        notes.Children().Append(MessageRow(r));
      }
      fixture.Children().Append(notes); Grid::SetColumnSpan(fixture,3); root.Children().Append(fixture);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      co_await SaveDesignSurface(fixture,chatOutput,"nativo-historico-chamadas-sintetico"); root.Children().RemoveAtEnd();
    }
    stage = "edit";
    {
      composer.Text(L"rascunho preservado"); EditLastOwn(); ParityRequire(editingId != 0);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      ParityRequire(inlineEditor && composer.Text() == L"rascunho preservado");
      co_await SaveDesignSurface(root, chatOutput, "nativo-edicao-inline");
      inlineEditor.Text(L"cancelar esta altera\u00e7\u00e3o"); CancelEdit();
      ParityRequire(!editingId && composer.Text() == L"rascunho preservado");
      EditLastOwn();
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      auto changed = L"editada pelo nativo " + std::to_wstring(GetTickCount64() % 100000);
      inlineEditor.Text(changed); SaveInlineEdit(); ParityRequire(editingId != 0 && editSent);
      for (int i = 0; i < 50 && editingId; ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(editingId == 0 && composer.Text() == L"rascunho preservado");
      co_await SaveDesignSurface(root, chatOutput, "nativo-edicao-salva");
      composer.Text(L"");
    }
    stage = "mentions";
    {
      using winrt::Windows::System::VirtualKey;
      composer.Focus(FocusState::Programmatic);
      composer.Text(L"oi @br"); composer.SelectionStart(6);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      ParityRequire(bool(mentionPopup) && !mentionMatches.empty() && mentionMatches[0].username == "bruna");
      // "@" alone offers @todos first, as the web does.
      composer.Text(L"oi @"); composer.SelectionStart(4);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      ParityRequire(bool(mentionPopup) && mentionMatches.size() >= 2 && mentionMatches[0].username == "todos");
      ParityRequire(MentionKey(VirtualKey::Down) && mentionHighlight == 1);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      {
        auto card = mentionPopup.Child().as<FrameworkElement>(); auto area = FindAncestor(composer, L"ComposerArea");
        auto c = card.TransformToVisual(nullptr).TransformPoint({0, 0}); auto a = area.TransformToVisual(nullptr).TransformPoint({0, 0});
        logf("mention panel %.0f,%.0f %.0fx%.0f; area %.0f,%.0f", c.X, c.Y, card.ActualWidth(), card.ActualHeight(), a.X, a.Y);
        ParityRequire(std::abs(c.X - (a.X + 8)) < 1 && std::abs(c.Y + card.ActualHeight() - (a.Y - 8)) < 1 && card.ActualWidth() == 348);
        co_await SaveDesignSurface(card, chatOutput, "nativo-mencoes");
      }
      auto chosen = mentionMatches[mentionHighlight].username;
      auto countBeforeMention = messageItems.Size();
      // The whole preview handler must consume autocomplete before it sends.
      ParityRequire(ComposerKey(VirtualKey::Enter, false));
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      ParityRequire(!mentionPopup && !sendWaiting && messageItems.Size() == countBeforeMention);
      ParityRequire(std::wstring(composer.Text()) == L"oi @" + net::Wide(chosen) + L" ");
      composer.Text(L"oi @ninguemaqui"); composer.SelectionStart(15);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      ParityRequire(bool(mentionPopup) && mentionMatches.empty());
      ParityRequire(MentionKey(VirtualKey::Escape) && !mentionPopup);
      composer.Text(L"");
    }
    stage = "lightbox";
    {
      // A picture opens over the app, Esc brings the conversation back, and
      // "Baixar" saves the same bytes twice under two names.
      app::ChatMessage withPicture; app::ChatAttachment picture;
      service->ChatModel().Inspect([&](app::ChatState const& s) { auto t = s.timelines.find(chatActive);
        if (t != s.timelines.end()) for (auto const& m : t->second.messages) for (auto const& a : m.attachments) if (!a.missing && a.mime.rfind("image/", 0) == 0) { withPicture = m; picture = a; } });
      ParityRequire(!picture.url.empty());
      OpenLightbox(picture, withPicture); ParityRequire(bool(lightbox));
      co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
      co_await SaveDesignSurface(root, chatOutput, "nativo-imagem-grande");
      if (picture.width > 0 && picture.width <= 800 && picture.height <= 400) {
        // Small enough for its own size: on the web it sits at exactly that (Messages.tsx Lightbox).
        Image big=FindDesignPart(lightbox,L"LightboxImage").try_as<Image>();
        ParityRequire(big && std::abs(big.ActualWidth() - picture.width) < 1 && std::abs(big.ActualHeight() - picture.height) < 1);
      }
      Download(picture); lastDownload.clear();
      for (int i = 0; i < 50 && lastDownload.empty(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      auto first = lastDownload; Download(picture); lastDownload.clear();
      for (int i = 0; i < 50 && lastDownload.empty(); ++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      auto second = lastDownload;
      std::error_code ec;
      ParityRequire(!first.empty() && !second.empty() && first != second && std::filesystem::file_size(first, ec) == uintmax_t(picture.size) && std::filesystem::file_size(second, ec) == uintmax_t(picture.size));
      lightbox.Focus(FocusState::Programmatic); CloseLightbox(); ParityRequire(!lightbox);
    }
    stage = "file-names";
    ParityRequire(SafeFileName(L"CON.txt") == L"_CON.txt" && SafeFileName(L"nul") == L"_nul" && SafeFileName(L"com1 .log") == L"_com1 .log");
    ParityRequire(SafeFileName(L"notas.") == L"notas" && SafeFileName(L"a/b\\c:d") == L"a_b_c_d" && SafeFileName(L"...") == L"arquivo");
    ParityRequire(SafeFileName(L"console.txt") == L"console.txt" && SafeFileName(L"foto de ontem.png") == L"foto de ontem.png");
    stage = "tabs";
    {
      // "Mensagens" opens a DM when the open conversation is a channel, and
      // "Servidor" brings a channel back (the owner's report, 17/10/2026).
      auto channel = chatActive;
      auto kindOf = [&](std::string const& id) { std::string k; service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.id == id) k = c.kind; }); return k; };
      auto press = [&](std::wstring const& name) { auto t = FindDesignPart(root, L"TabsIsland"); for (auto e : t.as<Border>().Child().as<Grid>().Children()) if (Automation::AutomationProperties::GetName(e.as<FrameworkElement>()) == name) return e.as<FrameworkElement>(); return FrameworkElement(nullptr); };
      ParityRequire(kindOf(channel) != "dm" && bool(press(L"Mensagens")));
      chatTab = "dm"; channelsSignature.clear(); FollowTab(); Render();
      co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
      ParityRequire(chatActive != channel && kindOf(chatActive) == "dm");
      if (!app::Environment(L"HARMONY_PARITY5").empty()) {
        ParityRequire(messageItems.Size()>0 && messageItems.Size()<5);
        auto last=messageList.ContainerFromIndex(messageItems.Size()-1).as<FrameworkElement>();
        auto bottom=last.TransformToVisual(root).TransformPoint({0,float(last.ActualHeight())}).Y;
        auto composerTop=FindDesignPart(root,L"ComposerArea").TransformToVisual(root).TransformPoint({0,0}).Y;
        logf("short history: last row bottom %.2f, composer top %.2f",bottom,composerTop);
        ParityRequire(composerTop-bottom>=0 && composerTop-bottom<=16);
        co_await SaveDesignSurface(root,chatOutput,"round5-short-history");
      }
      auto avatar = FindDesignPart(channelList, L"DirectAvatar");
      auto presence = FindDesignPart(channelList, L"DirectPresence");
      ParityRequire(avatar && presence && avatar.ActualWidth() == 32 && avatar.ActualHeight() == 32 && presence.ActualWidth() == 10 && presence.ActualHeight() == 10);
      co_await SaveDesignSurface(root, chatOutput, "nativo-aba-mensagens");
      auto dotPixels = std::make_shared<DesignPixels>(); co_await ReadDesignRegion(root, presence, dotPixels);
      auto green = [&](int x, int y) {
        auto i = size_t(y * dotPixels->width + x) * 4;
        return dotPixels->bytes[i + 1] > 100 && dotPixels->bytes[i + 1] > dotPixels->bytes[i + 2] + 40 && dotPixels->bytes[i + 1] > dotPixels->bytes[i] + 20;
      };
      int w = dotPixels->width, h = dotPixels->height;
      bool cornersClear = !green(0, 0) && !green(w - 1, 0) && !green(0, h - 1) && !green(w - 1, h - 1);
      bool centreFilled = green(w / 2, h / 2);
      bool sidesFilled = green(w / 2, 1) && green(w / 2, h - 2) && green(1, h / 2) && green(w - 2, h / 2);
      std::ofstream(chatOutput / L"direct-presence.json") << "{\"width\":" << w << ",\"height\":" << h
        << ",\"corners_clear\":" << (cornersClear ? "true" : "false") << ",\"centre_filled\":" << (centreFilled ? "true" : "false")
        << ",\"sides_filled\":" << (sidesFilled ? "true" : "false") << '}';
      logf("direct presence pixels: corners clear=%d centre filled=%d sides filled=%d", cornersClear, centreFilled, sidesFilled);
      ParityRequire(cornersClear && centreFilled && sidesFilled);
      stage = "tabs-back";
      chatTab = "server"; channelsSignature.clear(); FollowTab(); Render();
      co_await winrt::resume_after(std::chrono::milliseconds(800)); co_await ui;
      ParityRequire(chatActive == channel);
    }
    stage = "wheel";
    if (realInput) {
      stage = "wheel-not-foreground";
      co_await ChatAuditForeground("wheel");
      stage = "wheel";
      HookScroll(); ParityRequire(bool(messageScroll));
      UINT lines = 0; ParityRequire(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0));
      auto before = messageScroll.VerticalOffset();
      double step = lines == WHEEL_PAGESCROLL ? messageScroll.ViewportHeight() : lines * 24.0;
      if (step <= 0 || messageScroll.ScrollableHeight() <= step || before <= step) {
        logf("wheel failed: need step > 0, ScrollableHeight > step and before > step; step %.2f, height %.2f, before %.2f", step, messageScroll.ScrollableHeight(), before);
        ParityRequire(false);
      }
      double expected = before - step;
      POINT original{}; ParityRequire(GetCursorPos(&original));
      struct RestorePointer { POINT point; ~RestorePointer() { AuditSetCursorPos(point.x, point.y); } } restore{original};
      auto at = messageScroll.TransformToVisual(nullptr).TransformPoint({float(messageScroll.ActualWidth() / 2), float(messageScroll.ActualHeight() / 2)});
      double scale = GetDpiForWindow(hwnd) / 96.0;
      POINT point{LONG(at.X * scale), LONG(at.Y * scale)}; ClientToScreen(hwnd, &point);
      INPUT move{}; move.type = INPUT_MOUSE; move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      move.mi.dx = LONG((point.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535.0 / (GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
      move.mi.dy = LONG((point.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535.0 / (GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
      ParityRequire(AuditSendInput(1, &move, sizeof(move)) == 1);
      co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui;
      INPUT wheel{}; wheel.type = INPUT_MOUSE; wheel.mi.dwFlags = MOUSEEVENTF_WHEEL; wheel.mi.mouseData = WHEEL_DELTA;
      before = messageScroll.VerticalOffset(); expected = before - step;
      if (before <= step) { logf("wheel failed: first notch needs before > step; before %.2f, step %.2f", before, step); ParityRequire(false); }
      auto firstSize = messageItems.Size();
      ParityRequire(AuditSendInput(1, &wheel, sizeof(wheel)) == 1);
      co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui;
      auto early = messageScroll.VerticalOffset();
      bool firstPaged = messageItems.Size() != firstSize;
      if (firstPaged) logf("wheel: first notch paged; cannot verify the full step from absolute offsets");
      co_await SaveDesignSurface(root, chatOutput, "nativo-rolagem");
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      auto late = messageScroll.VerticalOffset();
      logf("wheel: system lines %u, 24 DIP per line, step %.2f, before %.2f, expected %.2f, at 50ms %.2f, settled %.2f", lines, step, before, expected, early, late);
      std::ofstream(chatOutput / L"wheel.json") << "{\"system_lines\":" << lines << ",\"line_dip\":24,\"step_dip\":" << step
        << ",\"before\":" << before << ",\"expected\":" << expected << ",\"at_50ms\":" << early << ",\"settled\":" << late << '}';
      // A page changes the offset origin, so this first-step proof needs a stable collection.
      if (firstPaged || messageItems.Size() != firstSize) {
        logf("wheel failed: first notch paged; retry with a stable collection"); ParityRequire(false);
      }
      if (std::abs(early - expected) >= 1 || std::abs(late - early) >= 1) {
        logf("wheel failed: first notch must move up by step %.2f within 50 ms and stay there", step); ParityRequire(false);
      }
      // The recycled list survives the jumps: notch by notch to the top and
      // back to the end, every notch lands within 50 ms, and the first and
      // last messages are realized when their edge is reached.
      auto panel = messageList.ItemsPanelRoot().try_as<ItemsStackPanel>(); ParityRequire(bool(panel));
      auto notches = [&](int delta, int& moved, int& slow) -> winrt::Windows::Foundation::IAsyncAction {
        double distance = delta > 0 ? messageScroll.VerticalOffset() : messageScroll.ScrollableHeight() - messageScroll.VerticalOffset();
        int budget = int(std::ceil(distance / step)) + 4;
        for (int i = 0; i < budget; ++i) {
          double from = messageScroll.VerticalOffset(), limit = messageScroll.ScrollableHeight();
          if ((delta > 0 && from <= 0) || (delta < 0 && from >= limit)) co_return;
          double to = std::clamp(from - delta / double(WHEEL_DELTA) * step, 0.0, limit);
          auto count = messageItems.Size();
          INPUT n{}; n.type = INPUT_MOUSE; n.mi.dwFlags = MOUSEEVENTF_WHEEL; n.mi.mouseData = DWORD(delta); ParityRequire(AuditSendInput(1, &n, sizeof(n)) == 1);
          co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui;
          // KeepItemsInView shifts absolute offsets when older messages are inserted.
          if (messageItems.Size() != count) { logf("wheel trip: %s notch %d paged", delta > 0 ? "up" : "down", i + 1); continue; }
          double after = messageScroll.VerticalOffset();
          if ((delta > 0 && after < from) || (delta < 0 && after > from)) ++moved;
          if (std::abs(after - to) >= 1) { ++slow; logf("wheel trip: %s notch %d off target at 50 ms; from %.2f, target %.2f, observed %.2f", delta > 0 ? "up" : "down", i + 1, from, to, after); }
        }
        double offset = messageScroll.VerticalOffset(), limit = messageScroll.ScrollableHeight();
        if ((delta > 0 && offset > 0) || (delta < 0 && offset < limit)) {
          logf("wheel failed: %s trip did not reach edge within %d notches; offset %.2f, limit %.2f", delta > 0 ? "up" : "down", budget, offset, limit);
          ParityRequire(false);
        }
      };
      int up = 0, upSlow = 0, down = 0, downSlow = 0;
      co_await notches(WHEEL_DELTA, up, upSlow);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      int firstShown = panel.FirstVisibleIndex(); bool topRealized = bool(messageList.ContainerFromIndex(0));
      co_await notches(-int(WHEEL_DELTA), down, downSlow);
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      int lastShown = panel.LastVisibleIndex(), size = int(messageItems.Size());
      bool endRealized = bool(messageList.ContainerFromIndex(uint32_t(size - 1)));
      logf("wheel trip: %d observed moves up (%d off target at 50 ms), first visible %d, %d observed moves down (%d off), last visible %d of %d",
           up, upSlow, firstShown, down, downSlow, lastShown, size);
      if (up == 0 || down == 0) { logf("wheel failed: trip needs observed movement in both directions; up %d, down %d", up, down); ParityRequire(false); }
      if (upSlow != 0 || downSlow != 0 || firstShown != 0 || !topRealized || lastShown != size - 1 || !endRealized)
        logf("wheel failed: trip has off-target notches or missing edge messages");
      // A gallery run only wants the pictures after this stage; the late notch
      // is logged above and still fails every ordinary run.
      if (app::Environment(L"HARMONY_AUDIT_GALLERY") != "1")
        ParityRequire(upSlow == 0 && downSlow == 0 && firstShown == 0 && topRealized && lastShown == size - 1 && endRealized);
    } else logf("wheel: skipped");
    stage = "placeholder";
    {
      // Run after attachment tests, and suppress Draft/Typing/mentions while
      // probing: a diagnostic string must never become a conversation draft.
      auto savedText = composer.Text(), savedHint = composer.PlaceholderText();
      auto savedForeground = composer.Foreground(); bool savedLoading = composerLoading;
      auto savedFocus = Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<Control>();
      int start = composer.SelectionStart(), length = composer.SelectionLength();
      struct Restore { std::function<void()> action; ~Restore() { action(); } } restore{[&, this] {
        composer.Text(savedText); composer.PlaceholderText(savedHint); composer.Foreground(savedForeground);
        for (auto key : {L"TextControlForeground", L"TextControlForegroundPointerOver", L"TextControlForegroundFocused"})
          composer.Resources().Lookup(box_value(key)).as<SolidColorBrush>().Color(savedForeground.as<SolidColorBrush>().Color());
        composer.Select(start, length); composerLoading = savedLoading;
        if (savedFocus) savedFocus.Focus(FocusState::Programmatic);
        UpdateSend(); CloseMentions();
      }};
      composerLoading = true; composer.Foreground(Brush(Muted)); CloseMentions();
      for (auto key : {L"TextControlForeground", L"TextControlForegroundPointerOver", L"TextControlForegroundFocused"})
        composer.Resources().Lookup(box_value(key)).as<SolidColorBrush>().Color(Brush(Muted).Color());
      auto surface = FindDesignPart(root, L"ComposerInputSurface"); ParityRequire(bool(surface));
      std::ofstream alignmentFile(chatOutput / L"composer-alignment.json"); alignmentFile << '['; bool first = true, alignmentPassed = true;
      for (bool multiline : {false, true}) for (bool focused : {true, false}) {
        std::wstring sample = savedHint.c_str();
        if (multiline) sample += L"\n" + sample;
        auto label = std::string(multiline ? "multiline" : "single") + (focused ? "-focused" : "-unfocused");
        composer.Text(L""); composer.PlaceholderText(sample);
        if (focused) ParityRequire(composer.Focus(FocusState::Programmatic));
        else ParityRequire(FindDesignPart(root, L"ComposerAttach").as<Button>().Focus(FocusState::Programmatic));
        co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
        root.UpdateLayout();
        auto emptyCaret = FindDesignCaret(composer);
        ParityRequire(!focused || bool(emptyCaret));
        auto caretAt = emptyCaret ? emptyCaret.TransformToVisual(surface).TransformPoint({0, 0}) : winrt::Windows::Foundation::Point{};
        auto empty = std::make_shared<DesignPixels>(); co_await ReadDesignPixels(surface, empty);
        co_await SaveDesignSurface(surface, chatOutput, "composer-hint-" + label);
        // Put both carets at the start: only their measured column may be
        // excluded, not an estimated column from a zero character rectangle.
        composer.Text(sample); composer.Select(0, 0);
        co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
        root.UpdateLayout();
        auto typedCaret = FindDesignCaret(composer);
        ParityRequire(!focused || bool(typedCaret));
        auto typedAt = typedCaret ? typedCaret.TransformToVisual(surface).TransformPoint({0, 0}) : winrt::Windows::Foundation::Point{};
        auto typed = std::make_shared<DesignPixels>(); co_await ReadDesignPixels(surface, typed);
        co_await SaveDesignSurface(surface, chatOutput, "composer-text-" + label);
        ParityRequire(empty->width == typed->width && empty->height == typed->height);
        double sx = empty->width / surface.ActualWidth();
        int caretLeft = focused ? int(std::floor(caretAt.X * sx)) : -1;
        int caretRight = focused ? int(std::ceil((caretAt.X + emptyCaret.ActualWidth()) * sx)) : -1;
        size_t ink = 0, different = 0;
        int left = empty->width, top = empty->height, right = -1, bottom = -1;
        for (int y = 0; y < empty->height; ++y) for (int x = 0; x < empty->width; ++x) {
          if (focused && x >= caretLeft && x < caretRight) continue;
          bool a = DesignInk(*empty, x, y, Surface), b = DesignInk(*typed, x, y, Surface);
          different += a != b;
          if (a) { ++ink; left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y); }
        }
        // Measure the last line's visible ink against the actual tool centre.
        // This is an optical ink measurement, not an invalid document caret
        // rectangle treated as a coordinate inside the TextBox.
        int lastLineTop = bottom;
        for (int y = bottom; y >= 0; --y) {
          bool rowInk = false;
          for (int x = 0; x < typed->width; ++x) if (!(focused && x >= caretLeft && x < caretRight) && DesignInk(*typed, x, y, Surface)) { rowInk = true; break; }
          if (!rowInk) break;
          lastLineTop = y;
        }
        auto tool = FindDesignPart(root, L"ComposerAttach"); ParityRequire(bool(tool));
        auto toolAt = tool.TransformToVisual(surface).TransformPoint({0, float(tool.ActualHeight() / 2)});
        double sy = typed->height / surface.ActualHeight();
        double inkCentreDelta = (lastLineTop + bottom + 1) / (2 * sy) - toolAt.Y;
        logf("composer %s: visual caret valid=%d at %.2f,%.2f, ink %zu at %d,%d..%d,%d, mismatched pixels %zu",
             label.c_str(), focused && bool(emptyCaret), caretAt.X, caretAt.Y, ink, left, top, right, bottom, different);
        if (!first) alignmentFile << ','; first = false;
        alignmentFile << "{\"case\":\"" << label << "\",\"ink\":" << ink << ",\"mismatched_pixels\":" << different
                     << ",\"ink_bounds\":[" << left << ',' << top << ',' << right << ',' << bottom << ']'
                     << ",\"caret_valid\":" << (focused ? "true" : "false") << ",\"caret_x\":";
        if (focused) alignmentFile << caretAt.X; else alignmentFile << "null";
        alignmentFile << ",\"caret_y\":"; if (focused) alignmentFile << caretAt.Y; else alignmentFile << "null";
        alignmentFile << ",\"caret_height\":"; if (focused) alignmentFile << emptyCaret.ActualHeight(); else alignmentFile << "null";
        alignmentFile << ",\"excluded_columns\":[" << caretLeft << ',' << caretRight << ']'
                      << ",\"last_line_ink_top\":" << lastLineTop << ",\"last_line_ink_bottom\":" << bottom
                      << ",\"last_line_tool_ink_centre_delta\":";
        if (ink > 20) alignmentFile << inkCentreDelta; else alignmentFile << "null";
        alignmentFile << '}';
        // The hint is a TextBlock and the typed text a RichEdit: the two
        // engines place one glyph a fraction of a pixel apart (measured: 10
        // pixels inside one letter). A real 1 px shift moves hundreds.
        alignmentPassed = alignmentPassed && ink > 20 && different * 50 <= ink
          && (!focused || (std::abs(typedAt.X - caretAt.X) < .01 && caretRight - caretLeft <= int(std::ceil(3 * sx))));
      }
      alignmentFile << ']'; alignmentFile.close(); ParityRequire(alignmentPassed);
    }
    stage = "profile";
    {
      ChatAuditPosition();
      auto person=FindDesignPart(root,L"Person-bruna"); ParityRequire(bool(person));
      OpenContextMenu(person, person.ContextFlyout(), winrt::Windows::Foundation::Point{float(person.ActualWidth()/2), float(person.ActualHeight()/2)});
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
        if (auto content=popup.Child().try_as<FrameworkElement>()) co_await SaveDesignSurface(content,chatOutput,"nativo-menu-pessoa");
      auto menu=person.ContextFlyout().as<MenuFlyout>(); ParityRequire(menu.Items().GetAt(0).as<MenuFlyoutItem>().Text()==L"Ver perfil");
      ParityRequire(menu.IsOpen()); menu.Hide();
      // Closing a flyout can outlive Hide() while its popup animates. Also
      // reacquire the row: live presence may have rebuilt the people list.
      for(int i=0;i<20 && Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()).Size();++i) {
        co_await winrt::resume_after(std::chrono::milliseconds(50)); co_await ui;
      }
      person=FindDesignPart(root,L"Person-bruna"); ParityRequire(bool(person));
      OpenProfile(app::ChatPerson{"bruna"}); // same action already shared by BindPerson
      co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
      ParityRequire(bool(profileOverlay) && bool(FindDesignPart(root,L"ProfileNickname")));
      auto profile=FindDesignPart(root,L"PersonProfile").as<Border>();
      auto inset=profile.Padding(); auto corners=profile.CornerRadius();
      ParityRequire(inset.Left==20 && inset.Top==20 && inset.Right==20 && inset.Bottom==20 && corners.TopLeft==16 && corners.BottomRight==16);
      co_await SaveDesignSurface(root,chatOutput,"nativo-perfil");
      auto nick=FindDesignPart(root,L"ProfileNickname").as<TextBox>(); nick.Text(L"Amiga de teste");
      auto save=FindDesignPart(root,L"SaveNickname").as<Button>();
      Automation::Peers::ButtonAutomationPeer(save).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
      for (int i=0;i<100 && ResolvePerson(app::ChatPerson{"bruna"}).nickname != "Amiga de teste";++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(ResolvePerson(app::ChatPerson{"bruna"}).nickname == "Amiga de teste");
      co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
      co_await SaveDesignSurface(root,chatOutput,"nativo-perfil-apelido");
      CloseProfile(); ParityRequire(!profileOverlay && bool(FindDesignPart(root,L"Composer")));
      stage="profile-join";
      bool joining=false;
      for (auto const& action:PersonActions(ResolvePerson(app::ChatPerson{"bruna"}))) if (action.label.starts_with(L"Entrar em ")) { action.pick(); joining=true; break; }
      ParityRequire(joining);
      for (int i=0;i<150 && (ReadState().voice.room.empty() || ReadState().voice.connecting);++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      ParityRequire(showingCall && !ReadState().voice.room.empty() && !ReadState().voice.connecting);
      co_await SaveDesignSurface(root,chatOutput,"nativo-perfil-entrar-sala");
      auto voicePerson=FindDesignPart(root,L"VoicePerson").as<Button>();
      Automation::Peers::ButtonAutomationPeer(voicePerson).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
      co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
      ParityRequire(bool(profileOverlay)); CloseProfile();
      showingCall=false; service->LeaveVoice();
      for (int i=0;i<50 && !ReadState().voice.room.empty();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
      Render();
    }
    stage = "settings";
    {
      // One card; a tab changes only its content, the tabs and the X stay.
      auto invoke = [](FrameworkElement e) { Automation::Peers::ButtonAutomationPeer(e.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke(); };
      auto keptComposer=composer; auto keptDraft=composer.Text();
      SettingsPage(); co_await winrt::resume_after(std::chrono::milliseconds(600)); co_await ui;
      for (auto [tab, part, shot] : {std::tuple{L"SettingsTab-app", L"Startup", "nativo-ajustes-aplicativo"}, std::tuple{L"SettingsTab-av", L"VoiceSettings", "nativo-ajustes-voz"},
                                     std::tuple{L"SettingsTab-conta", L"SettingsTabs", "nativo-ajustes-conta"}, std::tuple{L"SettingsTab-diag", L"CopyLog", "nativo-ajustes-diagnostico"}}) {
        auto button = FindDesignPart(root, tab); ParityRequire(bool(button)); invoke(button);
        co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
        ParityRequire(bool(FindDesignPart(root, part)) && bool(FindDesignPart(root, L"SettingsTabs")) && bool(FindDesignPart(root, L"CloseSettings")));
        auto modal=FindDesignPart(root,L"SettingsModal");
        auto inset=modal.as<Border>().Padding();
        ParityRequire(inset.Left==24 && inset.Top==24 && inset.Right==24 && inset.Bottom==24);
        ParityRequire(modal.ActualWidth() <= 880 && modal.ActualHeight() <= 720 && bool(FindDesignPart(root,L"Composer")));
        co_await SaveDesignSurface(root, chatOutput, shot);
        if (std::wstring(tab) == L"SettingsTab-conta") {
          stage="account-photo";
          auto profile=FindDesignPart(root,L"EditProfile").as<Border>(); profile.StartBringIntoView();
          co_await winrt::resume_after(std::chrono::milliseconds(300)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-perfil-antes");
          auto photoPick=FindDesignPart(root,L"ChangePhoto").as<Button>(); invoke(photoPick);
          for(int i=0;i<100 && !photoPick.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(FindDesignPart(root,L"PhotoStatus").as<TextBlock>().Text()==L"Foto atualizada.");
          co_await winrt::resume_after(std::chrono::milliseconds(500)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-perfil-foto");
          auto photoRemove=FindDesignPart(root,L"RemovePhoto").as<Button>(); ParityRequire(photoRemove.IsEnabled()); invoke(photoRemove);
          for(int i=0;i<100 && !photoPick.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(!photoRemove.IsEnabled());
          co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
          stage="factor-setup";
          for(int i=0;i<100 && !FindDesignPart(root,L"FactorToggle");++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          invoke(FindDesignPart(root,L"FactorToggle"));
          auto factorPassword=FindDesignPart(root,L"FactorPassword").as<PasswordBox>(); factorPassword.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD")));
          co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
          securityView->region.StartBringIntoView(); co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-2fa-escolha"); invoke(FindDesignPart(root,L"FactorSetup"));
          for(int i=0;i<100 && !FindDesignPart(root,L"FactorEnableTotp");++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(!securityView->setup["secret"].Str().empty()); auto proof=securityView->proof;
          auditSecrets.push_back(securityView->setup["secret"].Str()); auditSecrets.push_back(securityView->setup["uri"].Str());
          securityView->region.StartBringIntoView(); co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-2fa-chave"); invoke(FindDesignPart(root,L"FactorToggle"));
          ParityRequire(factorPassword.Password().empty() && proof.Text().empty() && securityView->setup.IsNull());
          stage="email-settings";
          invoke(FindDesignPart(root,L"EmailToggle"));
          auto address=FindDesignPart(root,L"EmailAddress").as<TextBox>(); address.Text(L"changed-ui@example.invalid");
          auto emailPassword=FindDesignPart(root,L"EmailPassword").as<PasswordBox>(); emailPassword.Password(L"invalid fixture");
          co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
          invoke(FindDesignPart(root,L"EmailSend"));
          for(int i=0;i<100 && !emailView->region.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(FindDesignPart(root,L"EmailStatus").as<TextBlock>().Text()==L"Usuário ou senha não conferem.");
          emailView->region.StartBringIntoView(); co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-email-troca"); invoke(FindDesignPart(root,L"EmailToggle"));
          ParityRequire(address.Text().empty() && emailPassword.Password().empty());
          stage="account-sessions";
          auto list=FindDesignPart(root,L"AccountSessions").as<StackPanel>();
          for (int i=0;i<100 && list.Children().Size()<4;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(list.Children().Size()>=4);
          list.StartBringIntoView(); co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-aparelhos");
          auto count=list.Children().Size(); invoke(FindDesignPart(root,L"RevokeSession"));
          for (int i=0;i<100 && list.Children().Size()!=count-1;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(list.Children().Size()==count-1);
          invoke(FindDesignPart(root,L"RevokeOtherSessions"));
          for (int i=0;i<100 && list.Children().Size()!=1;++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(list.Children().Size()==1);
          stage="account-change";
          auto expand=FindDesignPart(root,L"ChangePassword").as<Expander>(); expand.IsExpanded(true);
          co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
          expand.StartBringIntoView(); co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
          auto current=FindDesignPart(root,L"CurrentPassword").as<PasswordBox>();
          auto next=FindDesignPart(root,L"NewPassword").as<PasswordBox>();
          wchar_t fixture[512]{}; auto length=GetEnvironmentVariableW(L"HARMONY_TEST_PASSWORD",fixture,512); ParityRequire(length>0 && length<512);
          next.Password(std::wstring(fixture)+L"-changed"); current.Password(L"invalid fixture password");
          co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui;
          auto save=FindDesignPart(root,L"SavePassword").as<Button>(); ParityRequire(save.IsEnabled()); invoke(save);
          for (int i=0;i<100 && !current.IsEnabled();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          auto passwordStatus=FindDesignPart(root,L"PasswordStatus").as<TextBlock>();
          ParityRequire(current.IsEnabled() && expand.IsExpanded() && passwordStatus.Text()==L"Usuário ou senha não conferem.");
          current.Password(fixture); SecureZeroMemory(fixture,sizeof fixture);
          co_await SaveDesignSurface(root,chatOutput,"nativo-conta-senha"); invoke(save);
          for (int i=0;i<100 && expand.IsExpanded();++i) { co_await winrt::resume_after(std::chrono::milliseconds(100)); co_await ui; }
          ParityRequire(!expand.IsExpanded() && current.Password().empty() && next.Password().empty());
          ParityRequire(passwordStatus.Text()==L"Senha trocada. Os outros aparelhos saíram da conta.");
          co_await winrt::resume_after(std::chrono::milliseconds(350)); co_await ui;
          co_await SaveDesignSurface(root,chatOutput,"nativo-conta-senha-salva");
          stage="settings";
          invoke(FindDesignPart(root, L"AskLogout"));
          co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
          ParityRequire(bool(logoutDialog) && ReadState().signedIn);
          auto cancel = FindDesignPart(logoutDialog, L"CloseButton").as<Button>();
          ParityRequire(cancel.ActualHeight() == 44 && DesignColor(cancel.Foreground()) == Foreground);
          co_await SaveDesignSurface(logoutDialog, chatOutput, "nativo-confirmar-saida");
          logoutDialog.Hide();
          co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
          ParityRequire(!logoutDialog && ReadState().signedIn && settings);
        }
      }
      invoke(FindDesignPart(root, L"CloseSettings")); co_await winrt::resume_after(std::chrono::milliseconds(600)); co_await ui;
      ParityRequire(!settings && composer == keptComposer && composer.Text() == keptDraft && bool(FindDesignPart(root, L"Composer")));
    }
    stage="private-diagnostics";
    for(auto const& line:LogLines()) {
      for(auto const& secret:auditSecrets) if(!secret.empty()) ParityRequire(line.find(secret)==std::string::npos);
      ParityRequire(line.find("@example.invalid")==std::string::npos && line.find("harmony_session=")==std::string::npos);
    }
    passed = true; stage = "done";
  } catch (AuditStop const&) { passed = true; }
  catch (hresult_error const& e) { logf("chat audit failed at %s: %ld", stage.c_str(), e.code().value); }
  catch (...) { logf("chat audit failed at %s", stage.c_str()); }
  auto st = ReadState(); auto said = st.error.empty() ? st.syncError : st.error; // user-facing text, never a secret
  std::ofstream(chatOutput / L"resultado.json") << "{\"passed\":" << (passed ? "true" : "false") << ",\"stage\":\"" << stage << "\",\"rows\":" << rows
      << ",\"signed_in\":" << (st.signedIn ? "true" : "false") << ",\"needs_code\":" << (st.needsCode ? "true" : "false") << ",\"busy\":" << (st.busy ? "true" : "false") << ",\"error\":\"" << said << "\"}";
  Quit();
}

#endif
