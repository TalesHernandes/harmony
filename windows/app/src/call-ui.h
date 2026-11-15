// W6 — the direct call and the Windows notices, drawn as the web draws them
// (web/harmony/src/DirectCall.tsx): the card that asks, the screen the call
// becomes, the strip when you go back to the app; and the notice in the
// corner when somebody calls or writes to you and nobody is looking.
//
// Included inside App, after chat-ui.h (it uses its helpers and colours). The
// model is the service's (direct-call.h); this only reads CallView.

// Everything here lives on a layer above the page (`host`, row 1), so that
// ShellPage and the Ajustes, which rebuild `root`, never take a call away.
Grid directLayer{nullptr};
std::string directSignature;
bool directSmall = false;          // "Voltar ao app": the call goes on in the strip
ULONGLONG directLiveAt = 0;        // this window's clock, for mm:ss
std::string directLiveId;
TextBlock directStatus{nullptr};
Documents::Run directStatusRun{nullptr};   // the header's clock, a run beside the name
std::wstring directStatusPrefix;   // "Nome · " on the screen's chip, nothing on the strip
DispatcherTimer directClock{nullptr};
std::optional<app::Notice> shownNotice;   // the one in the corner, for its click
HICON noticeIcon{};
bool callBalloon = false;          // the balloon on screen is a call's
std::string endedNotice;           // why the last call ended, for a few seconds
ULONGLONG endedUntil = 0;

void EnsureDirectLayer() {
  if (directLayer) return;
  directLayer = Grid(); directLayer.Name(L"DirectCallLayer");
  Grid::SetRow(directLayer, 1); Canvas::SetZIndex(directLayer, 10);
  host.Children().Append(directLayer);
}

app::ChatPerson CallFace(app::CallPerson const& p) {
  app::ChatPerson person; person.username = p.username; person.displayName = p.displayName;
  if (service) service->ChatModel().Inspect([&](app::ChatState const& s) {
    for (auto const& m : s.members) if (m.username == p.username) person = m;
  });
  return person;
}

// The clock in figures of one width: Inter's "1" is narrower than its "4",
// and the strip, as wide as what it holds, grew and shrank every second (the
// owner, 03/11/2026).
static TextBlock SteadyClock(TextBlock const& text) {
  Documents::Typography::SetNumeralAlignment(text, FontNumeralAlignment::Tabular);
  return text;
}
static std::wstring CallClock(ULONGLONG since) {
  if (!since) return L"";
  auto total = (GetTickCount64() - since) / 1000;
  wchar_t text[16]; swprintf_s(text, L"%02llu:%02llu", total / 60, total % 60);
  return text;
}

std::wstring CallStatus(app::CallView const& v) {
  if (v.phase == app::CallPhase::Calling) return L"Chamando…";
  // Accepted, and the two ends are finding a route to each other.
  if (v.phase == app::CallPhase::Connecting || !v.mediaConnected) return L"Ligando o áudio…";
  return CallClock(directLiveAt);
}

// The harmony's own sounds (sounds.h): the ring, and the pops of the room
// and of the direct call. Made on first use, on the UI thread.
std::unique_ptr<sounds::Player> soundPlayer;
bool directOwnHangup = false; // this side put the call down: a cancelled ring makes no sound
sounds::Player& SoundPlayer() {
  if (!soundPlayer) {
    auto queue = window.DispatcherQueue();
    soundPlayer = std::make_unique<sounds::Player>([this] { return exiting || Measuring(); },
        [weak = get_weak(), queue](std::function<void()> block) {
          queue.TryEnqueue([weak, block] { if (auto self = weak.get(); self && !self->exiting) block(); });
        });
  }
  return *soundPlayer;
}
// Only ever asked to stop from outside (leaving, signing out): when it rings
// is the direct call's to say (SoundDirect).
void Ringing(bool on) { if (!on && soundPlayer) soundPlayer->StopRing(); }
void SoundDirect(app::CallView const& v) {
  sounds::Call c;
  switch (v.phase) {
    case app::CallPhase::Idle: c.step = sounds::CallStep::Idle; break;
    case app::CallPhase::Calling: c.step = sounds::CallStep::Calling; break;
    case app::CallPhase::Ringing: c.step = sounds::CallStep::Ringing; break;
    case app::CallPhase::Connecting: c.step = sounds::CallStep::Connecting; break;
    case app::CallPhase::Live: c.step = sounds::CallStep::Live; break;
  }
  // Connected once, not right now: a reconnect is not a second hello.
  c.connected = v.phase != app::CallPhase::Idle && v.liveSince != 0;
  c.muted = v.muted; c.deafened = v.deafened;
  auto& player = SoundPlayer();
  player.rules().Direct(c, directOwnHangup);
  if (v.phase == app::CallPhase::Idle) directOwnHangup = false;
  player.RingFor();
}
void SoundRoom(app::State const& state) {
  auto const& v = state.voice;
  sounds::Own own; own.room = v.room; own.live = !v.room.empty() && !v.connecting && !v.reconnecting;
  own.reconnecting = v.reconnecting; own.muted = v.muted; own.deafened = v.deafened; own.screen = v.sharingScreen;
  auto& rules = SoundPlayer().rules();
  rules.Voice(own);
  if (v.room.empty()) return;
  std::vector<sounds::RoomPerson> people;
  for (auto const& room : state.model.rooms) if (room.id == v.room) for (auto const& p : room.people) people.push_back({p.username, p.screen});
  rules.Room(v.room, people, v.self, !own.live);
}

// A separate wave output keeps the message blip from stopping a call's ringtone.
// Service applies the same 500 ms rate limit as notify.ts before this callback.
static void PlayMessageSound() {
  std::thread([] {
    constexpr uint32_t rate=22050, count=rate*24/100;
    std::vector<int16_t> samples(count);
    double phase=0;
    for (uint32_t i=0;i<count;++i) {
      double t=double(i)/rate;
      double gain=t<.01 ? .0001*std::pow(800.,t/.01) : t<.22 ? .08*std::pow(.0001/.08,(t-.01)/.21) : .0001;
      samples[i]=int16_t(32767*gain*std::sin(phase));
      phase+=2*3.14159265358979*(t<.09?660:880)/rate;
    }
    HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!ready) return;
    WAVEFORMATEX format{WAVE_FORMAT_PCM,1,rate,rate*2,2,16,0}; HWAVEOUT output{};
    if (waveOutOpen(&output,WAVE_MAPPER,&format,DWORD_PTR(ready),0,CALLBACK_EVENT) != MMSYSERR_NOERROR) { CloseHandle(ready); logf("message sound unavailable"); return; }
    WAVEHDR header{}; header.lpData=reinterpret_cast<LPSTR>(samples.data()); header.dwBufferLength=DWORD(samples.size()*2);
    if (waveOutPrepareHeader(output,&header,sizeof(header)) == MMSYSERR_NOERROR) {
      ResetEvent(ready);
      if (waveOutWrite(output,&header,sizeof(header)) == MMSYSERR_NOERROR) {
        while (!(header.dwFlags & WHDR_DONE)) if (WaitForSingleObject(ready,1000) != WAIT_OBJECT_0) { waveOutReset(output); break; }
      }
      waveOutUnprepareHeader(output,&header,sizeof(header));
    }
    waveOutClose(output); CloseHandle(ready);
  }).detach();
}

Button WideButton(std::string const& icon, std::wstring const& label, uint32_t ground, uint32_t hover, uint32_t ink, std::function<void()> clicked) {
  Button b; HarmonyPointer(b); b.Height(44); b.MinHeight(0); b.MinWidth(0); b.HorizontalAlignment(HorizontalAlignment::Stretch);
  b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0, 0, 0, 0}); b.Background(Brush(ground));
  b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(hover)); b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(hover));
  StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8); row.HorizontalAlignment(HorizontalAlignment::Center);
  auto glyph = VoiceIcon(icon, ink, 20).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph);
  auto words = Line(label, 14, ink, 500); words.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(words);
  if (ink == DangerSoftFg) {
    NegativeButton(b);
    b.PointerEntered([row,words,icon](auto const&,auto const&) { auto glyph=VoiceIcon(icon,Foreground,20).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().SetAt(0,glyph); words.Foreground(Brush(Foreground)); });
    b.PointerExited([row,words,icon](auto const&,auto const&) { auto glyph=VoiceIcon(icon,Foreground,20).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().SetAt(0,glyph); words.Foreground(Brush(Foreground)); });
  }
  b.Content(row); Automation::AutomationProperties::SetName(b, label);
  b.Click([this, clicked](auto const&, auto const&) { if (!exiting) clicked(); });
  return b;
}

// The red "Desligar" of the bar, the room's "Sair" with another word.
Button HangupButton(std::wstring const& label) {
  Button b; HarmonyPointer(b); b.Name(L"DirectHangup"); b.Height(44); b.MinHeight(0); b.MinWidth(0); b.Padding(Thickness{20, 0, 20, 0});
  b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0, 0, 0, 0}); b.Background(Brush(0xdb3b3e));
  b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(0xe15451)); b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(0xe15451));
  StackPanel words; words.Orientation(Orientation::Horizontal); words.Spacing(8);
  auto door = VoiceIcon("handset-arrow-out", Foreground, 20).as<FrameworkElement>(); door.VerticalAlignment(VerticalAlignment::Center); words.Children().Append(door);
  auto text = Line(label, 14, Foreground, 500); text.VerticalAlignment(VerticalAlignment::Center); words.Children().Append(text);
  b.Content(words); Automation::AutomationProperties::SetName(b, label); ToolTipService::SetToolTip(b, box_value(label));
  b.Click([this](auto const&, auto const&) { if (!exiting) { directOwnHangup = true; service->CallHangup(); } });
  NegativeButton(b);
  return b;
}

Button DirectMute(app::CallView const& v, double box) {
  auto b = CallSquare(v.muted ? "microphone-slash" : "microphone", v.muted ? DangerSoftFg : Foreground, Default, 0x2e2e31,
      v.deafened ? L"Volte a ouvir para ligar o microfone" : v.muted ? L"Ligar o microfone" : L"Desligar o microfone",
      [this] { service->CallMute(!service->CallRead().muted); });
  b.Width(box); b.Height(box); b.Name(L"DirectMute");
  if (box == 36) b.Content(VoiceIcon(v.muted ? "microphone-slash" : "microphone", v.muted ? DangerSoftFg : Foreground, 16));
  return b;
}

// A picture in the call, as the room's tiles draw one (media-ui.h
// MediaPanel): a panel the engine paints into, sized to what is on screen,
// and let go when the call is redrawn. Slots: 1 their camera, 2 their screen,
// 3 my camera, 4 my screen (call-media.h Video).
std::set<int> directSlots;
std::map<int, std::weak_ptr<app::PanelRenderer>> directRenderers;
#include "pip-ui.h"
std::weak_ptr<app::PanelRenderer> directRenderer;   // their camera's: the W6 round counts what it presents
SwapChainPanel DirectPanel(int slot) {
  static wchar_t const* names[] = {L"", L"DirectCameraVideo", L"DirectScreenVideo", L"DirectMyCamera", L"DirectMyScreen"};
  SwapChainPanel panel; panel.Name(names[slot]);
  auto renderer = std::make_shared<app::PanelRenderer>(winrt::get_unknown(panel), slot == 2 ? app::PanelCadence::Screen : app::PanelCadence::Camera); if (slot == 1) directRenderer = renderer;
  directRenderers[slot] = renderer; KeepPicture(panel, renderer);
  // My camera fills its block, as the web's (DirectCall.tsx cover).
  if (slot == 3) renderer->Cover(true);
  service->CallVideo(slot, [renderer](auto track) { renderer->Attach(track); });
  // To the block's edge: 2 px kept clear for the speaking ring showed the
  // block's grey around every camera, talking or not (the owner, 02/11/2026:
  // "só deve aparecer a borda verde quando eu falar"). The ring is drawn
  // over the picture's edge instead.
  RoundVideo(panel, 8);
  directSlots.insert(slot);
  // Hidden in the tray or minimised nobody sees it, so it stops drawing, as
  // the room's tiles do (media-ui.h RefreshMediaVisibility).
  auto measure = [this, renderer](SwapChainPanel surface) {
    double dpi = surface.XamlRoot() ? surface.XamlRoot().RasterizationScale() : 1;
    renderer->Size((mediaShown && !directSmall && !pipActive) ? int(surface.ActualWidth() * dpi) : 0, (mediaShown && !directSmall && !pipActive) ? int(surface.ActualHeight() * dpi) : 0);
  };
  // Weakly: the table outlives the panel, and must not keep its renderer
  // (swap chain, GPU lease, thread) alive after the call.
  mediaVisibility["direct/" + std::to_string(slot)] = [this, weak = make_weak(panel), render = std::weak_ptr<app::PanelRenderer>(renderer)](bool) {
    auto surface = weak.get(); auto draw = render.lock(); if (!surface || !draw) return;
    double dpi = surface.XamlRoot() ? surface.XamlRoot().RasterizationScale() : 1;
    draw->Size((mediaShown && !directSmall && !pipActive) ? int(surface.ActualWidth() * dpi) : 0, (mediaShown && !directSmall && !pipActive) ? int(surface.ActualHeight() * dpi) : 0);
  };
  panel.SizeChanged([measure](auto const& sender, auto const&) { measure(sender.template as<SwapChainPanel>()); });
  panel.CompositionScaleChanged([measure](auto const& sender, auto const&) { measure(sender.template as<SwapChainPanel>()); });
  panel.Unloaded([renderer](auto const&, auto const&) { renderer->Size(0, 0); renderer->Unload(); });
  return panel;
}
void ForgetDirectVideo() {
  if (!service) { for (int slot : directSlots) mediaVisibility.erase("direct/" + std::to_string(slot)); directSlots.clear(); return; }
  for (int slot : directSlots) { service->CallVideo(slot, {}); mediaVisibility.erase("direct/" + std::to_string(slot)); }
  directSlots.clear(); directRenderers.clear();
}

// The green ring of whoever talks, as in a room: read every 50 ms from the
// call's two meters, the room's rule (−60 dBFS, lit for 300 ms after).
Border directRingMine{nullptr}, directRingTheirs{nullptr};
winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer directTalk{nullptr};
uint64_t directLoudMine = 0, directLoudTheirs = 0;
void DirectTalking(bool on) {
  if (!on) { if (directTalk) directTalk.Stop(); directRingMine = nullptr; directRingTheirs = nullptr; return; }
  if (directTalk) { directTalk.Start(); return; }
  directTalk = window.DispatcherQueue().CreateTimer(); directTalk.Interval(std::chrono::milliseconds(50));
  directTalk.Tick([this](auto const&, auto const&) {
    if (!service || exiting) return;
    double mine = 0, theirs = 0; app::DirectLevels(&mine, &theirs);
    auto v = service->CallRead(); auto now = GetTickCount64();
    if (!v.muted && !v.deafened && std::isfinite(mine) && mine > app::VoiceState::kSpeakingFloor) directLoudMine = now;
    if (!v.peerMuted && std::isfinite(theirs) && theirs > app::VoiceState::kSpeakingFloor) directLoudTheirs = now;
    auto lit = [&](Border const& ring, uint64_t loud) { if (ring) ring.Visibility(loud && now - loud < uint64_t(app::VoiceState::kSpeakingHoldMs) ? Visibility::Visible : Visibility::Collapsed); };
    lit(directRingMine, directLoudMine); lit(directRingTheirs, directLoudTheirs);
  });
  directTalk.Start();
}

// IncomingCall (DirectCall.tsx): a card at the top of the window, the rest of
// the app usable under it. Answering is the accent, refusing is danger; no
// green, nobody is talking yet.
void DrawIncoming(app::CallView const& v) {
  auto card = Island(8); card.Background(Brush(Surface2)); card.Name(L"IncomingCall"); card.Width(384); card.Padding(Thickness{16, 16, 16, 16});
  card.HorizontalAlignment(HorizontalAlignment::Center); card.VerticalAlignment(VerticalAlignment::Top); card.Margin(Thickness{12, 12, 12, 12});
  StackPanel stack; stack.Spacing(16);
  Grid who; who.ColumnSpacing(12);
  for (auto w : {GridLength{48, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}}) { ColumnDefinition c; c.Width(w); who.ColumnDefinitions().Append(c); }
  auto person = CallFace(v.peer);
  auto face = PhotoFace(person, 48); face.VerticalAlignment(VerticalAlignment::Center); who.Children().Append(face);
  StackPanel names; names.VerticalAlignment(VerticalAlignment::Center);
  names.Children().Append(Line(PersonName(person), 16, Foreground, 500));
  names.Children().Append(Line(L"está te ligando", 13, Muted, 400));
  Grid::SetColumn(names, 1); who.Children().Append(names); stack.Children().Append(who);
  Grid buttons; buttons.ColumnSpacing(8);
  for (int i = 0; i < 2; ++i) { ColumnDefinition c; c.Width(GridLength{1, GridUnitType::Star}); buttons.ColumnDefinitions().Append(c); }
  auto accept = WideButton("handset-arrow-in", L"Atender", Accent, 0x3592f9, Foreground, [this] { directSmall = false; service->CallAccept(); });
  accept.Name(L"DirectAccept"); buttons.Children().Append(accept);
  auto reject = WideButton("xmark", L"Recusar", 0x351d20, 0xdb3b3e, DangerSoftFg, [this] { service->CallDecline(); });
  NegativeButton(reject); reject.Name(L"DirectReject"); Grid::SetColumn(reject, 1); buttons.Children().Append(reject);
  stack.Children().Append(buttons); card.Child(stack);
  directLayer.Children().Append(card);
}

// CallScreen (DirectCall.tsx): the call over the app, drawn as a room is
// (the owner, 18/10/2026: "parecer mais com a interface de uma chamada normal
// em um canal"; 25/10/2026: with what the room got since, by drawing the
// room's own stage, main.cpp DrawStage): the room's header with the person
// where the channel goes, the two people as blocks with the green ring, a
// screen as one more block that can be made big, full screen or watched with
// the cameras floating over it, its sound on its own slider, and the room's
// bar — microphone, hearing, camera, screen and the red way out.
std::string directScreenSeen;   // the screen block last put in focus by itself
void DrawCallScreen(app::CallView const& v) {
  ForgetDirectVideo();
  auto peer = CallFace(v.peer);
  app::ChatPerson me;
  service->ChatModel().Inspect([&](app::ChatState const& s) { me.username = s.owner; for (auto const& p : s.members) if (p.username == s.owner) me = p; });
  bool live = v.phase == app::CallPhase::Live;
  auto marks = [](bool muted, bool deafened) {
    std::vector<std::pair<std::string, uint32_t>> m;
    if (deafened) m.push_back({"volume-xmark", DangerSoftFg}); else if (muted) m.push_back({"microphone-slash", DangerSoftFg});
    return m;
  };
  std::vector<Seat> seats;
  auto theirName = PersonName(peer), myName = PersonName(me);
  {
    Seat seat; seat.key = "p:" + peer.username; seat.name = seat.label = seat.access = theirName; seat.person = peer;
    if (live && v.peerCam) seat.picture = [this] { return DirectPanel(1).as<FrameworkElement>(); };
    seat.marks = marks(v.peerMuted, false); seat.ring = [this](Border r) { directRingTheirs = r; };
    seat.menu = [this, who = peer.username, theirName](FrameworkElement at, auto point) { OpenVoiceVolume(at, who, theirName, point); };
    seats.push_back(std::move(seat));
  }
  {
    Seat seat; seat.key = "p:" + me.username; seat.mine = true; seat.name = myName; seat.label = seat.access = myName + L" (você)"; seat.person = me;
    if (v.cam) seat.picture = [this] { return DirectPanel(3).as<FrameworkElement>(); };
    seat.marks = marks(v.muted, v.deafened); seat.ring = [this](Border r) { directRingMine = r; };
    seats.push_back(std::move(seat));
  }
  if (live && v.peerScreen) {
    Seat seat; seat.key = "s:" + peer.username; seat.screen = true; seat.name = theirName;
    seat.label = seat.access = L"Tela de " + theirName; seat.bigLabel = theirName + L" está mostrando a tela";
    seat.picture = [this] { return DirectPanel(2).as<FrameworkElement>(); };
    seat.volume = [this, who = peer.username, theirName] { return LiveVolume(who, theirName); };
    seats.push_back(std::move(seat));
  }
  if (v.screen) {
    Seat seat; seat.key = "s:" + me.username; seat.screen = true; seat.mine = true; seat.name = myName;
    seat.label = seat.bigLabel = seat.access = L"Sua tela";
    seat.picture = [this] { return DirectPanel(4).as<FrameworkElement>(); };
    seats.push_back(std::move(seat));
  }
  // A screen that starts is what is being pointed at (theirs before yours),
  // as before; from there the person chooses, as in a room.
  std::string started = live && v.peerScreen ? "s:" + peer.username : v.screen ? "s:" + me.username : "";
  if (started != directScreenSeen) { directScreenSeen = started; if (!started.empty() && directStage.full.empty()) directStage.focus = started; }
  if (seats.size() < 2 || std::none_of(seats.begin(), seats.end(), [&](Seat const& s) { return s.key == directStage.focus; })) directStage.focus.clear();
  if (fullStage == &directStage && directStage.full != directStage.focus) LeaveCallFull();
  const bool full = !directStage.full.empty();

  Grid screen; screen.Name(L"DirectCallScreen"); screen.Background(Brush(full ? 0x000000 : Background)); screen.Padding(full ? Thickness{0, 0, 0, 0} : Thickness{16, 16, 16, 16});
  for (auto h : {GridLength{full ? 0.0 : 56.0, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}, GridLength{full ? 0.0 : 68.0, GridUnitType::Pixel}}) {
    RowDefinition r; r.Height(h); screen.RowDefinitions().Append(r);
  }

  // The header: the room's island, "Voltar ao app" where the column would
  // be (the call keeps going, and it says so in words), then the handset,
  // the name and the clock.
  auto heading = Island(); heading.Name(L"DirectHeader"); heading.Height(48); heading.VerticalAlignment(VerticalAlignment::Top); heading.Padding(Thickness{8, 0, 8, 0});
  Grid head; head.ColumnSpacing(8);
  for (auto w : {GridLength{0, GridUnitType::Auto}, GridLength{18, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); head.ColumnDefinitions().Append(c); }
  Button back; HarmonyPointer(back); back.Name(L"DirectShrink"); back.Height(36); back.MinHeight(0); back.Padding(Thickness{12, 0, 12, 0}); back.VerticalAlignment(VerticalAlignment::Center);
  back.CornerRadius(CornerRadius{8, 8, 8, 8}); back.BorderThickness(Thickness{0, 0, 0, 0}); back.Background(Brush(Default));
  back.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(0x2e2e31)); back.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(0x2e2e31));
  StackPanel backRow; backRow.Orientation(Orientation::Horizontal); backRow.Spacing(8);
  auto chevron = VoiceIcon("chevron-down", Foreground, 16).as<FrameworkElement>(); chevron.VerticalAlignment(VerticalAlignment::Center); backRow.Children().Append(chevron);
  auto backWords = Line(L"Voltar ao app", 13, Foreground, 500); backWords.VerticalAlignment(VerticalAlignment::Center); backRow.Children().Append(backWords);
  back.Content(backRow); Automation::AutomationProperties::SetName(back, L"voltar ao app sem desligar");
  ToolTipService::SetToolTip(back, box_value(L"A chamada continua — você volta por cima da conversa"));
  back.Click([this](auto const&, auto const&) { directSmall = true; directSignature.clear(); Render(); });
  head.Children().Append(back);
  auto handset = VoiceIcon("handset", Muted, 18).as<FrameworkElement>(); handset.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(handset, 1); head.Children().Append(handset);
  // The name and the clock are one line of text, two sizes on one baseline:
  // as two blocks each centred on its own, the 13 sat lower than the 16 and
  // neither lined up with the handset (the owner, 02/11/2026).
  auto title = Line(L"", 16, Foreground, 600); title.Name(L"DirectTitle"); title.VerticalAlignment(VerticalAlignment::Center);
  title.TextTrimming(TextTrimming::CharacterEllipsis);
  Documents::Run named; named.Text(theirName); title.Inlines().Append(named);
  directStatusPrefix = L"  ·  "; directStatusRun = Documents::Run(); directStatusRun.Text(directStatusPrefix + CallStatus(v));
  directStatusRun.FontSize(13); directStatusRun.FontWeight(Weight(400)); directStatusRun.Foreground(Brush(Muted)); title.Inlines().Append(directStatusRun);
  SteadyClock(title);
  Grid::SetColumn(title, 2); head.Children().Append(title);
  // The floating window, opened by hand (the owner, 02/11/2026: it must not
  // appear by itself, and once closed it must open again): the call goes
  // back to the app and the picture floats over everything.
  if (PipAvailable(v)) { auto pip = PipButton(36); pip.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(pip, 3); head.Children().Append(pip); }
  heading.Child(head); if (!full) screen.Children().Append(heading);

  if (!v.notice.empty() && !full) {
    // "Reconectando…" is the line getting worse and still working: warning;
    // anything else said here is something that failed.
    bool worse = v.notice == "Reconectando…";
    Border note; note.Name(L"DirectNotice"); note.CornerRadius(CornerRadius{8, 8, 8, 8}); note.Padding(Thickness{12, 8, 12, 8}); note.Margin(Thickness{0, 8, 0, 0});
    note.Background(worse ? Brush(0x2f2a17) : Brush(0x351d20));
    note.Child(Text(net::Wide(v.notice), 13, Foreground)); Grid::SetRow(note, 2); screen.Children().Append(note);
  }

  // The room's bar (main.cpp CallBar).
  auto controls = Island(); controls.Name(L"DirectBar"); controls.Height(60); controls.VerticalAlignment(VerticalAlignment::Bottom); controls.Padding(Thickness{8, 8, 8, 8}); Grid::SetRow(controls, 3);
  Grid bar; StackPanel toggles; toggles.Orientation(Orientation::Horizontal); toggles.Spacing(8);
  auto mute = DirectMute(v, 44); mute.IsEnabled(!v.deafened); toggles.Children().Append(mute);
  auto deaf = CallSquare(v.deafened ? "volume-xmark" : "headphones", v.deafened ? DangerSoftFg : Foreground, Default, 0x2e2e31,
      v.deafened ? L"Voltar a ouvir" : L"Não ouvir", [this] { service->CallDeafen(!service->CallRead().deafened); });
  deaf.Name(L"DirectDeafen"); toggles.Children().Append(deaf);
  toggles.Children().Append(CameraControls(true));
  auto share = CallSquare(v.screen ? "display-pulse" : "display", Foreground, v.screen ? Accent : Default, v.screen ? 0x3592f9 : 0x2e2e31,
      v.screen ? L"Trocar ou parar a tela" : L"Compartilhar a tela", [this] { ScreenPicker(true); });
  share.Name(L"DirectShare"); toggles.Children().Append(share);
  auto hang = HangupButton(L"Desligar"); hang.HorizontalAlignment(HorizontalAlignment::Right);
  if (!full) { bar.Children().Append(toggles); bar.Children().Append(hang); controls.Child(bar); screen.Children().Append(controls); }
  else controls.Height(NAN);
  // The 16 around the screen is this call's margin already.
  DrawStage(directStage, screen, 1, seats, controls, toggles, hang).Margin(Thickness{0, 0, 0, 0});
  DirectTalking(true);
  directLayer.Children().Append(screen);
}

// CallStrip (DirectCall.tsx): the call put away, still up, with the way back.
// The owner, 02/11/2026: only as wide as what it holds, carried anywhere
// with the mouse (it stays where it was left for the rest of the call), and
// not the colour of the line under the pointer, which it sat on and merged
// with: a step up from the conversation, with a shadow under it.
double stripX = 0, stripY = 0;        // where it was carried, from its place at the top
std::string stripCall;                // the call that offset belongs to
struct StripDrag { bool on = false; POINT from{}; double x = 0, y = 0; } stripDrag;
void ClampStrip(Border const& strip, TranslateTransform const& move) {
  double room = directLayer ? directLayer.ActualWidth() : 0, high = directLayer ? directLayer.ActualHeight() : 0;
  double w = strip.ActualWidth(), h = strip.ActualHeight();
  if (room > w) stripX = std::clamp(stripX, -(room - w) / 2 + 8, (room - w) / 2 - 8);
  if (high > h) stripY = std::clamp(stripY, -64.0 + 8, high - 64 - h - 8);
  move.X(stripX); move.Y(stripY);
}
void DrawCallStrip(app::CallView const& v) {
  if (stripCall != v.id) { stripCall = v.id; stripX = stripY = 0; }
  Border strip; strip.Name(L"DirectStrip"); strip.Background(Brush(Surface2)); strip.CornerRadius(CornerRadius{8, 8, 8, 8});
  strip.Padding(Thickness{12, 6, 6, 6});
  strip.HorizontalAlignment(HorizontalAlignment::Center); strip.VerticalAlignment(VerticalAlignment::Top); strip.Margin(Thickness{0, 64, 0, 0});
  strip.Shadow(ThemeShadow()); strip.Translation(winrt::Windows::Foundation::Numerics::float3{0, 0, 32});
  TranslateTransform move; strip.RenderTransform(move);
  HarmonyPointer(strip, winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeAll);
  strip.PointerPressed([this, move](auto const& sender, Input::PointerRoutedEventArgs const& e) {
    auto self = sender.template as<Border>();
    if (!e.GetCurrentPoint(self).Properties().IsLeftButtonPressed()) return;
    stripDrag.on = true; GetCursorPos(&stripDrag.from); stripDrag.x = stripX; stripDrag.y = stripY;
    self.CapturePointer(e.Pointer()); e.Handled(true);
  });
  strip.PointerMoved([this, move](auto const& sender, auto const&) {
    if (!stripDrag.on) return; POINT p{}; GetCursorPos(&p);
    double scale = GetDpiForWindow(hwnd) / 96.0;
    stripX = stripDrag.x + (p.x - stripDrag.from.x) / scale; stripY = stripDrag.y + (p.y - stripDrag.from.y) / scale;
    ClampStrip(sender.template as<Border>(), move);
  });
  auto release = [this](auto const& sender, auto const&) { if (!stripDrag.on) return; stripDrag.on = false; sender.template as<UIElement>().ReleasePointerCaptures(); };
  strip.PointerReleased(release); strip.PointerCaptureLost([this](auto const&, auto const&) { stripDrag.on = false; });
  strip.SizeChanged([this, move](auto const& sender, auto const&) { ClampStrip(sender.template as<Border>(), move); });
  StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8);
  Border dot; dot.Width(8); dot.Height(8); dot.CornerRadius(CornerRadius{4, 4, 4, 4}); dot.Background(Brush(Success)); dot.VerticalAlignment(VerticalAlignment::Center);
  dot.Margin(Thickness{0, 0, 4, 0}); row.Children().Append(dot);
  StackPanel words; words.VerticalAlignment(VerticalAlignment::Center); words.MaxWidth(160); words.Margin(Thickness{0, 0, 8, 0});
  words.Children().Append(Line(PersonName(CallFace(v.peer)), 13, Foreground, 500));
  directStatus = SteadyClock(Line(CallStatus(v), 13, Muted, 400)); directStatusPrefix.clear(); words.Children().Append(directStatus);
  row.Children().Append(words);
  auto lift = [](Button const& b) {
    b.Background(Brush(Surface3)); b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(0x3f3f46)); b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(0x3f3f46));
  };
  Button open; HarmonyPointer(open); open.Name(L"DirectOpen"); open.Height(36); open.MinHeight(0); open.Padding(Thickness{12, 0, 12, 0});
  open.CornerRadius(CornerRadius{8, 8, 8, 8}); open.BorderThickness(Thickness{0, 0, 0, 0}); lift(open);
  open.Content(Line(L"Ver chamada", 13, Foreground, 500)); Automation::AutomationProperties::SetName(open, L"voltar para a chamada");
  ToolTipService::SetToolTip(open, box_value(L"Voltar para a tela da chamada"));
  open.Click([this](auto const&, auto const&) { directSmall = false; pipWanted = false; directSignature.clear(); Render(); });
  open.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(open);
  if (PipAvailable(v)) { auto pip = PipButton(36); lift(pip); row.Children().Append(pip); }
  auto mute = DirectMute(v, 36); if (!v.muted) lift(mute); row.Children().Append(mute);
  auto hang = Square("handset-arrow-out", L"Desligar", 36, 16, DangerSoftFg, 0xdb3b3e, [this] { directOwnHangup = true; service->CallHangup(); });
  hang.Name(L"DirectStripHangup"); hang.Background(Brush(0x351d20)); row.Children().Append(hang);
  strip.Child(row); directLayer.Children().Append(strip);
}

winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer endedTimer{nullptr};
void DrawEnded(std::string const& text) {
  auto note = Island(8); note.Name(L"DirectEnded"); note.Padding(Thickness{16, 12, 16, 12});
  note.HorizontalAlignment(HorizontalAlignment::Center); note.VerticalAlignment(VerticalAlignment::Top); note.Margin(Thickness{12, 12, 12, 12});
  note.Child(Text(net::Wide(text), 13, Foreground)); note.IsHitTestVisible(false);
  directLayer.Children().Append(note);
}

void DrawDirectCall() {
  if (!service) return;
  auto v = service->CallRead();
  // The bell on both sides while it asks, the sounds of the call starting
  // and ending (sounds.h).
  SoundDirect(v);
  if (v.phase == app::CallPhase::Live && v.mediaConnected && directLiveId != v.id) { directLiveId = v.id; directLiveAt = GetTickCount64(); }
  if (v.phase == app::CallPhase::Idle) { directSmall = false; directLiveId.clear(); directLiveAt = 0; }
  // A call put away, ringing or over takes its full screen with it, and a
  // new call starts on the grid.
  if ((v.phase == app::CallPhase::Idle || v.phase == app::CallPhase::Ringing || directSmall) && fullStage == &directStage) { LeaveCallFull(); directSignature.clear(); }
  if (v.phase == app::CallPhase::Idle) { directStage = CallStage{true}; directScreenSeen.clear(); }
  if (v.phase == app::CallPhase::Idle && !v.notice.empty() && endedNotice != std::to_string(v.sequence)) {
    // Why it ended ("Chamada recusada.", "Essa pessoa já está em outra
    // chamada."), said for six seconds where the call was, then gone.
    endedNotice = std::to_string(v.sequence); endedUntil = GetTickCount64() + 6000;
    auto weak = get_weak();
    auto timer = window.DispatcherQueue().CreateTimer(); timer.Interval(std::chrono::milliseconds(6100)); timer.IsRepeating(false);
    timer.Tick([weak](auto const&, auto const&) { if (auto self = weak.get()) { self->directSignature.clear(); self->Render(); } });
    timer.Start(); endedTimer = timer;
  }
  if (v.phase != app::CallPhase::Idle || GetTickCount64() >= endedUntil) endedUntil = 0;
  std::string signature = (endedUntil ? "e" : "") + std::to_string(int(v.phase)) + "|" + v.id + "|" + v.peer.username + "|" + (v.muted ? "m" : "_") + (v.peerMuted ? "p" : "_") +
      (v.mediaConnected ? "c" : "_") + (directSmall ? "s" : "_") + (v.deafened ? "d" : "_") + (v.cam ? "v" : "_") + (v.screen ? "t" : "_") + (v.peerCam ? "V" : "_") + (v.peerScreen ? "T" : "_") + "|" + v.notice;
  if (signature == directSignature) { SyncPip(); return; }
  directSignature = signature;
  EnsureDirectLayer(); directLayer.Children().Clear(); directStatus = nullptr; directStatusRun = nullptr; DirectTalking(false);
  if (v.phase == app::CallPhase::Idle || v.phase == app::CallPhase::Ringing || directSmall) ForgetDirectVideo();
  if (v.phase != app::CallPhase::Ringing) ForgetCallNotice();
  if (v.phase == app::CallPhase::Idle) {
    SyncPip();
    if (directClock) directClock.Stop();
    if (!v.notice.empty() && endedUntil) DrawEnded(v.notice);
    return;
  }
  if (v.phase == app::CallPhase::Ringing) DrawIncoming(v);
  else if (directSmall) DrawCallStrip(v);
  else DrawCallScreen(v);
  if (!directClock) {
    directClock = DispatcherTimer(); directClock.Interval(std::chrono::seconds(1));
    directClock.Tick([this](auto const&, auto const&) {
      if (!service) return;
      if (directStatusRun) directStatusRun.Text(directStatusPrefix + CallStatus(service->CallRead()));
      if (directStatus) directStatus.Text(directStatusPrefix + CallStatus(service->CallRead()));
    });
  }
  directClock.Start();
  SyncPip(true);
}

// ---------------------------------------------------------------- notices

// Prefer the native toast (toast.cpp); the tray balloon is its fallback.
// Windows shows harmony's name and icon, and a click returns to the window.
void ShowNotice(app::Notice notice, bool fallback = false) {
  if (exiting) return;
  if (modernNotices && !fallback) { ShowModernNotice(notice); return; }
  if (!trayAdded) return;
  if (!noticeIcon) noticeIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
  NOTIFYICONDATAW d = tray; d.uFlags = NIF_INFO;
  auto title = net::Wide(notice.title), body = net::Wide(notice.body);
  if (title.size() > 63) title = title.substr(0, 62) + L"…";
  if (body.size() > 255) body = body.substr(0, 254) + L"…";
  wcsncpy_s(d.szInfoTitle, title.c_str(), _TRUNCATE); wcsncpy_s(d.szInfo, body.empty() ? L" " : body.c_str(), _TRUNCATE);
  d.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME; d.hBalloonIcon = noticeIcon ? noticeIcon : icon;
  // A call that cannot be said now is not said later, ringing for nobody.
  bool call = notice.target == app::Notice::Target::Call;
  if (call) d.uFlags |= NIF_REALTIME;
  bool shown = Shell_NotifyIconW(NIM_MODIFY, &d);
  if (shown) { callBalloon = call;
#ifndef HARMONY_PUBLISH
    if (!callOutput.empty()) auditNotices.push_back(notice);
#endif
    shownNotice = std::move(notice);
  }
  logf("notice shown %d", shown);
}

// A click on the notice: the window comes back at the place it was about.
void OpenNotice() {
  auto notice = shownNotice;
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetForegroundWindow() != hwnd) Open();
  if (!notice || !service) return;
  logf("notice clicked");
  if (notice->target == app::Notice::Target::Call) { directSmall = false; service->CallOpen(notice->id); return; }
  if (notice->target != app::Notice::Target::Conversation) return;
  if (settings) CloseSettings();
  bool dm = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& c : s.conversations) if (c.id == notice->id) dm = c.kind == "dm"; });
  std::string tab = dm ? "dm" : "server";
  if (chatTab != tab) { chatTab = tab; channelsSignature.clear(); }
  OpenConversation(notice->id);
}

ToggleSwitch NoticesToggle() {
  ToggleSwitch toggle; HarmonyPointer(toggle); toggle.Name(L"Notices"); toggle.Header(box_value(L"Avisos neste computador")); toggle.IsOn(service && service->NoticesOn());
  toggle.Toggled([this](auto const& sender, auto const&) {
    if (!designOutput.empty() || !service) return;
    service->SetNoticesOn(sender.template as<ToggleSwitch>().IsOn());
  });
  return toggle;
}

ToggleSwitch SoundsToggle() {
  ToggleSwitch toggle; HarmonyPointer(toggle); toggle.Name(L"MessageSounds"); toggle.Header(box_value(L"Som de mensagem")); toggle.IsOn(service && service->SoundsOn());
  toggle.Toggled([this](auto const& sender,auto const&) {
    if (!designOutput.empty() || !service) return;
    service->SetSoundsOn(sender.template as<ToggleSwitch>().IsOn());
  });
  return toggle;
}

// The Windows notice with buttons (toast.cpp): Atender and Recusar on the
// call, and it stays in the notification centre. It is the system's classic
// toast with a COM activator of the program's own, because the Windows App
// SDK's AppNotificationManager::Register() fails in this self-contained
// program (issue 6774, 0x8007007E). When Windows does not take it either
// (notices off for the program, Focus, an old shortcut), the tray balloon
// above carries the notice, and the log says which one did.
bool modernNotices = false;
std::string callToastTag;
uint64_t toastGeneration = 0, callToastGeneration = 0;
std::wstring NoticeIdentity() { return callOutput.empty() ? std::wstring(app::AppID) : L"Harmony.Native.Windows.Audit"; }
void RegisterNotices() {
  // A measuring run registers nothing; the W6 round registers under an
  // identity of its own and removes it at the end (call-audit.h).
  if (!designOutput.empty() || !chatOutput.empty()) return;
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  // Activate arrives on a COM thread: the action goes to the window's queue.
  modernNotices = app::ToastsStart(NoticeIdentity(), [dispatcher, weak](app::ToastAction action) {
    dispatcher.TryEnqueue([weak, values = std::move(action.values)] {
      if (auto self = weak.get()) if (!self->exiting) self->NoticeAction(values);
    });
  });
  logf(modernNotices ? "notices: Windows toasts" : "notices: toasts unavailable; tray balloon");
}
void UnregisterNotices() {
  if (!modernNotices) return;
  modernNotices = false; ++toastGeneration;
  // Only while the program runs (the server does not wake a closed one):
  // a notice left behind would start it again for a call long gone.
  app::ToastsStop();
}
void ShowModernNotice(app::Notice const& notice) {
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  bool call = notice.target == app::Notice::Target::Call;
  // Reserve the tag now: a call ending during confirmation queues Remove after Show.
  if (call) callToastTag = notice.tag;
  auto generation = toastGeneration, callGeneration = callToastGeneration;
  app::ToastShowAsync(notice, [dispatcher, weak, notice, call, generation, callGeneration](bool shown) {
    dispatcher.TryEnqueue([weak, notice, call, generation, callGeneration, shown] {
      auto self = weak.get();
      if (!self || self->exiting || generation != self->toastGeneration ||
          (call && callGeneration != self->callToastGeneration)) return;
      logf("notice shown (toast) %d", shown);
      if (shown) {
#ifndef HARMONY_PUBLISH
        if (!callOutput.empty()) self->auditNotices.push_back(notice);
#endif
      } else {
        if (call && self->callToastTag == notice.tag) self->callToastTag.clear();
        self->ShowNotice(notice, true);
      }
    });
  });
}
// The call stopped ringing (answered here or elsewhere, refused, gone): its
// notice goes too, buttons and all.
void ForgetCallNotice() {
  ++callToastGeneration;
  if (callBalloon && trayAdded) {
    // An empty text takes the balloon away (NOTIFYICONDATAW), only if it is
    // still the call's and not a message that replaced it.
    callBalloon = false; shownNotice.reset();
    NOTIFYICONDATAW d = tray; d.uFlags = NIF_INFO; d.szInfo[0] = 0; d.szInfoTitle[0] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &d);
  }
  if (callToastTag.empty() || !modernNotices) return;
  app::ToastRemove(callToastTag); callToastTag.clear();
}
void NoticeAction(std::map<std::string, std::string> const& values) {
  auto get = [&](char const* key) { auto it = values.find(key); return it == values.end() ? std::string() : it->second; };
  auto target = get("target"), id = get("id"), action = get("action");
  logf("notice clicked (%s%s%s)", target.c_str(), action.empty() ? "" : " ", action.c_str());
  if (!service || exiting) return;
  if (target == "call" && action == "decline") {
    if (service->CallRead().id == id) service->CallDecline();
    return;
  }
  Open();
  if (target == "call") {
    directSmall = false;
    if (action == "accept" && service->CallRead().id == id && service->CallRead().phase == app::CallPhase::Ringing) service->CallAccept();
    else service->CallOpen(id);
    return;
  }
  if (target != "conversation") return;
  shownNotice = app::Notice{app::Notice::Target::Conversation, id, "", "", ""};
  OpenNotice();
}
