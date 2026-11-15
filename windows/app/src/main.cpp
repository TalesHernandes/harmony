#include "test-process-guard.h"
#include <windows.h>
#undef GetCurrentTime
#include <shellapi.h>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <sddl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Text.h>
#include <mmsystem.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include "service.h"
#include "chat-format.h"
#include "toast.h"
#include <winrt/Windows.UI.Notifications.h>
#include "platform.h"
#include "updater.h"
#include "version.h"
#include "startup-timing.h"
#ifndef HARMONY_PUBLISH
#include "../../native/gpu/diagnostic-state.h"
#endif
#include "diagnostic-history.h"
#include "../../native/src/network-state.h"
#include "../../native/src/audio.h"
#include <array>
#include "../../native/src/log.h"
#include "../../native/src/perf.h"
#include <memory>
#include <fstream>
#include <cwctype>
#include <source_location>
#include "design-audit.h"
#include "wheel-scroll.h"
#include "changelog.h"
#include "harmony-controls.h"
#include "media-preview.h"
#include "../../native/src/camera.h"
#include "panel-renderer.h"
#include "sounds.h"
#include "capture-notice.h"
#include "emoji.h"
#include "photo-crop.h"
#include "window-state.h"
#include "tray-menu.h"
#include "../instalar/installer-text.h"
#include "tray-icons.h"
#include "image-cache-budget.h"
#include <robuffer.h>
#include <atomic>
#include <set>
#include <deque>
#include <coroutine>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
namespace {
constexpr UINT TrayMessage = WM_APP + 17, OpenMessage = WM_APP + 18;
std::wstring controlClassStorage = L"Harmony.Native.Windows.Control";
const wchar_t* ControlClass = controlClassStorage.c_str();
bool startHidden = false;
std::filesystem::path uiTestOutput;
std::filesystem::path designOutput;
bool resourceAudit = false;
std::filesystem::path chatOutput;
std::filesystem::path callOutput;   // --call-audit (W6)
bool voiceDesign = false;
bool screenDesign=false;
bool callDesign=false;
std::wstring MutexName() {
  HANDLE token{}; check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  DWORD size{}; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> bytes(size);
  BOOL ok = GetTokenInformation(token, TokenUser, bytes.data(), size, &size); CloseHandle(token); check_bool(ok);
  PWSTR sid{}; check_bool(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid));
  std::wstring name = L"Local\\Harmony.Native.Windows." + std::wstring(sid); LocalFree(sid);
  if (auto root = app::installer::TestRoot(); !root.empty()) name += L".Installer." + root.filename().wstring();
  return name;
}
SolidColorBrush Brush(uint32_t rgb) {
  return SolidColorBrush(winrt::Windows::UI::Color{255, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)});
}
TextBlock Text(std::wstring const& value, double size = 15, uint32_t color = 0xfcfcfc) {
  TextBlock text; text.Text(value); text.FontSize(size); text.Foreground(Brush(color));
  text.TextWrapping(TextWrapping::Wrap); text.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter"));
  text.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  text.LineHeight(size == 20 ? 28 : size == 16 ? 24 : size == 15 ? 22 : size == 13 ? 18 : 16);
  text.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
  text.CharacterSpacing(size == 12 ? 60 : size == 13 ? -20 : size == 16 ? -10 : -15);
  return text;
}
UIElement VoiceIcon(std::string name, uint32_t color = 0xfcfcfc, double size = 20) {
  // Read once per name (UI thread only): opening the file for every icon was
  // part of the 0.6 ms each one cost, and a message row makes several.
  static std::map<std::string, std::wstring> sources;
  auto source = sources.find(name);
  if (source == sources.end()) {
    std::ifstream in(app::Executable().parent_path() / L"assets" / (name + ".xaml"));
    std::string xaml((std::istreambuf_iterator<char>(in)), {});
    source = sources.emplace(name, net::Wide(xaml)).first;
  }
  auto view = Markup::XamlReader::Load(source->second).as<Viewbox>();
  auto canvas = view.Child().as<Canvas>();
  auto path = canvas.Children().GetAt(0).as<Shapes::Path>(); path.Fill(Brush(color));
  // The geometry itself is scaled, not the drawn shape: a Viewbox scaled the
  // 16-unit path after it was rasterized, which left every glyph not drawn at
  // 16 soft (the gear at 20, the "x" at 12).
  Media::Matrix m{1, 0, 0, 1, 0, 0};
  if (auto t = path.RenderTransform().try_as<TranslateTransform>()) m.OffsetX = t.X(), m.OffsetY = t.Y();
  else if (auto x = path.RenderTransform().try_as<MatrixTransform>()) m = x.Matrix();
  double s = size / 16.0;
  MatrixTransform scaled; scaled.Matrix(Media::Matrix{m.M11 * s, m.M12 * s, m.M21 * s, m.M22 * s, m.OffsetX * s, m.OffsetY * s});
  canvas.Children().Clear(); view.Child(nullptr);
  // An identity transform on the geometry draws nothing (the pin at 16, the
  // one icon with no offset, was a blank button in the message's actions):
  // at its own size and place, the geometry is left as it was read.
  bool identity = s == 1 && m.M11 == 1 && m.M12 == 0 && m.M21 == 0 && m.M22 == 1 && m.OffsetX == 0 && m.OffsetY == 0;
  path.RenderTransform(nullptr); if (!identity) path.Data().Transform(scaled);
  Canvas box; box.Width(size); box.Height(size); box.Children().Append(path);
  return box;
}
void IconInk(winrt::Windows::Foundation::IInspectable const& content, uint32_t color) {
  auto box=content.try_as<Canvas>();
  if (box && box.Children().Size()) box.Children().GetAt(0).as<Shapes::Path>().Fill(Brush(color));
}
UIElement Headphones() {
  // Exact path and placement from web/harmony/src/Icon.tsx, not a system emoji.
  std::ifstream in(app::Executable().parent_path()/L"assets/headphones.xaml");
  std::string xaml((std::istreambuf_iterator<char>(in)), {});
  return Markup::XamlReader::Load(net::Wide(xaml)).as<UIElement>();
}
std::wstring Initials(std::wstring name) {
  return app::ChatInitials(name);
}
ToggleSwitch StartupToggle(bool enabled, std::function<void(bool)> changed) {
  ToggleSwitch startup; HarmonyPointer(startup); startup.Header(box_value(L"Abrir com o Windows")); startup.IsOn(enabled);
  startup.Toggled([changed](auto const& sender, auto const&) {
    changed(sender.template as<ToggleSwitch>().IsOn());
  });
  return startup;
}
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
  app::installer::Updater* updater_=nullptr;
  explicit App(app::installer::Updater* updater=nullptr):updater_(updater) {}
  XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
  Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) { return metadata.GetXamlType(type); }
  Markup::IXamlType GetXamlType(hstring const& name) { return metadata.GetXamlType(name); }
  com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }
  Window window{nullptr}; Grid root{nullptr}, host{nullptr}; StackPanel page{nullptr}, roomList{nullptr};
  TextBox user{nullptr}, code{nullptr}; PasswordBox password{nullptr};
  TextBlock status{nullptr}, error{nullptr}, factorLabel{nullptr}; Button submit{nullptr}, recoveryButton{nullptr};
  bool recoveryCode = false;
  HWND hwnd{}, control{}; HICON icon{}; NOTIFYICONDATAW tray{};
  UINT taskbarCreated{};
  bool trayAdded = false, exiting = false, settings = false, showingCode = false, showingRooms = false;
  bool startupUsable=false;
  bool startupMaximized=false,placementReady=false;
  bool Measuring() const {return !designOutput.empty() || !chatOutput.empty() || !callOutput.empty();}
  bool PlacementAudit() const {
    return !designOutput.empty() && app::Environment(L"HARMONY_W8A_PLACEMENT")=="1" && !app::installer::TestRoot().empty();
  }
  void SavePlacement() {
    if(!placementReady || (Measuring() && !PlacementAudit()))return;
    try {if(!app::WriteWindowState(app::DataDirectory()/L"window-state.txt",app::CaptureWindowState(hwnd)))logf("window placement write failed");} catch(...) {logf("window placement write failed");}
  }
  bool settingsSignedIn = false;
  StackPanel voiceBar{nullptr};
  Button accountSettings{nullptr};
  FrameworkElement auditVolume{nullptr}; Flyout volumeFlyout{nullptr};
  std::map<std::string, Border> voiceFaces;
  std::map<std::string, TextBlock> voiceNames;
  std::string roomsSignature, barSignature, callSignature;
  FrameworkElement roomNotice{nullptr}; Grid callView{nullptr};
  std::map<std::string, Border> callFaces;
  ComboBox voiceInput{nullptr}, voiceOutput{nullptr};
  TextBlock noiseStatus{nullptr};
  ToggleSwitch voiceTestToggle{nullptr}; TextBlock voiceTestNote{nullptr}; bool voiceTestShown=false;
  std::vector<app::VoiceDevice> shownInputs, shownOutputs;
  bool syncingDevices = false;

  std::unique_ptr<app::Service> service;
  HWND streamingNotice=nullptr,videoWindow=nullptr;
  ULONGLONG transmissionPendingUntil=0;
  WINDOWPLACEMENT videoPlacement{sizeof(WINDOWPLACEMENT)};
  std::map<std::string,std::function<void(bool)>> mediaVisibility;
  std::set<std::string> viewingScreens;
  std::set<std::pair<std::string,std::string>> videoKeys;
  DispatcherTimer idle{nullptr};
  DispatcherTimer resourceTimer{nullptr};
  DispatcherTimer meter{nullptr}; TextBlock measurements{nullptr};
  ULONGLONG lastActivity = GetTickCount64();
  app::State designState;
  app::State ReadState() { return designOutput.empty() ? service->Read() : designState; }
#ifndef HARMONY_PUBLISH
  winrt::Windows::Foundation::IAsyncAction ResourceAudit() {
    auto lifetime=get_strong();winrt::apartment_context ui;
    co_await winrt::resume_after(std::chrono::seconds(3));co_await ui;
    startup::Mark("hidden-login-settled");
    auto gpuState=gpu::VideoProcessorCacheSnapshot();
    {std::ofstream out(resource_meter::Output(),std::ios::app);
     out<<"{\"stage\":\"hidden-media-owners\",\"media_device_loaded\":"<<gpuState.device_loaded<<",\"media_device_users\":"<<gpuState.device_users<<",\"cached_texture_bytes\":"<<gpuState.texture_bytes<<",\"voice_loaded\":"<<resources::Read().parts[resources::Voice].loaded<<",\"dfn_loaded\":"<<resources::Read().parts[resources::Dfn].loaded<<"}\n";}

    designState.busy=false;designState.signedIn=true;designState.model.live=true;
    designState.model.rooms={{"fixture-room","Fixture room",{{"fixture-user","Fixture User"}}}};
    Render();root.UpdateLayout();startup::Mark("first-room-list");
    co_await winrt::resume_after(std::chrono::seconds(5));co_await ui;
    startup::Mark("hidden-list-settled");
    co_await winrt::resume_after(std::chrono::seconds(60));co_await ui;
    startup::Mark("hidden-list-idle-60s");window.Close();Exit();
  }
  winrt::Windows::Foundation::IAsyncAction DesignAudit() {
    if(screenDesign) {co_await DesignAuditW4();co_return;}
    if(callDesign) {co_await DesignAuditCall();co_return;}
    logf("design audit started");
    auto lifetime = get_strong();
    winrt::apartment_context ui;
    try {
      co_await winrt::resume_after(std::chrono::milliseconds(500));
      co_await ui;
      if(app::Environment(L"HARMONY_W8D")=="1") { co_await W8dAudit(); co_return; }
      if(app::Environment(L"HARMONY_W8C")=="1") { co_await W8cAudit(); co_return; }
      if(app::Environment(L"HARMONY_W8A")=="1") {
        co_await W8aAudit(); co_return;
      }
      if(app::Environment(L"HARMONY_PARITY7C")=="1") {
        co_await Parity7cDesign(); co_await ui;
        std::ofstream(designOutput/L"resultado.json") << "{\"passed\":true,\"fixture_only\":true}";
        window.Close(); Exit(); co_return;
      }
      logf("design audit login fields");
      user.Text(L"Tales"); user.Name(L"Identifier"); password.Name(L"Password");
      co_await SaveDesignSurface(root,designOutput,"nativo-entrar");
      co_await SaveDesignSurface(host,designOutput,"nativo-janela");
      co_await SaveDesignWindow(hwnd,designOutput);
      user.Focus(FocusState::Programmatic); root.UpdateLayout();
      auto clear=FindDesignPart(user,L"DeleteButton").as<Button>();
      VisualStateManager::GoToState(clear,L"PointerOver",false);
      co_await SaveDesignSurface(root,designOutput,"nativo-limpar-hover");
      Automation::Peers::ButtonAutomationPeer(clear).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
      check_bool(user.Text().empty());
      user.Text(L"Tales"); password.Password(L"Exemplo visual"); password.Focus(FocusState::Programmatic); root.UpdateLayout();
      auto reveal=FindDesignPart(password,L"RevealButton").as<Controls::Primitives::ToggleButton>();
      VisualStateManager::GoToState(password,L"ButtonVisible",false); root.UpdateLayout();
      VisualStateManager::GoToState(reveal,L"PointerOver",false);
      co_await SaveDesignSurface(root,designOutput,"nativo-senha-hover");
      Automation::Peers::ToggleButtonAutomationPeer(reveal).GetPattern(Automation::Peers::PatternInterface::Toggle).as<Automation::Provider::IToggleProvider>().Toggle();
      check_bool(reveal.IsChecked() && reveal.IsChecked().Value());
      co_await SaveDesignSurface(root,designOutput,"nativo-senha-botao");
      password.PasswordRevealMode(PasswordRevealMode::Visible);
      co_await SaveDesignSurface(root,designOutput,"nativo-senha-visivel");
      // Synthetic challenges never send a code or retain a real credential.
      designState.busy = true; Render();
      check_bool(!submit.IsEnabled() && unbox_value<hstring>(submit.Content()) == L"Entrando…");
      designState.busy = false; designState.needsCode = true; designState.method = "totp"; Render();
      check_bool(password.Password().empty() && factorLabel.Text() == L"Código do app" && bool(recoveryButton));
      co_await SaveDesignSurface(root, designOutput, "nativo-segundo-fator-app");
      code.Text(L"123456"); ToggleRecoveryCode();
      check_bool(code.Text().empty() && factorLabel.Text() == L"Código de recuperação");
      co_await SaveDesignSurface(root, designOutput, "nativo-segundo-fator-recuperacao");
      code.Text(L"example-only"); ToggleRecoveryCode();
      check_bool(code.Text().empty() && factorLabel.Text() == L"Código do app");
      designState.method = "email"; LoginPage(designState);
      check_bool(!recoveryButton && factorLabel.Text() == L"Código recebido por e-mail");
      co_await SaveDesignSurface(root, designOutput, "nativo-segundo-fator-email");
      designState.needsCode = false; Render();
      SettingsPage();
      co_await winrt::resume_after(std::chrono::milliseconds(250));
      co_await ui;
      co_await SaveDesignSurface(root,designOutput,"nativo-ajustes");
      // Installer visual reference: capture the actual shared dialog styles.
      AskLogout();
      co_await winrt::resume_after(std::chrono::milliseconds(400)); co_await ui;
      co_await SaveDesignSurface(logoutDialog,designOutput,"nativo-confirmar-saida");
      logoutDialog.Hide();
      co_await winrt::resume_after(std::chrono::milliseconds(200)); co_await ui;
      auto toggle=FindDesignPart(root,L"Startup").as<ToggleSwitch>();
      toggle.IsOn(true);
      co_await SaveDesignSurface(root,designOutput,"nativo-interruptor-ligado");
      toggle.IsOn(false); CopyStatus(true);
      co_await SaveDesignSurface(root,designOutput,"nativo-registro-copiado");
      if(AuditRealPointer()) {
        AuditWindowState(true);
        co_await winrt::resume_after(std::chrono::milliseconds(250));
        co_await ui;
        co_await SaveDesignSurface(root,designOutput,"nativo-ajustes-maximizado");
        AuditWindowState(false);
      } else logf("settings maximized picture: skipped (HARMONY_AUDIT_REAL_POINTER)");
      settings=false; if (meter) meter.Stop(); page=nullptr;
      designState.signedIn=true; designState.model.live=true;
      designState.model.rooms={{"voz-geral","geral",{{"design-visitor","Test User"}}}};
      Render();
      co_await winrt::resume_after(std::chrono::milliseconds(250));
      co_await ui;
      co_await SaveDesignSurface(root,designOutput,"nativo-salas");
      if (voiceDesign) {
        designState.model.rooms[0].people.insert(designState.model.rooms[0].people.begin(),{"design-me","Observador"});
        designState.voice.self="design-me";
        designState.voice.Join("voz-geral","geral"); designState.voice.connecting = false;
        designState.voice.speaking["design-visitor"] = false; Render();
        co_await SaveDesignSurface(root,designOutput,"nativo-voz");
        designState.voice.speaking["design-visitor"]=true; Render();
        co_await SaveDesignSurface(root,designOutput,"nativo-falando");
        auto person=FindDesignPart(root,L"VoicePerson").as<Button>(); check_bool(bool(person));
        Automation::Peers::ButtonAutomationPeer(person).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
        co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
        CloseProfile(); OpenVoiceVolume(person,"design-visitor",L"Test User");
        co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
        co_await SaveDesignSurface(auditVolume,designOutput,"nativo-volume");
        auto volume=FindDesignPart(auditVolume,L"VoiceVolumeSlider").as<Slider>(); volume.Value(0);
        check_bool(designState.voice.Volume("design-visitor")==0); co_await SaveDesignSurface(auditVolume,designOutput,"nativo-volume-zero");
        auto reset=FindDesignPart(auditVolume,L"VoiceVolumeReset").as<Button>();
        Automation::Peers::ButtonAutomationPeer(reset).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
        check_bool(volume.Value()==100 && designState.voice.Volume("design-visitor")==1); volumeFlyout.Hide();
        designState.voice.muted = true; Render();
        co_await SaveDesignSurface(root,designOutput,"nativo-mudo");
        designState.voice.deafened = true; Render();
        co_await SaveDesignSurface(root,designOutput,"nativo-nao-ouvir");
        designState.voice.reconnecting = true; Render();
        co_await SaveDesignSurface(root,designOutput,"nativo-reconectando");
        VoiceSettingsPage(); root.UpdateLayout();
        co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
        co_await SaveDesignSurface(root,designOutput,"nativo-ajustes-voz");
        auto dfn=FindDesignPart(root,L"DfnToggle").as<ToggleSwitch>();
        auto strength=FindDesignPart(root,L"DfnSlider").as<Slider>();
        check_bool(dfn.IsOn() && strength.IsEnabled() && strength.Value()==50);
        dfn.IsOn(false); check_bool(!strength.IsEnabled() && !designState.voice.dfn);
        auto noise=FindDesignPart(root,L"NoiseToggle").as<ToggleSwitch>();
        auto echo=FindDesignPart(root,L"EchoToggle").as<ToggleSwitch>();
        auto agc=FindDesignPart(root,L"AgcToggle").as<ToggleSwitch>();
        check_bool(noise.IsEnabled() && noise.IsOn() && echo.IsEnabled() && agc.IsEnabled());
        echo.IsOn(false); agc.IsOn(false); noise.IsOn(false);
        check_bool(!designState.voice.echo && !designState.voice.agc && !designState.voice.noise);
        co_await SaveDesignSurface(root,designOutput,"nativo-dfn-desligado");
        VisualStateManager::GoToState(echo,L"PointerOver",false);
        co_await SaveDesignSurface(root,designOutput,"nativo-filtro-hover");
        dfn.IsOn(true); strength.Value(73); check_bool(strength.IsEnabled() && designState.voice.strength==73);
        co_await SaveDesignSurface(root,designOutput,"nativo-dfn-forca73");
        auto thumb=FindDesignPart(strength,L"HorizontalThumb"); check_bool(thumb.ActualWidth()==16 && thumb.ActualHeight()==16);
        VisualStateManager::GoToState(strength,L"PointerOver",false);
        co_await SaveDesignSurface(root,designOutput,"nativo-slider-hover");
        auto test=FindDesignPart(root,L"VoiceTestToggle").as<ToggleSwitch>();
        check_bool(test.IsEnabled() && !test.IsOn());  // in a room it is the call's sidetone
        designState.voice.room.clear(); VoiceSettingsPage(); root.UpdateLayout();
        co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
        test=FindDesignPart(root,L"VoiceTestToggle").as<ToggleSwitch>(); check_bool(test.IsEnabled());
        test.IsOn(true); co_await SaveDesignSurface(root,designOutput,"nativo-ouvir-minha-voz");
        // The mention's tint must sit on the mention: at the start, mid-line,
        // after an emoji picture, twice in one line, and after a link (whose
        // "@" is the address's); then the link cards (Messages.tsx Embeds).
        CloseSettings();
        StackPanel mentions; mentions.Name(L"MentionTints"); mentions.Background(Brush(0x18181b)); mentions.Padding(Thickness{16,16,16,16}); mentions.Spacing(8);
        mentions.HorizontalAlignment(HorizontalAlignment::Left); mentions.VerticalAlignment(VerticalAlignment::Top); mentions.Width(520);
        for(auto text:{"@fulano cade meu amigo","oi @fulano tudo bem","\xF0\x9F\x98\x80 @fulano depois do emoji","@todos e @fulano juntos","olha https://x.com/@bob, @fulano"}) {
          app::ChatMessage m; m.body=text; mentions.Children().Append(MessageBody(m,"design"));
        }
        app::ChatMessage cards; cards.id=1;
        cards.embeds.push_back({"https://www.youtube.com/watch?v=x","video","YouTube","Um vídeo com um título comprido que ocupa duas linhas inteiras no cartão da prévia","Descrição do vídeo.","/api/previews/design",1280,720});
        cards.embeds.push_back({"https://g1.globo.com/x","link","g1","Notícia com figura pequena","Uma descrição que conta do que a página trata, em até três linhas.","/api/previews/design",200,200});
        if(auto extras=MessageExtras(cards,"design")) mentions.Children().Append(extras);
        Grid::SetColumnSpan(mentions,(std::max)(1,int(root.ColumnDefinitions().Size()))); Grid::SetRowSpan(mentions,(std::max)(1,int(root.RowDefinitions().Size())));
        root.Children().Append(mentions); root.UpdateLayout();
        co_await winrt::resume_after(std::chrono::milliseconds(250)); co_await ui;
        co_await SaveDesignSurface(mentions,designOutput,"nativo-mencoes");
        uint32_t mentionsAt=0; if(root.Children().IndexOf(mentions,mentionsAt)) root.Children().RemoveAt(mentionsAt);
      }
      std::ofstream(designOutput/L"resultado.json") << "{\"passed\":true,\"clear_invoked\":true,\"fixture_only\":true}";
    } catch(hresult_error const& e) {
      std::ofstream(designOutput/L"resultado.json") << "{\"passed\":false,\"hresult\":" << e.code().value << '}';
    }
    if(meter) meter.Stop(); window.Close(); Exit();
  }
#endif
  ~App() {
    if (service) std::thread([owned = std::move(service)] { owned->Stop(); }).detach();
    if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
    if (control) DestroyWindow(control);
    if (icon) DestroyIcon(icon);
  }
  void OnLaunched(LaunchActivatedEventArgs const&) {
    startup::Mark("winui-launched");
    Resources().MergedDictionaries().Append(XamlControlsResources());
    std::ifstream theme(app::Executable().parent_path()/L"assets/harmony.xaml");
    std::string themeXaml((std::istreambuf_iterator<char>(theme)),{});
    IInspectable styles;
    try { styles = Markup::XamlReader::Load(net::Wide(themeXaml)); }
    catch (hresult_error const& failure) {
      // Only the bundled theme is parsed here, never account or user input.
      logf("theme could not be loaded: %s",to_string(failure.message()).c_str()); throw;
    }
    Resources().MergedDictionaries().Append(styles.as<ResourceDictionary>());
    startup::Mark("resources-loaded");
#ifndef HARMONY_PUBLISH
    if (!uiTestOutput.empty()) {
      bool released = true, senderCorrect = true;
      for (int i=0; i<20; ++i) {
        bool value = false;
        auto toggle = StartupToggle(false, [&](bool enabled) { value = enabled; });
        auto weak = make_weak(toggle);
        toggle.IsOn(true); senderCorrect = senderCorrect && value;
        toggle = nullptr;
        released = released && !weak.get();
      }
      // Runs before creating a window: no activation, real input or account.
      // A failed check is written down, not left as a crash with no name.
      std::string stage="icons", failure;
      try {
      for (auto const& entry : std::filesystem::directory_iterator(app::Executable().parent_path()/L"assets")) {
        if (entry.path().extension()!=L".xaml") continue;
        if (entry.path().stem()==L"harmony") continue; // the theme, not a glyph
        for (double size : {12.,14.,16.,18.,20.,22.,28.}) {
          auto name=net::Narrow(entry.path().stem().wstring());
          stage="icon "+name+" at "+std::to_string(int(size));
          auto a=VoiceIcon(name,0xa1a1aa,size).as<Canvas>();
          auto b=VoiceIcon(name,0xfcfcfc,size).as<Canvas>();
          auto first=a.Children().GetAt(0).as<Shapes::Path>();
          auto second=b.Children().GetAt(0).as<Shapes::Path>();
          // WinUI refuses one Geometry under two parents ("Element is already
          // the child of another element"), so every use owns its own.
          check_bool(first!=second && first.Data()!=second.Data());
          auto data=first.Data(); IconInk(a,0xfcfcfc);
          check_bool(first.Data()==data && a.Children().GetAt(0).as<Shapes::Path>()==first);
          check_bool(first.Fill().as<SolidColorBrush>().Color().R==0xfc);
          IconInk(a,0xa1a1aa);
          check_bool(second.Fill().as<SolidColorBrush>().Color().R==0xfc);
          if(name=="pin" && size==16) check_bool(!data.Transform());
        }
      }
      stage="people"; CheckPeoplePresence();
      } catch (hresult_error const& e) { failure=stage+": "+to_string(e.message()); }
      std::ofstream out(uiTestOutput);
      out << "{\"passed\":" << (released && senderCorrect && failure.empty() ? "true" : "false")
          << ",\"failure\":\"" << failure << "\""
          << ",\"released_controls\":" << (released ? 20 : 0) << "}";
      out.close(); Exit(); return;
    }
#endif
    window = Window(); window.Title(L"harmony");
    startup::Mark("window-created");
    root = Grid(); root.RequestedTheme(ElementTheme::Dark); root.Background(Brush(0x060607));
    host=Grid(); host.RequestedTheme(ElementTheme::Dark); host.Background(Brush(0x060607));
    RowDefinition caption; caption.Height(GridLength{32,GridUnitType::Pixel}); host.RowDefinitions().Append(caption);
    RowDefinition content; content.Height(GridLength{1,GridUnitType::Star}); host.RowDefinitions().Append(content);
    Grid titleBar; titleBar.Height(32); titleBar.Name(L"HarmonyTitleBar"); titleBar.Background(Brush(0x060607));
    // The mark in the corner where the title used to be (25/10/2026), 18 in
    // a 32 bar; in the middle, small and grey, where you are (titlebar-ui.h,
    // 02/11/2026: the empty bar the owner had asked for on 22/10 looked bare).
    {auto mark=HarmonyMark(18).as<FrameworkElement>(); mark.Name(L"HarmonyMark");
     mark.Margin(Thickness{8,7,0,7}); mark.HorizontalAlignment(HorizontalAlignment::Left); mark.VerticalAlignment(VerticalAlignment::Center);
     Automation::AutomationProperties::SetName(mark,L"harmony"); titleBar.Children().Append(mark);}
    host.Children().Append(titleBar); Grid::SetRow(root,1); host.Children().Append(root);
    window.Content(host); window.ExtendsContentIntoTitleBar(true); window.SetTitleBar(titleBar);
    host.AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, Input::KeyRoutedEventArgs const& e) {
      if (e.Key() == winrt::Windows::System::VirtualKey::Escape && fullStage && !exiting) { auto& st = *fullStage; LeaveCallFull(); RedrawStage(st); e.Handled(true); }
    })), true);
    auto systemCaption=window.AppWindow().TitleBar();
    systemCaption.ButtonBackgroundColor(Brush(0x060607).Color());
    systemCaption.ButtonInactiveBackgroundColor(Brush(0x060607).Color());
    systemCaption.ButtonForegroundColor(Brush(0xfcfcfc).Color());
    systemCaption.ButtonInactiveForegroundColor(Brush(0xa1a1aa).Color());
    systemCaption.ButtonHoverBackgroundColor(Brush(0x27272a).Color());
    systemCaption.ButtonPressedBackgroundColor(Brush(0x2e2e31).Color());
    check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
    if (AuditingPersonal())
      SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    window.AppWindow().ResizeClient({960,680});
    if(!designOutput.empty() || !callOutput.empty()) {
      MONITORINFO target{sizeof target};GetMonitorInfoW(MonitorFromPoint({2060,600},MONITOR_DEFAULTTONEAREST),&target);
      SetWindowPos(hwnd,nullptr,target.rcWork.left+32,target.rcWork.top+32,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    SetWindowSubclass(hwnd, WindowProc, 1, reinterpret_cast<DWORD_PTR>(this));
    HoldCursorInChildren(hwnd);
    BuildCaption(titleBar);
    if(!Measuring() || PlacementAudit()) {
      app::WindowState saved;
      if(app::ReadWindowState(app::DataDirectory()/L"window-state.txt",saved)) {
        app::RestoreWindowState(hwnd,saved);startupMaximized=saved.maximized;
      }
    }
    placementReady=true;
    WNDCLASSW cls{}; cls.lpfnWndProc = ControlProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = ControlClass;
    RegisterClassW(&cls);
    control = CreateWindowExW(0, ControlClass, designOutput.empty() && chatOutput.empty() && callOutput.empty() ? ControlClass : L"Harmony.Native.Windows.Measuring", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated"); if(!resourceAudit) AddTray();
    startup::Mark("window-configured");
    auto base = app::Environment(L"HARMONY_URL");
    if (base.empty()) base = "https://harmony.taleshernandes.com";
    auto weak = get_weak(); auto dispatcher = window.DispatcherQueue();
    // Notifications invalidate the current view; Render reads the latest state
    // and consumes the chat journal. Keep only one pending refresh so bursts
    // of presence/voice events do not queue redundant work ahead of input.
    auto refreshPending = std::make_shared<std::atomic<bool>>(false);
    service = std::make_unique<app::Service>(base, [weak, dispatcher, refreshPending] {
      if (refreshPending->exchange(true)) return;
      if (!dispatcher.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak, refreshPending] {
        // Clear before reading: a change during Render must get another turn.
        refreshPending->store(false);
        if (auto self = weak.get()) if (!self->exiting) self->Render();
      })) refreshPending->store(false);
    }, chatOutput.empty() && designOutput.empty() && callOutput.empty()); // a measuring run never writes a session, drafts or settings over the owner's
    startup::Mark("service-created");
    service->OnMessageSound([weak, dispatcher] {
      dispatcher.TryEnqueue([weak] { if (auto self=weak.get(); self && !self->exiting && chatOutput.empty() && designOutput.empty() && callOutput.empty()) PlayMessageSound(); });
    });
    if (updater_) service->OnAppRelease([updater = updater_] { logf("update announced by the server"); updater->ReleaseAnnounced(); });
    service->OnNotice([weak, dispatcher](app::Notice notice) {
      dispatcher.TryEnqueue([weak, notice = std::move(notice)] { if (auto self = weak.get()) if (!self->exiting) self->ShowNotice(notice); });
    });
    if(!resourceAudit) RegisterNotices();
    UnhandledException([](auto const&, UnhandledExceptionEventArgs const& args) {
      logf("unhandled UI failure HRESULT %ld", args.Exception().value);
    });
    Render();
    startup::Mark("first-view");
    // Only the installer enables startup by default. Merely running a build
    // must not change the owner's Run key or override a disabled preference.
    if (AuditingPersonal()) {
      window.AppWindow().Move({-20000, -20000});
      if(resourceAudit) window.AppWindow().Hide(); else window.AppWindow().Show(false);
    } else if (!startHidden || !trayAdded) Open();
    startup::Mark("window-shown");
    root.KeyDown([this](auto const&, Input::KeyRoutedEventArgs const& args) {
      lastActivity = GetTickCount64();
      if (args.Key() == winrt::Windows::System::VirtualKey::Enter && !settings && !showingRooms && !searchOpen) { Submit(); args.Handled(true); }
    });
    root.PointerPressed([this](auto const&, auto const&) { lastActivity = GetTickCount64(); });
    root.AddHandler(UIElement::PointerMovedEvent(),box_value(Input::PointerEventHandler(
      [](auto const&,Input::PointerRoutedEventArgs const& e) { HarmonyInputAt(e.OriginalSource()); })),true);
    root.PointerWheelChanged([this](auto const&, auto const&) { lastActivity = GetTickCount64(); });
    idle = DispatcherTimer(); idle.Interval(std::chrono::seconds(5));
    idle.Tick([this](auto const&, auto const&) { ReportPresence(); }); idle.Start();
    resourceTimer=DispatcherTimer();resourceTimer.Interval(std::chrono::seconds(2));
    resourceTimer.Tick([this](auto const&,auto const&){if(service && !exiting){auto timing=perf::Get("diagnostics.ui-resources").Begin();service->ChatModel().PublishResources();{auto& r=resources::State();std::lock_guard lock(r.mutex);r.decodedCount=decodedBudget.Count();r.decodedBytes=decodedBudget.Bytes();}timing.Done();
#ifndef HARMONY_PUBLISH
      W8dLivePoll();
#endif
    }});resourceTimer.Start();
#ifndef HARMONY_PUBLISH
    if (resourceAudit) ResourceAudit();
    else if (!chatOutput.empty()) { std::filesystem::create_directories(chatOutput); ChatAudit(); }
    else if (!callOutput.empty()) { std::filesystem::create_directories(callOutput); CallAudit(); }
    else if (designOutput.empty()) service->Restore();
    else { designState.busy=false; Render(); DesignAudit(); }
#else
    service->Restore();
#endif
    logf("window ready");
  }
  // WM_SETCURSOR reaches the XAML child under the pointer, not this window:
  // every child answers with the held shape while the pointer stays put.
  static LRESULT CALLBACK CursorProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT && HarmonyHoldCursor()) return TRUE;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, CursorProc, 7);
    return DefSubclassProc(window, message, w, l);
  }
  static void HoldCursorInChildren(HWND parent) {
    SetWindowSubclass(parent, CursorProc, 7, 0);
    EnumChildWindows(parent, [](HWND child, LPARAM) -> BOOL { SetWindowSubclass(child, CursorProc, 7, 0); return TRUE; }, 0);
  }
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<App*>(data);
    if (message == WM_CLOSE && !self->exiting) {
      self->SavePlacement();
      if (!self->trayAdded) self->AddTray();
      if (self->trayAdded) { ShowWindow(window, SW_HIDE); self->RefreshMediaVisibility(false); if (self->meter) self->meter.Stop(); self->ReportPresence(); return 0; }
      if(self->error) self->error.Text(L"Não foi possível criar o ícone na bandeja. Use Sair nos Ajustes para fechar.");
      return 0;
    }
    if(message==WM_EXITSIZEMOVE)self->SavePlacement();
    if(message==WM_NCMOUSEMOVE || message==WM_NCMOUSELEAVE || message==WM_MOUSEMOVE || message==WM_MOUSELEAVE)
      self->CaptionHoverAtPointer();
    if (message == WM_POWERBROADCAST && w == PBT_APMRESUMEAUTOMATIC) {
      if (self->service) self->service->RecoverVoice("resume");
      return TRUE;
    }
    if(message==WM_DPICHANGED) self->window.DispatcherQueue().TryEnqueue([weak=self->get_weak()] {
      if(auto app=weak.get()) app->CaptionRegions();
    });
    if(message==WM_SHOWWINDOW) HoldCursorInChildren(window);
    if(message==WM_SHOWWINDOW||message==WM_SIZE) {
      bool shown=message==WM_SHOWWINDOW?w!=0:IsWindowVisible(window)&&!IsIconic(window);
      self->RefreshMediaVisibility(shown);
      self->window.DispatcherQueue().TryEnqueue([weak=self->get_weak()] { if(auto app=weak.get(); app && !app->exiting) app->SyncPip(); });
      // The drafts go to disk while the window is away and leave it when it
      // comes back, the web's rule (chat.cpp Foreground).
      if(self->service&&self->chatShown!=shown) { self->chatShown=shown; self->service->ChatForeground(shown); }
    }
    if (message == WM_ACTIVATE) {
      if (self->service) self->service->WindowSeen(LOWORD(w) != WA_INACTIVE && IsWindowVisible(window) && !IsIconic(window));
      self->lastActivity = GetTickCount64(); self->ReportPresence();
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, WindowProc, 1);
    return DefSubclassProc(window, message, w, l);
  }
  static LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, w, l);
    if (message == OpenMessage) { self->Open(); return 0; }
    if (message == self->taskbarCreated && self->taskbarCreated) { self->trayAdded = false; self->AddTray(); return 0; }
    if (message == TrayMessage) {
      UINT event = LOWORD(l);
      if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) self->Open();
      if (event == NIN_BALLOONUSERCLICK) self->OpenNotice();
      if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
        // Shell callbacks are synchronous cross-process messages. XAML popup
        // activation must run after returning to the dispatcher, outside that
        // SendMessage stack (including when the window was hidden).
        self->window.DispatcherQueue().TryEnqueue([weak=self->get_weak()] {
          if(auto app=weak.get(); app && !app->exiting) app->OpenTrayMenu();
        });
      }
      return 0;
    }
    return DefWindowProcW(window, message, w, l);
  }
#include "tray-ui.h"
#ifndef HARMONY_PUBLISH
#include "w8a-audit.h"
#define HARMONY_W8C_CAMERA
#include "w8c-audit.h"
#include "w8d-audit.h"
#endif
  void AddTray() {
    tray = {}; tray.cbSize = sizeof tray; tray.hWnd = control; tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = TrayMessage; tray.hIcon = icon;
    wcscpy_s(tray.szTip, L"harmony");
    trayAdded = Shell_NotifyIconW(NIM_ADD, &tray);
    if (trayAdded) { tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray); }
    logf("tray status %d", trayAdded);
  }
#ifndef HARMONY_PUBLISH
  bool auditPlaced=false;
#endif
  void Open() {
    CloseTrayMenu();
#ifndef HARMONY_PUBLISH
    if(Measuring() && !AuditRealPointer()) {
      // Once only: a later Open must leave a restored placement where it is.
      if(!auditPlaced) {ChatAuditPosition();auditPlaced=true;}
      if(startupMaximized) {AuditWindowState(true);startupMaximized=false;}
      else if(IsIconic(hwnd)) AuditWindowState(false);
      ShowWindow(hwnd,SW_SHOWNA);RefreshMediaVisibility(true);
      if(settings && meter)meter.Start();
      lastActivity=GetTickCount64();ReportPresence();logf("audit window opened without activation");return;
    }
#endif
    window.Activate();
    if(startupMaximized) {ShowWindow(hwnd,SW_MAXIMIZE);startupMaximized=false;}
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(hwnd);
    RefreshMediaVisibility(true);
    if (settings && meter) meter.Start();
    lastActivity = GetTickCount64(); ReportPresence();
    logf("window opened");
  }
  void Quit() {
    if (exiting) return;
    SavePlacement();
    StopPip(); if(pipTimer) pipTimer.Stop();
    CloseTrayMenu(); if(trayPopup) { DestroyWindow(trayPopup); trayPopup=nullptr; }
    exiting = true; if(pipWindow) pipWindow.Close(); if(resourceTimer)resourceTimer.Stop(); if(videoWindow) {DestroyWindow(videoWindow);videoWindow=nullptr;} TransmissionNotice(false); root.IsHitTestVisible(false); if (idle) idle.Stop(); if (meter) meter.Stop();
    // Cleanup owns the service until both workers have stopped. The UI never
    // joins a network thread; keep pumping messages until cleanup completes.
    auto dispatcher = window.DispatcherQueue(); auto self = get_strong();
    std::thread([owned = std::move(service), dispatcher, self] {
      owned->Stop();
      dispatcher.TryEnqueue([self] { self->window.Close(); self->Exit(); });
    }).detach();
    if (trayAdded) { Shell_NotifyIconW(NIM_DELETE, &tray); trayAdded = false; }
    Ringing(false); UnregisterNotices();
  }
  void ReportPresence() {
    if (service) service->WindowSeen(IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd);
    if (!service || !service->Read().signedIn) return;
    bool active = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
    auto state = active ? (GetTickCount64() - lastActivity >= 300000 ? "away" : "active") : "hidden";
    service->Presence(state);
  }
  static void NegativeInk(DependencyObject const& node,uint32_t color) {
    if(auto text=node.try_as<TextBlock>()) text.Foreground(Brush(color));
    if(auto path=node.try_as<Shapes::Path>()) path.Fill(Brush(color));
    for(int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i) NegativeInk(Media::VisualTreeHelper::GetChild(node,i),color);
  }
  void NegativeButton(Button const& button) {
    button.Style(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
    button.Loaded([](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();button.Background(Brush(0xdb3b3e)); button.Foreground(Brush(0xfcfcfc)); if(auto content=button.Content().try_as<DependencyObject>()) NegativeInk(content,0xfcfcfc);});
    button.PointerEntered([](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>(); if(auto content=button.Content().try_as<DependencyObject>()) NegativeInk(content,0xfcfcfc); });
    button.PointerExited([](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>(); if(auto content=button.Content().try_as<DependencyObject>()) NegativeInk(content,0xfcfcfc); });
  }
  Button Action(std::wstring const& label, std::function<void()> clicked) {
    Button button; HarmonyPointer(button); button.Style(Resources().Lookup(box_value(L"HarmonyButton")).as<Style>());
    auto normal=Resources().Lookup(box_value(L"HarmonyButton")).as<Style>();
    auto danger=Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>();
    auto wasNegative=std::make_shared<bool>(false);
    button.RegisterPropertyChangedCallback(ContentControl::ContentProperty(),[this,normal,danger,wasNegative](auto const& sender,auto const&) {
      auto value=sender.template as<Button>(); auto text=unbox_value_or<hstring>(value.Content(),hstring{});
      std::wstring label(text);
      bool negative=label.find(L"Cancelar")!=std::wstring::npos || label.find(L"Sair")!=std::wstring::npos || label.find(L"Recusar")!=std::wstring::npos || label.find(L"Parar")!=std::wstring::npos || label.find(L"Desconectar")!=std::wstring::npos || label.find(L"Remover")!=std::wstring::npos || label.find(L"Desligar")!=std::wstring::npos;
      if(negative!=*wasNegative) { value.Style(negative?danger:normal);
        if(negative) {value.Background(Brush(0xdb3b3e));value.Foreground(Brush(0xfcfcfc));}
        else {value.ClearValue(Control::BackgroundProperty());value.ClearValue(Control::ForegroundProperty());}
        *wasNegative=negative; }
    });
    button.Loaded([wasNegative](auto const& eventSender,auto const&) { auto button = eventSender.template as<Button>();if(*wasNegative) {button.Background(Brush(0xdb3b3e));button.Foreground(Brush(0xfcfcfc));}});
    button.Content(box_value(label)); button.HorizontalAlignment(HorizontalAlignment::Left);
    button.Click([this,clicked](auto const&, auto const&) { if (!exiting) clicked(); }); return button;
  }
  void OpenVoiceVolume(FrameworkElement target, std::string const& who, std::wstring const& name, std::optional<winrt::Windows::Foundation::Point> point = std::nullopt) {
            if(who==ReadState().voice.self) return;
            Flyout flyout; StackPanel content; content.Width(392); content.Spacing(8);
            content.Children().Append(Text(L"VOLUME — SÓ PARA VOCÊ",12,0xa1a1aa));
            Border control; control.Height(44); control.Padding(Thickness{12,0,12,0}); control.Background(Brush(0x232325)); control.CornerRadius(CornerRadius{8,8,8,8});
            Grid row; for(auto width:{76.,240.,52.}) { ColumnDefinition column; column.Width(GridLength{width,GridUnitType::Pixel}); row.ColumnDefinitions().Append(column); }
            auto label=Text(L"Voz",13); label.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(label);
            Slider slider; HarmonyPointer(slider); slider.Name(L"VoiceVolumeSlider"); slider.Width(240); slider.Height(20); slider.MinHeight(20); slider.Minimum(0); slider.Maximum(200); slider.StepFrequency(1); slider.Value(ReadState().voice.Volume(who)*100); Grid::SetColumn(slider,1); row.Children().Append(slider);
            Automation::AutomationProperties::SetName(slider, L"Volume de " + name);
            auto percent=Action(std::to_wstring(int(ReadState().voice.Volume(who)*100))+L"%",[weak=make_weak(slider)] { if(auto value=weak.get()) value.Value(100); });
            percent.Name(L"VoiceVolumeReset"); percent.Height(18); percent.MinHeight(18); percent.MinWidth(0); percent.Padding(Thickness{0}); percent.FontSize(13); percent.FontWeight(winrt::Windows::UI::Text::FontWeight{500}); percent.Foreground(Brush(0xa1a1aa)); percent.Background(nullptr); percent.VerticalAlignment(VerticalAlignment::Center); percent.HorizontalAlignment(HorizontalAlignment::Right); Grid::SetColumn(percent,2); row.Children().Append(percent);
            slider.ValueChanged([this, who, percent](auto const&, auto const& args) {
              percent.Content(box_value(std::to_wstring(int(args.NewValue()))+L"%"));
              if(designOutput.empty()) service->VoiceVolume(who,args.NewValue()/100); else designState.voice.Volume(who,args.NewValue()/100);
            }); control.Child(row); content.Children().Append(control); flyout.Content(content); auditVolume=content; volumeFlyout=flyout;
            ShowAtPointer(flyout, target, point);
  }
  #include "titlebar-ui.h"
  #include "media-ui.h"
  #include "camera-menu.h"
  #include "file-ui.h"
  bool showingCall = false;
  bool callTilesAway = false; // the room's tiles were released while a text channel is read
  #include "chat-ui.h"
  #include "update-ui.h"
  #include "diagnostic-ui.h"
  #include "network-ui.h"
  #include "photo-ui.h"
  #include "person-ui.h"
  #include "roles-ui.h"
  static bool AuditingPersonal() {
#ifdef HARMONY_PUBLISH
    return false;
#else
    return resourceAudit || !app::Environment(L"HARMONY_AUDIT_PERSONAL").empty();
#endif
  }
  #include "personal-ui.h"
  #include "personal-audit.h"
  #include "account-ui.h"
  #include "security-ui.h"
  #include "email-ui.h"
  #include "login-ui.h"
  #include "signup-ui.h"
  #include "call-ui.h"
#ifndef HARMONY_PUBLISH
  #include "parity5-audit.h"
  #include "parity6-audit.h"
  #include "parity7-audit.h"
  #include "parity7c-audit.h"
  #include "call-audit.h"
#endif
  // ---------------------------------------------------------------------------
  // The open call, as the web's Call.tsx draws it at 1280x800 (measured side by
  // side, windows/app/arquivo/medidas-web.md): the header's island with the room's name,
  // the blocks on the page's black, and the bar's island under them. The people
  // column folds away on the way in, as on the web, and the header's button
  // brings it back.
  // ---------------------------------------------------------------------------
  bool callPeople = false;
  std::string videoWho; // whose screen the separate window is showing
  std::string watchRoom; // the room viewingScreens belongs to: a new session starts watching nobody, as on the web
  void WatchScreen(std::string const& who) { viewingScreens.insert(who); callSignature.clear(); Render(); }
  // Call.tsx's two layouts: the grid, and focus — pressing a block makes it
  // the big one with everybody else in a strip under it, which folds away;
  // pressing the big one goes back to the grid. Keys are "p:<user>" for a
  // person and "s:<user>" for a screen. Full screen is the block in focus
  // filling the monitor inside this window (the owner, 25/10/2026: the
  // separate window it used to open "nada a ver"), and Esc or the ✕ bring
  // the focus back. A room and a direct call draw the same stage (the owner,
  // 25/10/2026: the 1:1 had stayed as code 10 drew it while the room got the
  // live, full screen and the floating cameras); each keeps its own focus.
  struct CallStage { bool direct = false; std::string focus, full; bool strip = true; std::set<std::string> tucked; };
  CallStage roomStage, directStage{true};
  CallStage* fullStage = nullptr;   // the one in full screen, if any
  DispatcherTimer fullIdle{nullptr};
  winrt::weak_ref<Grid> fullChrome;   // the overlay the idle timer fades: one handler for every redraw
  // Folded strip: the cameras float over the big block (Call.tsx Floating).
  // Where, as a fraction of the room they move in (1,1 the bottom right), and
  // how big, as a scale of 160x90 pulled by their edges; the cameras taken off
  // with their x come back when the strip does.
  double floatX = 1, floatY = 1, floatScale = 1;
  // Full screen hides the mouse with the controls, as a video player does;
  // ShowCursor counts, so it is only ever one step away from shown.
  bool cursorHidden = false; POINT fullPointer{};
  void HideCursor(bool hide) { if (hide == cursorHidden) return; cursorHidden = hide; ShowCursor(hide ? FALSE : TRUE); }
  void RedrawStage(CallStage const& st) { if (st.direct) directSignature.clear(); else callSignature.clear(); Render(); }
  void FocusSeat(CallStage& st, std::string const& key) { if (!st.full.empty() || exiting) return; st.focus = st.focus == key ? std::string() : key; RedrawStage(st); }
  void EnterCallFull(CallStage& st, std::string const& key) {
    if (exiting) return;
    st.focus = st.full = key; fullStage = &st;
    try { window.AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen); } catch (winrt::hresult_error const& e) { logf("call full screen: presenter refused 0x%08lx", static_cast<unsigned long>(e.code().value)); }
    try { host.RowDefinitions().GetAt(0).Height(GridLength{0, GridUnitType::Pixel}); } catch (winrt::hresult_error const& e) { logf("call full screen: caption row 0x%08lx", static_cast<unsigned long>(e.code().value)); }
    try { RedrawStage(st); } catch (winrt::hresult_error const& e) { logf("call full screen: drawing failed 0x%08lx", static_cast<unsigned long>(e.code().value)); throw; }
  }
  // Without Render: the drawing itself calls it when the block went away.
  void LeaveCallFull() {
    if (!fullStage) return;
    fullStage->full.clear(); fullStage = nullptr; if (fullIdle) fullIdle.Stop(); HideCursor(false);
    window.AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::Overlapped);
    host.RowDefinitions().GetAt(0).Height(GridLength{32, GridUnitType::Pixel});
  }
  // "som da live" (Call.tsx Slider, SCREEN_AUDIO): the sound of someone's
  // screen, apart from their voice, only for you.
  FrameworkElement LiveVolume(std::string const& who, std::wstring const& name) {
    Border box; box.Name(L"LiveVolume"); box.Height(36); box.Width(220); box.Padding(Thickness{12, 0, 12, 0}); box.CornerRadius(CornerRadius{8, 8, 8, 8}); box.Background(Brush(Background));
    Grid row; row.ColumnSpacing(8);
    for (auto w : {GridLength{0, GridUnitType::Auto}, GridLength{1, GridUnitType::Star}, GridLength{40, GridUnitType::Pixel}}) { ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c); }
    auto key = who + "/screen_audio";
    auto glyph = VoiceIcon("volume", Foreground, 16).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph);
    Slider slider; HarmonyPointer(slider); slider.Name(L"LiveVolumeSlider"); slider.Minimum(0); slider.Maximum(200); slider.StepFrequency(1); slider.VerticalAlignment(VerticalAlignment::Center);
    slider.Value(ReadState().voice.Volume(key) * 100); Grid::SetColumn(slider, 1); row.Children().Append(slider);
    auto percent = Line(std::to_wstring(int(ReadState().voice.Volume(key) * 100)) + L"%", 13, Muted, 500); percent.VerticalAlignment(VerticalAlignment::Center); percent.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(percent, 2); row.Children().Append(percent);
    slider.ValueChanged([this, key, percent](auto const&, auto const& args) {
      percent.Text(std::to_wstring(int(args.NewValue())) + L"%");
      if (designOutput.empty()) service->VoiceVolume(key, args.NewValue() / 100); else designState.voice.Volume(key, args.NewValue() / 100);
    });
    Automation::AutomationProperties::SetName(slider, L"Som da live de " + name); ToolTipService::SetToolTip(box, box_value(L"Som da live — só para você"));
    box.Child(row); return box;
  }
  void StopWatching(std::string const& who) {
    viewingScreens.erase(who);
    if (videoWindow && videoWho == who) SendMessageW(videoWindow, WM_CLOSE, 0, 0);
    callSignature.clear(); Render();
  }
  // A 44 square of the bar (CallKit.tsx ROUND): default ground and an icon of
  // 20. `rest` is the icon's colour; an icon that is muted at rest turns to the
  // foreground under the pointer, as the web's hover:text-foreground does.
  Button CallSquare(std::string const& icon, uint32_t rest, uint32_t ground, uint32_t hover, std::wstring const& label, std::function<void()> clicked) {
    auto b = Square(icon, label, 44, 20, rest, hover, std::move(clicked));
    b.Background(Brush(ground)); b.Resources().Insert(box_value(L"ButtonBackgroundDisabled"), Brush(ground));
    if (rest == Muted) {
      b.PointerEntered([weak = make_weak(b), icon](auto const&, auto const&) { if (auto x = weak.get()) IconInk(x.Content(), Foreground); });
      b.PointerExited([weak = make_weak(b), icon](auto const&, auto const&) { if (auto x = weak.get()) IconInk(x.Content(), Muted); });
    }
    return b;
  }
  Button RoomScreenButton(bool sharing, double size) {
    auto button = CallSquare(sharing ? "display-pulse" : "display", Foreground,
      sharing ? Accent : Default, sharing ? 0x3592f9 : 0x2e2e31,
      sharing ? L"Trocar a tela transmitida" : L"Compartilhar tela", [this] { ScreenPicker(); });
    button.Width(size); button.Height(size); button.MinWidth(size); button.MinHeight(size);
    return button;
  }
  // The chip on a block (CallKit.tsx CHIP): 24 tall, 8 in from the block's
  // corner, the page's black behind 13/18 at 500, its icons 12 and 6 apart.
  Border CallChip(std::wstring const& text, std::vector<std::pair<std::string, uint32_t>> const& icons) {
    Border chip; chip.Name(L"CallName"); chip.Height(24); chip.Padding(Thickness{8, 0, 8, 0}); chip.CornerRadius(CornerRadius{8, 8, 8, 8});
    chip.Background(Brush(Background)); chip.Margin(Thickness{8, 8, 8, 8});
    chip.HorizontalAlignment(HorizontalAlignment::Left); chip.VerticalAlignment(VerticalAlignment::Bottom); chip.IsHitTestVisible(false);
    Grid row; row.ColumnSpacing(6);
    for (auto const& [name, color] : icons) {
      ColumnDefinition c; c.Width(GridLength{0, GridUnitType::Auto}); row.ColumnDefinitions().Append(c);
      auto glyph = VoiceIcon(name, color, 12).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center);
      Grid::SetColumn(glyph, int(row.ColumnDefinitions().Size()) - 1); row.Children().Append(glyph);
    }
    ColumnDefinition c; c.Width(GridLength{1, GridUnitType::Star}); row.ColumnDefinitions().Append(c);
    auto label = Line(text, 13, Foreground, 600); /* 500 reads thin on Windows at 13 px (owner, 22/10/2026) */ label.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(label, int(row.ColumnDefinitions().Size()) - 1); row.Children().Append(label);
    chip.Child(row); return chip;
  }
  // The same chip as a button, top right (the screen's "Parar de assistir").
  Button CallChipButton(std::string const& icon, std::wstring const& text, std::function<void()> clicked) {
    Button b; HarmonyPointer(b); b.Height(24); b.MinHeight(0); b.MinWidth(0); b.Padding(Thickness{8, 0, 8, 0}); b.CornerRadius(CornerRadius{8, 8, 8, 8});
    b.BorderThickness(Thickness{0, 0, 0, 0}); b.Background(Brush(Background));
    b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
    StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(6);
    if (!icon.empty()) { auto glyph = VoiceIcon(icon, Foreground, 12).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph); }
    auto label = Line(text, 13, Foreground, 500); label.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(label);
    b.Content(row); Automation::AutomationProperties::SetName(b, text);
    b.Click([this, clicked](auto const&, auto const&) { if (!exiting) clicked(); });
    return b;
  }
  // The floating cameras as one stack over the big block (Call.tsx Floating):
  // a column, two columns past three, 8 apart and 12 from the picture's edge.
  // Dragging it moves it (a press that did not travel is a press), its edges
  // and corners resize it keeping its shape with the far corner still. Returns
  // what places it once the big block's rectangle is known.
  std::function<void(winrt::Windows::Foundation::Rect)> FloatingCameras(Canvas body, std::vector<FrameworkElement> const& cams) {
    static constexpr double Margin = 12, SizeMin = 0.6, SizeMax = 2.5;
    int n = int(cams.size()), columns = n > 3 ? 2 : 1, rows = (n + columns - 1) / columns;
    Grid stack; stack.Name(L"FloatingCameras"); stack.RowSpacing(8); stack.ColumnSpacing(8); stack.UseLayoutRounding(false);
    for (int c = 0; c < columns; ++c) { ColumnDefinition d; d.Width(GridLength{160, GridUnitType::Pixel}); stack.ColumnDefinitions().Append(d); }
    for (int r = 0; r < rows; ++r) { RowDefinition d; d.Height(GridLength{90, GridUnitType::Pixel}); stack.RowDefinitions().Append(d); }
    for (int i = 0; i < n; ++i) { Grid::SetColumn(cams[i], i % columns); Grid::SetRow(cams[i], i / columns); stack.Children().Append(cams[i]); }
    struct Place { winrt::Windows::Foundation::Rect big{}; double w0 = 0, h0 = 0; bool pressed = false, dragged = false; winrt::Windows::Foundation::Point from{}; double px = 0, py = 0;
      std::string edge; double gl = 0, gt = 0, gw = 0, gh = 0; };
    auto at = std::make_shared<Place>(); at->w0 = columns * 160.0 + (columns - 1) * 8; at->h0 = rows * 90.0 + (rows - 1) * 8;
    auto weakStack = make_weak(stack); auto weakBody = make_weak(body);
    // The biggest it may be: never wider or taller than the picture, margins in.
    auto limit = [at] { return (std::min)(SizeMax, (std::min)((at->big.Width - 2 * Margin) / at->w0, (at->big.Height - 2 * Margin) / at->h0)); };
    auto room = [this, at, limit](double& s, double& rw, double& rh) {
      s = (std::max)(SizeMin, (std::min)(floatScale, limit()));
      rw = (std::max)(0.0, at->big.Width - at->w0 * s - 2 * Margin); rh = (std::max)(0.0, at->big.Height - at->h0 * s - 2 * Margin);
    };
    // Sized, not scaled (the owner, 29/10/2026: made bigger it stayed blurry
    // and its green ring grew): the picture is drawn again at the new size
    // and the ring and corners stay as on every block.
    auto applied = std::make_shared<double>(0);
    auto apply = [this, at, room, weakStack, applied] {
      auto stack = weakStack.get(); if (!stack || at->big.Width <= 0) return;
      double s, rw, rh; room(s, rw, rh);
      if (s != *applied) {
        *applied = s; stack.RowSpacing(8 * s); stack.ColumnSpacing(8 * s);
        for (auto const& d : stack.ColumnDefinitions()) d.Width(GridLength{160 * s, GridUnitType::Pixel});
        for (auto const& d : stack.RowDefinitions()) d.Height(GridLength{90 * s, GridUnitType::Pixel});
        for (auto const& child : stack.Children()) if (auto cam = child.try_as<FrameworkElement>(); cam && cam.Name() == L"FloatingCamera") { cam.Width(160 * s); cam.Height(90 * s); }
      }
      Canvas::SetLeft(stack, at->big.X + Margin + rw * floatX); Canvas::SetTop(stack, at->big.Y + Margin + rh * floatY);
    };
    auto clamp01 = [](double v) { return (std::min)(1.0, (std::max)(0.0, v)); };
    stack.PointerPressed([at, room, weakBody](auto const& sender, Input::PointerRoutedEventArgs const& e) {
      auto body = weakBody.get(); if (!body || !e.GetCurrentPoint(body).Properties().IsLeftButtonPressed()) return;
      double s, rw, rh; room(s, rw, rh);
      at->pressed = true; at->dragged = false; at->from = e.GetCurrentPoint(body).Position();
      at->px = Canvas::GetLeft(sender.template as<UIElement>()) - at->big.X - Margin; at->py = Canvas::GetTop(sender.template as<UIElement>()) - at->big.Y - Margin;
    });
    stack.PointerMoved([this, at, room, apply, clamp01, weakBody](auto const& sender, Input::PointerRoutedEventArgs const& e) {
      auto body = weakBody.get(); if (!at->pressed || !at->edge.empty() || !body) return;
      auto p = e.GetCurrentPoint(body).Position(); double dx = p.X - at->from.X, dy = p.Y - at->from.Y;
      if (!at->dragged) { if (std::hypot(dx, dy) < 6) return; at->dragged = true; sender.template as<UIElement>().CapturePointer(e.Pointer()); HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeAll); }
      double s, rw, rh; room(s, rw, rh);
      floatX = rw > 0 ? clamp01((at->px + dx) / rw) : 0; floatY = rh > 0 ? clamp01((at->py + dy) / rh) : 0; apply(); e.Handled(true);
    });
    auto release = [at](auto const& sender, Input::PointerRoutedEventArgs const& e) {
      if (at->dragged) { e.Handled(true); HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow); }
      at->pressed = false; sender.template as<UIElement>().ReleasePointerCaptures();
    };
    stack.PointerReleased(release); stack.PointerCaptureLost([at](auto const&, auto const&) { at->pressed = false; });
    // A drag's press is not a press on a camera.
    stack.AddHandler(UIElement::TappedEvent(), box_value(Input::TappedEventHandler([at](auto const&, Input::TappedRoutedEventArgs const& e) { if (at->dragged) { at->dragged = false; e.Handled(true); } })), false);
    using Shape = winrt::Microsoft::UI::Input::InputSystemCursorShape;
    struct EdgeSpec { char const* name; Shape shape; HorizontalAlignment h; VerticalAlignment v; double w, ht; Thickness m; };
    EdgeSpec edges[] = {
      {"n", Shape::SizeNorthSouth, HorizontalAlignment::Stretch, VerticalAlignment::Top, NAN, 8, {8, -4, 8, 0}},
      {"s", Shape::SizeNorthSouth, HorizontalAlignment::Stretch, VerticalAlignment::Bottom, NAN, 8, {8, 0, 8, -4}},
      {"e", Shape::SizeWestEast, HorizontalAlignment::Right, VerticalAlignment::Stretch, 8, NAN, {0, 8, -4, 8}},
      {"w", Shape::SizeWestEast, HorizontalAlignment::Left, VerticalAlignment::Stretch, 8, NAN, {-4, 8, 0, 8}},
      {"nw", Shape::SizeNorthwestSoutheast, HorizontalAlignment::Left, VerticalAlignment::Top, 14, 14, {-6, -6, 0, 0}},
      {"se", Shape::SizeNorthwestSoutheast, HorizontalAlignment::Right, VerticalAlignment::Bottom, 14, 14, {0, 0, -6, -6}},
      {"ne", Shape::SizeNortheastSouthwest, HorizontalAlignment::Right, VerticalAlignment::Top, 14, 14, {0, -6, -6, 0}},
      {"sw", Shape::SizeNortheastSouthwest, HorizontalAlignment::Left, VerticalAlignment::Bottom, 14, 14, {-6, 0, 0, -6}},
    };
    for (auto const& spec : edges) {
      Border grip; grip.Name(L"FloatingEdge"); grip.Background(SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0})); grip.HorizontalAlignment(spec.h); grip.VerticalAlignment(spec.v); grip.Margin(spec.m);
      if (!std::isnan(spec.w)) grip.Width(spec.w); if (!std::isnan(spec.ht)) grip.Height(spec.ht);
      Grid::SetRowSpan(grip, rows); Grid::SetColumnSpan(grip, columns); HarmonyPointer(grip, spec.shape);
      std::string edge = spec.name;
      grip.PointerPressed([at, room, weakStack, weakBody, edge](auto const& sender, Input::PointerRoutedEventArgs const& e) {
        auto stack = weakStack.get(); auto body = weakBody.get(); if (!stack || !body) return;
        double s, rw, rh; room(s, rw, rh);
        at->edge = edge; at->from = e.GetCurrentPoint(body).Position(); at->gl = Canvas::GetLeft(stack) - at->big.X; at->gt = Canvas::GetTop(stack) - at->big.Y; at->gw = at->w0 * s; at->gh = at->h0 * s;
        sender.template as<UIElement>().CapturePointer(e.Pointer()); e.Handled(true);
      });
      // Pulling an edge (Call.tsx resize): one scale for both sides, the
      // corner across from the edge stays, and on a corner the axis pulled
      // furthest decides; the side not pulled does not hold it back.
      grip.PointerMoved([this, at, room, apply, clamp01, limit, weakBody](auto const&, Input::PointerRoutedEventArgs const& e) {
        auto body = weakBody.get(); if (at->edge.empty() || !body) return;
        auto p = e.GetCurrentPoint(body).Position(); double dx = p.X - at->from.X, dy = p.Y - at->from.Y;
        bool west = at->edge.find('w') != std::string::npos, north = at->edge.find('n') != std::string::npos;
        bool east = at->edge.find('e') != std::string::npos, south = at->edge.find('s') != std::string::npos;
        double was = at->gw / at->w0, byW = east ? (at->gw + dx) / at->w0 : west ? (at->gw - dx) / at->w0 : NAN, byH = south ? (at->gh + dy) / at->h0 : north ? (at->gh - dy) / at->h0 : NAN;
        double want = !std::isnan(byW) && !std::isnan(byH) ? (std::abs(byW - was) >= std::abs(byH - was) ? byW : byH) : !std::isnan(byW) ? byW : !std::isnan(byH) ? byH : was;
        double right = at->gl + at->gw, bottom = at->gt + at->gh;
        double roomW = west ? right - Margin : east ? at->big.Width - Margin - at->gl : INFINITY;
        double roomH = north ? bottom - Margin : south ? at->big.Height - Margin - at->gt : INFINITY;
        floatScale = (std::max)(SizeMin, (std::min)({want, roomW / at->w0, roomH / at->h0, limit()}));
        double left = west ? right - at->w0 * floatScale : at->gl, top = north ? bottom - at->h0 * floatScale : at->gt;
        double s, rw, rh; room(s, rw, rh);
        floatX = rw > 0 ? clamp01((left - Margin) / rw) : 0; floatY = rh > 0 ? clamp01((top - Margin) / rh) : 0; apply(); e.Handled(true);
      });
      grip.PointerReleased([at](auto const& sender, Input::PointerRoutedEventArgs const& e) { if (at->edge.empty()) return; at->edge.clear(); at->pressed = false; sender.template as<UIElement>().ReleasePointerCaptures(); e.Handled(true); });
      grip.PointerCaptureLost([at](auto const&, auto const&) { at->edge.clear(); });
      grip.Tapped([](auto const&, Input::TappedRoutedEventArgs const& e) { e.Handled(true); });
      stack.Children().Append(grip);
    }
    body.Children().Append(stack);
    return [at, apply](winrt::Windows::Foundation::Rect big) { at->big = big; apply(); };
  }
  // One block of a call's stage: a person ("p:<user>") or a screen ("s:<user>").
  struct Seat {
    std::string key;
    std::wstring name, label, bigLabel, access;   // the chip; a screen's chip while big; what a reader says
    bool screen = false, mine = false;
    app::ChatPerson person;                        // the face, when there is no picture
    std::function<FrameworkElement()> picture;     // the camera or the screen; empty: the face
    std::vector<std::pair<std::string, uint32_t>> marks;
    std::function<void(Border)> ring;              // where the green ring is kept, to light it
    std::function<void()> watch, stop;             // a screen not arriving here; "Parar de assistir"
    std::function<FrameworkElement()> volume;      // the live's sound
    std::function<void(FrameworkElement, std::optional<winrt::Windows::Foundation::Point>)> menu;    // the right button: the person's volume, at the pointer
  };
  // The stage of a call (Call.tsx), the room's and the direct call's: the
  // blocks in a grid, or the one in focus big with the strip under it (folding
  // away, the cameras then floating over it), or the one in full screen with
  // its controls over it. The caller places the bar's island (`controls`),
  // except in full screen: there `toggles` and `leave` go over the picture.
  Grid DrawStage(CallStage& st, Grid view, int row, std::vector<Seat> const& seats, Border controls, StackPanel toggles, FrameworkElement leave) {
    const bool full = !st.full.empty();
    const bool floatingOn = !st.focus.empty() && !st.strip;
    Grid stage; Grid::SetRow(stage, row); stage.Margin(full ? Thickness{0, 0, 0, 0} : Thickness{4, 0, 8, 0}); view.Children().Append(stage);
    Canvas body; body.UseLayoutRounding(false); stage.Children().Append(body);
    std::vector<std::pair<std::string, FrameworkElement>> placed;
    // On the big block the chips come up under the pointer (Call.tsx
    // tilebar: group-hover), so the picture is clear while you watch it.
    auto fade = [](Grid& card, std::vector<UIElement> parts) {
      for (auto& part : parts) part.Opacity(0);
      card.PointerEntered([parts](auto const&, auto const&) { for (auto part : parts) part.Opacity(1); });
      card.PointerExited([parts](auto const&, auto const&) { for (auto part : parts) part.Opacity(0); });
    };
    // The ring of whoever is talking, over the block's own corner (border-2
    // border-success): drawn on top, so nothing inside moves when it lights.
    auto ring = [](Grid& content, Seat const& seat) {
      if (!seat.ring) return;
      Border r; r.Name(L"CallSpeaking"); r.CornerRadius(CornerRadius{8, 8, 8, 8}); r.BorderThickness(Thickness{2, 2, 2, 2}); r.BorderBrush(Brush(Success));
      r.IsHitTestVisible(false); r.Visibility(Visibility::Collapsed); content.Children().Append(r); seat.ring(r);
    };
    auto block = [&](bool picture) {
      Border layer; layer.CornerRadius(CornerRadius{8, 8, 8, 8}); layer.Background(Brush(picture ? Surface2 : Surface)); layer.UseLayoutRounding(false);
      return layer;
    };
    // A press on a chip, a button or the slider is theirs, not the block's.
    auto pressedPart = [](auto const& sender, auto const& args) {
      auto source = args.OriginalSource().template try_as<DependencyObject>();
      for (auto at = source; at && at != sender.template as<DependencyObject>(); at = VisualTreeHelper::GetParent(at))
        if (at.template try_as<Button>() || at.template try_as<Slider>()) return true;
      return false;
    };
    // The floating cameras: the picture only, no name, no circle (Call.tsx
    // Floating); pressing one makes it the big block, its x takes it off.
    std::vector<FrameworkElement> floaters;
    auto floater = [&](Seat const& seat) {
      Grid cam; cam.Name(L"FloatingCamera"); cam.Width(160); cam.Height(90); cam.UseLayoutRounding(false);
      auto layer = block(true); Grid content; content.UseLayoutRounding(false);
      // The picture fills its rounded block to the edge, under the ring
      // (call-ui.h DirectPanel), instead of fitting whole with black at its sides.
      auto picture = seat.picture(); CoverPicture(picture); picture.Margin(Thickness{0, 0, 0, 0}); RoundVideo(picture, 8);
      content.Children().Append(picture); ring(content, seat);
      layer.Child(content); cam.Children().Append(layer);
      auto tuck = Square("xmark", L"Tirar a câmera de " + seat.label + L" de cima da imagem", 24, 12, Foreground, Default, [this, &st, key = seat.key] { st.tucked.insert(key); RedrawStage(st); });
      tuck.Name(L"FloatingTuck"); tuck.Background(Brush(Background)); tuck.HorizontalAlignment(HorizontalAlignment::Right); tuck.VerticalAlignment(VerticalAlignment::Top); tuck.Margin(Thickness{4, 4, 4, 4}); tuck.Opacity(0);
      cam.Children().Append(tuck);
      cam.PointerEntered([tuck](auto const&, auto const&) { tuck.Opacity(1); });
      cam.PointerExited([tuck](auto const&, auto const&) { tuck.Opacity(0); });
      cam.Tapped([this, &st, key = seat.key, pressedPart](auto const& sender, auto const& args) { if (!pressedPart(sender, args)) FocusSeat(st, key); });
      Automation::AutomationProperties::SetName(cam, L"Câmera de " + seat.label); floaters.push_back(cam);
    };
    for (auto const& seat : seats) if (!seat.screen) {
      bool camera = bool(seat.picture);
      if (floatingOn && camera && seat.key != st.focus && !st.tucked.count(seat.key)) { floater(seat); continue; }
      if (full && seat.key != st.full) continue;
      bool big = seat.key == st.focus;
      Grid card; card.Name(L"CallTile"); card.UseLayoutRounding(false);
      auto layer = block(camera); Grid content; content.UseLayoutRounding(false);
      if (camera) content.Children().Append(seat.picture());
      // The circle, 64 at 24/36 600, and under it, 12 apart and centred
      // with it, the way to a screen that is not arriving here.
      StackPanel middle; middle.Spacing(12); middle.HorizontalAlignment(HorizontalAlignment::Center); middle.VerticalAlignment(VerticalAlignment::Center);
      if (!camera) {
        auto face = PhotoFace(seat.person, 64); face.Name(L"CallAvatar"); face.HorizontalAlignment(HorizontalAlignment::Center);
        if (auto letter = face.Child().try_as<TextBlock>()) { letter.FontSize(24); letter.LineHeight(36); }
        middle.Children().Append(face);
      }
      if (seat.watch) {
        Button watch; HarmonyPointer(watch); watch.Name(L"WatchScreen"); watch.Height(44); watch.MinHeight(0); watch.MinWidth(0); watch.Padding(Thickness{16, 0, 16, 0});
        watch.CornerRadius(CornerRadius{8, 8, 8, 8}); watch.BorderThickness(Thickness{0, 0, 0, 0}); watch.Background(Brush(Default)); watch.HorizontalAlignment(HorizontalAlignment::Center);
        watch.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Accent)); watch.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Accent));
        StackPanel line; line.Orientation(Orientation::Horizontal); line.Spacing(8);
        auto glyph = VoiceIcon("display", Foreground, 20).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); line.Children().Append(glyph);
        auto word = Line(L"Assistir", 13, Foreground, 500); word.VerticalAlignment(VerticalAlignment::Center); line.Children().Append(word);
        watch.Content(line); Automation::AutomationProperties::SetName(watch, L"Assistir à tela de " + seat.name);
        watch.Click([this, act = seat.watch](auto const&, auto const&) { if (!exiting) act(); });
        // On a camera the person is the picture: the button sits on its
        // lower part, 40 up, clear of the name.
        // Only under the pointer there (the owner, 17/10/2026: it covered
        // the person's camera), or with the keyboard on it.
        if (camera) {
          watch.VerticalAlignment(VerticalAlignment::Bottom); watch.Margin(Thickness{0, 0, 0, 40}); watch.Opacity(0); content.Children().Append(watch);
          auto weakView = winrt::make_weak(watch);
          card.PointerEntered([weakView](auto const&, auto const&) { if (auto v = weakView.get()) v.Opacity(1); });
          card.PointerExited([weakView](auto const&, auto const&) { if (auto v = weakView.get(); v && v.FocusState() != FocusState::Keyboard) v.Opacity(0); });
          watch.GotFocus([](IInspectable const& s, auto const&) { s.as<Button>().Opacity(1); });
          watch.LostFocus([](IInspectable const& s, auto const&) { s.as<Button>().Opacity(0); });
        }
        else middle.Children().Append(watch);
      }
      if (middle.Children().Size() > 0) content.Children().Append(middle);
      auto chip = CallChip(seat.label, seat.marks); content.Children().Append(chip);
      ring(content, seat);
      layer.Child(content); card.Children().Append(layer);
      if (big && !full) fade(card, {chip});
      // Pressing is "look at this"; what you can do about the person is on
      // the right button (Call.tsx), here their volume.
      card.Tapped([this, &st, key = seat.key, pressedPart](auto const& sender, auto const& args) { if (!pressedPart(sender, args)) FocusSeat(st, key); });
      if (seat.menu) card.RightTapped([menu = seat.menu](auto const& sender, Input::RightTappedRoutedEventArgs const& e) { auto at = sender.template as<FrameworkElement>(); e.Handled(true); menu(at, e.GetPosition(at)); });
      Automation::AutomationProperties::SetName(card, seat.access); placed.push_back({seat.key, card}); body.Children().Append(card);
    }
    // A screen is one more block, after the people (Call.tsx: seats, then
    // screens), with "Tela de …" in its chip.
    for (auto const& seat : seats) if (seat.screen) {
      if (full && seat.key != st.full) continue;
      bool big = seat.key == st.focus;
      Grid card; card.Name(L"ScreenTile"); card.UseLayoutRounding(false);
      auto layer = block(true); Grid content; content.UseLayoutRounding(false);
      content.Children().Append(seat.picture());
      if (full) {
        // Only the picture: the chips are on the overlay, which goes away.
        layer.CornerRadius(CornerRadius{0, 0, 0, 0}); layer.Background(Brush(0x000000));
        layer.Child(content); card.Children().Append(layer);
        Automation::AutomationProperties::SetName(card, seat.access); placed.push_back({seat.key, card}); body.Children().Append(card);
        continue;
      }
      auto chip = CallChip(big ? seat.bigLabel : seat.label, {{"display-pulse", Foreground}}); content.Children().Append(chip);
      StackPanel top; top.Orientation(Orientation::Horizontal); top.Spacing(8); top.Margin(Thickness{8, 8, 8, 8});
      top.HorizontalAlignment(HorizontalAlignment::Right); top.VerticalAlignment(VerticalAlignment::Top);
      if (seat.stop) { auto stop = CallChipButton("", L"Parar de assistir", seat.stop); stop.Name(L"StopWatching"); top.Children().Append(stop); }
      // "Tela cheia" is on the block in focus, as on the web.
      if (big) { auto expand = CallChipButton("chevrons-expand-up-right", L"Tela cheia", [this, &st, key = seat.key] { EnterCallFull(st, key); }); expand.Name(L"ScreenFullscreen"); top.Children().Append(expand); }
      content.Children().Append(top);
      std::vector<UIElement> hover{chip, top};
      FrameworkElement volume{nullptr};
      if (seat.volume) {
        // The live's sound, bottom right, under the pointer (Call.tsx
        // data-live-volume: bottom-10 right-2, over the name's line).
        volume = seat.volume(); volume.HorizontalAlignment(HorizontalAlignment::Right); volume.VerticalAlignment(VerticalAlignment::Bottom);
        volume.Margin(Thickness{8, 8, 8, 40}); content.Children().Append(volume); hover.push_back(volume);
      }
      ring(content, seat);
      layer.Child(content); card.Children().Append(layer);
      if (big) fade(card, hover);
      else if (volume) { volume.Opacity(0); card.PointerEntered([volume](auto const&, auto const&) { volume.Opacity(1); }); card.PointerExited([volume](auto const&, auto const&) { volume.Opacity(0); }); }
      card.Tapped([this, &st, key = seat.key, pressedPart](auto const& sender, auto const& args) { if (!pressedPart(sender, args)) FocusSeat(st, key); });
      Automation::AutomationProperties::SetName(card, seat.access); placed.push_back({seat.key, card}); body.Children().Append(card);
    }
    // Focus: the strip folds away with the button between it and the big
    // block (Call.tsx: "⌄ 👥", h-7 on bg-surface, the strip h-28).
    int bigIndex = -1;
    for (int i = 0; i < int(placed.size()); ++i) if (placed[i].first == st.focus) bigIndex = i;
    Button stripFold{nullptr};
    if (bigIndex >= 0 && !full && seats.size() > 1) {
      stripFold = Button(); HarmonyPointer(stripFold); stripFold.Name(L"CallStripToggle"); stripFold.Height(28); stripFold.MinHeight(0); stripFold.MinWidth(0); stripFold.Padding(Thickness{8, 0, 8, 0});
      stripFold.CornerRadius(CornerRadius{8, 8, 8, 8}); stripFold.BorderThickness(Thickness{0, 0, 0, 0}); stripFold.Background(Brush(Surface));
      stripFold.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); stripFold.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
      StackPanel glyphs; glyphs.Orientation(Orientation::Horizontal); glyphs.Spacing(4);
      auto chevron = VoiceIcon("chevron-down", Muted, 14).as<FrameworkElement>(); chevron.VerticalAlignment(VerticalAlignment::Center);
      if (!st.strip) { chevron.RenderTransformOrigin({0.5, 0.5}); RotateTransform turn; turn.Angle(180); chevron.RenderTransform(turn); }
      glyphs.Children().Append(chevron);
      auto persons = VoiceIcon("persons", Muted, 14).as<FrameworkElement>(); persons.VerticalAlignment(VerticalAlignment::Center); glyphs.Children().Append(persons);
      stripFold.Content(glyphs);
      auto label = st.strip ? (st.direct ? L"Esconder quem está na chamada" : L"Esconder quem está na sala") : (st.direct ? L"Mostrar quem está na chamada" : L"Mostrar quem está na sala");
      Automation::AutomationProperties::SetName(stripFold, label); ToolTipService::SetToolTip(stripFold, box_value(label));
      stripFold.Click([this, &st](auto const&, auto const&) { if (exiting) return; if (!st.strip) st.tucked.clear(); st.strip = !st.strip; RedrawStage(st); });
      body.Children().Append(stripFold);
      if (!st.strip) for (int i = 0; i < int(placed.size()); ++i) if (i != bigIndex) placed[i].second.Visibility(Visibility::Collapsed);
    }
    std::vector<FrameworkElement> cards; for (auto& seat : placed) cards.push_back(seat.second);
    std::function<void(winrt::Windows::Foundation::Rect)> placeFloat;
    if (!floaters.empty() && (full || bigIndex >= 0)) placeFloat = FloatingCameras(body, floaters);
    if (full || bigIndex >= 0) {
      // The big block: the whole area at 16:9, centred (Call.tsx BIG); under
      // it, 8 apart, the fold button and the strip of 112-tall blocks.
      bool strip = st.strip && !full; int big = full ? 0 : bigIndex;
      body.SizeChanged([cards, big, stripFold, strip, full, placeFloat](auto const& sender, auto const&) {
        if (cards.empty()) return; auto canvas = sender.template as<Canvas>(); double W = canvas.ActualWidth(), H = canvas.ActualHeight();
        double footer = stripFold ? 8 + 28 + (strip ? 8 + 112 : 0) : 0, area = (std::max)(0.0, H - footer);
        double w = full ? W : (std::min)(W, area * 16 / 9), h = full ? H : w * 9 / 16;
        cards[big].Width(w); cards[big].Height(h); Canvas::SetLeft(cards[big], (W - w) / 2); Canvas::SetTop(cards[big], full ? 0 : (area - h) / 2);
        if (placeFloat) placeFloat(winrt::Windows::Foundation::Rect{float((W - w) / 2), float(full ? 0 : (area - h) / 2), float(w), float(h)});
        if (!stripFold) return;
        stripFold.Measure({float(W), 28.f}); Canvas::SetLeft(stripFold, (W - stripFold.DesiredSize().Width) / 2); Canvas::SetTop(stripFold, area + 8);
        if (!strip) return;
        int n = int(cards.size()) - 1; double sw = (std::min)(112.0 * 16 / 9, (W - 8 * (n - 1)) / n), sh = sw * 9 / 16, x = (W - n * sw - 8 * (n - 1)) / 2, y = area + 8 + 28 + 8 + (112 - sh) / 2;
        for (int i = 0; i < int(cards.size()); ++i) if (i != big) { cards[i].Width(sw); cards[i].Height(sh); Canvas::SetLeft(cards[i], x); Canvas::SetTop(cards[i], y); x += sw + 8; }
      });
    } else
    // Same column/row counts, eight-pixel gaps and 16:9 constraint as Call.tsx.
    body.SizeChanged([cards](auto const& sender,auto const&) {
      if(cards.empty()) return; auto canvas=sender.template as<Canvas>(); int count=int(cards.size()); int columns=count==1?1:count<=4?2:count<=9?3:4; int rows=(count+columns-1)/columns;
      double width=(std::max)(0.0,(std::min)((canvas.ActualWidth()-8*(columns-1))/columns,(canvas.ActualHeight()-8*(rows-1))/rows*16/9)); double height=width*9/16;
      for(int i=0;i<count;++i) { int row=i/columns, inRow=(std::min)(columns,count-row*columns); cards[i].Width(width); cards[i].Height(height); Canvas::SetLeft(cards[i],(canvas.ActualWidth()-inRow*width-8*(inRow-1))/2+(i%columns)*(width+8)); Canvas::SetTop(cards[i],(canvas.ActualHeight()-rows*height-8*(rows-1))/2+row*(height+8)); }
    });
    if (!full) return stage;
    // Call.tsx FullscreenControls: the name and the ✕ at the top, the
    // bar (and the live's sound) at the bottom; up on any movement, gone
    // 2.5 s after the last one, by opacity.
    Seat const* shown = nullptr; for (auto const& seat : seats) if (seat.key == st.full) shown = &seat;
    toggles.Children().Append(leave); controls.Margin(Thickness{12, 12, 12, 12}); controls.Child(toggles);
    Grid chrome; chrome.Name(L"FullscreenControls"); Grid::SetRow(chrome, row);
    winrt::Microsoft::UI::Xaml::ScalarTransition fadeOut; fadeOut.Duration(std::chrono::milliseconds(200)); chrome.OpacityTransition(fadeOut);
    Grid topRow; topRow.Margin(Thickness{12, 12, 12, 12}); topRow.VerticalAlignment(VerticalAlignment::Top);
    std::wstring label = shown ? (shown->screen ? shown->bigLabel : shown->label) : std::wstring();
    Border name; name.Height(44); name.Padding(Thickness{16, 0, 16, 0}); name.CornerRadius(CornerRadius{8, 8, 8, 8}); name.Background(Brush(Background)); name.HorizontalAlignment(HorizontalAlignment::Left);
    auto nameText = Line(label, 13, Foreground, 600); nameText.VerticalAlignment(VerticalAlignment::Center); name.Child(nameText); topRow.Children().Append(name);
    Button out; HarmonyPointer(out); out.Name(L"LeaveFullscreen"); out.Height(44); out.MinHeight(0); out.MinWidth(0); out.Padding(Thickness{16, 0, 16, 0});
    out.CornerRadius(CornerRadius{8, 8, 8, 8}); out.BorderThickness(Thickness{0, 0, 0, 0}); out.Background(Brush(Background)); out.HorizontalAlignment(HorizontalAlignment::Right);
    out.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); out.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
    out.Content(VoiceIcon("xmark", Foreground, 20)); Automation::AutomationProperties::SetName(out, L"Sair da tela cheia"); ToolTipService::SetToolTip(out, box_value(L"Sair da tela cheia (Esc)"));
    out.Click([this, &st](auto const&, auto const&) { if (exiting) return; LeaveCallFull(); RedrawStage(st); });
    topRow.Children().Append(out); chrome.Children().Append(topRow);
    StackPanel bottom; bottom.Orientation(Orientation::Horizontal); bottom.Spacing(8); bottom.HorizontalAlignment(HorizontalAlignment::Center); bottom.VerticalAlignment(VerticalAlignment::Bottom);
    if (shown && shown->volume) { auto volume = shown->volume(); volume.VerticalAlignment(VerticalAlignment::Center); bottom.Children().Append(volume); }
    bottom.Children().Append(controls); chrome.Children().Append(bottom);
    view.Children().Append(chrome);
    auto weakChrome = make_weak(chrome); fullChrome = weakChrome;
    if (!fullIdle) {
      fullIdle = DispatcherTimer(); fullIdle.Interval(std::chrono::milliseconds(2500));
      fullIdle.Tick([this](auto const& timer, auto const&) {
        timer.template as<DispatcherTimer>().Stop(); if (auto c = fullChrome.get()) { c.Opacity(0); c.IsHitTestVisible(false); }
        // The mouse goes with them, but only over this window: it is ours
        // to hide only there, and a menu or a dialog keeps it.
        POINT at{}; GetCursorPos(&at); fullPointer = at;
        if (GetForegroundWindow() == hwnd && GetAncestor(WindowFromPoint(at), GA_ROOT) == hwnd && fullStage) HideCursor(true);
      });
    }
    // A press, or a move that went somewhere: XAML also reports moves
    // when what is under a still pointer changes (the controls fading).
    auto wake = [this, weakChrome](bool press) {
      POINT at{}; GetCursorPos(&at);
      if (!press && at.x == fullPointer.x && at.y == fullPointer.y) return;
      fullPointer = at; HideCursor(false);
      if (auto c = weakChrome.get()) { c.Opacity(1); c.IsHitTestVisible(true); } if (fullIdle) { fullIdle.Stop(); fullIdle.Start(); }
    };
    view.PointerMoved([wake](auto const&, auto const&) { wake(false); }); view.PointerPressed([wake](auto const&, auto const&) { wake(true); });
    GetCursorPos(&fullPointer); fullIdle.Stop(); fullIdle.Start();
    out.Loaded([](auto const& sender, auto const&) { sender.template as<Button>().Focus(FocusState::Programmatic); });
    return stage;
  }
  void DrawCall(app::State const& state) {
    auto const& voice=state.voice;
    if (voice.room != watchRoom) { viewingScreens.clear(); roomStage.tucked.clear(); watchRoom = voice.room; }
    // Only this room's people redraw the call (a change in another room used to
    // rebuild it, and with it every picture), and their photos do too.
    std::map<std::string, app::ChatPerson> members;
    service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& p : s.members) members[p.username] = p; });
    std::set<std::string> sharing;
    std::string key = voice.room + "|" + voice.self;
    for (auto const& room : state.model.rooms) if (room.id == voice.room) for (auto const& p : room.people) {
      auto it = members.find(p.username);
      key += "|" + p.username + "|" + p.name + (p.muted ? "m" : "_") + (p.deafened ? "d" : "_") + (p.screen ? "s" : "_") + (p.camera ? "v" : "_") + (it == members.end() ? "" : it->second.avatar);
      if (p.screen) sharing.insert(p.username);
    }
    // The separate window goes with the screen it shows, as the web's floating one does.
    if (videoWindow && !videoWho.empty() && !sharing.count(videoWho)) SendMessageW(videoWindow, WM_CLOSE, 0, 0);
    // The blocks the room draws, in order (people, then screens): focus needs
    // two of them, and a focus or full screen on a block that left ends.
    std::vector<std::string> seatKeys;
    for (auto const& room : state.model.rooms) if (room.id == voice.room) {
      for (auto const& p : room.people) seatKeys.push_back("p:" + p.username);
      for (auto const& p : room.people) if (p.screen && (p.username == voice.self || viewingScreens.count(p.username))) seatKeys.push_back("s:" + p.username);
    }
    if (seatKeys.size() < 2 || std::find(seatKeys.begin(), seatKeys.end(), roomStage.focus) == seatKeys.end()) roomStage.focus.clear();
    if (fullStage == &roomStage && (voice.room.empty() || !showingCall || roomStage.full != roomStage.focus)) LeaveCallFull();
    const bool full = !roomStage.full.empty();
    const bool floatingOn = !roomStage.focus.empty() && !roomStage.strip;
    key+=std::string(showingCall?"C":"_")+(callPeople?"P":"_")+(voice.muted?"m":"_") + std::string(voice.deafened?"d":"_") + (voice.reconnecting?"r":"_") + (voice.connecting?"c":"_")+(voice.sharingScreen?"s":"_")+(voice.sharingCamera?"v":"_");
    for (auto const& who : viewingScreens) key += "|w" + who;
    key += "|f" + roomStage.focus + "|F" + roomStage.full + (roomStage.strip ? "T" : "_");
    for (auto const& t : roomStage.tucked) key += "|t" + t;
    if(key!=callSignature) {
      callSignature=key; callFaces.clear();
      std::set<std::pair<std::string,std::string>> nextVideoKeys;
      if(!voice.room.empty())for(auto& room:state.model.rooms)if(room.id==voice.room)for(auto& person:room.people){if((!full||roomStage.full=="p:"+person.username||(floatingOn&&!roomStage.tucked.count("p:"+person.username)))&&(person.camera||(person.username==voice.self&&voice.sharingCamera)))nextVideoKeys.insert({person.username,"cam"});if((!full||roomStage.full=="s:"+person.username)&&person.screen&&(person.username==voice.self||viewingScreens.count(person.username)))nextVideoKeys.insert({person.username,"screen"});}
      for(auto& item:videoKeys)if(!nextVideoKeys.count(item))service->Video(item.first,item.second,{});videoKeys=std::move(nextVideoKeys);
      if(callView) { uint32_t at{}; if(root.Children().IndexOf(callView,at)) root.Children().RemoveAt(at); callView=nullptr; }
      // The call takes the middle column only while it is the thing being
      // looked at: a text channel can be read with the voice still on, as on
      // the web (W5).
      bool showCall=!voice.room.empty()&&showingCall;
      if(conversation) conversation.Visibility(showCall?Visibility::Collapsed:Visibility::Visible);
      bool fold = showCall ? !callPeople : !peopleShown;
      if (peopleIsland) peopleIsland.Visibility(fold ? Visibility::Collapsed : Visibility::Visible);
      if (root.ColumnDefinitions().Size() > 2) root.ColumnDefinitions().GetAt(2).Width(GridLength{fold ? 0.0 : 248.0, GridUnitType::Pixel});
      // Reading a text channel with the voice on. The tiles left the screen;
      // their renderers (swap chain, thread) go now, and the screens are told
      // nobody is looking, so a long read stops the stream while a peek
      // changes nothing (media-policy.h). Coming back subscribes afresh.
      // Once per departure: doing it again on every redraw would subscribe
      // again and restart the wait.
      if(!showCall) {
        if(!callTilesAway) for(auto& item:videoKeys) {
          service->Video(item.first,item.second,[](webrtc::VideoTrackInterface*){});
          if(item.second=="screen")service->VideoSize(item.first,-1,1,false);
        }
        callTilesAway=!voice.room.empty();
        return;
      }
      callTilesAway=false;
      callView=Grid(); callView.Name(L"CallView"); Grid::SetColumn(callView,1); callView.UseLayoutRounding(false);
      // Full screen: over the whole window, on black, with only the block.
      if (full) {
        Grid::SetColumn(callView, 0); Grid::SetColumnSpan(callView, (std::max)(1, int(root.ColumnDefinitions().Size()))); Grid::SetRowSpan(callView, (std::max)(1, int(root.RowDefinitions().Size())));
        callView.Background(Brush(0x000000));
      }
      for(auto height:{GridLength{full?0.0:64.0,GridUnitType::Pixel},GridLength{1,GridUnitType::Star},GridLength{full?0.0:76.0,GridUnitType::Pixel}}) { RowDefinition row; row.Height(height); callView.RowDefinitions().Append(row); }
      // The header: the chat's island (48 tall, 8 from the top, padding 0 8 0
      // 16), the headphones 22 in muted, the name 12 after it at 20/28 600, and
      // the people button, a 32 square, on the right.
      auto heading = Island(); heading.Name(L"CallHeader"); heading.Margin(Thickness{4,8,8,8}); heading.Padding(Thickness{16,0,8,0});
      Grid headerContent; headerContent.ColumnSpacing(12);
      for (auto w : {GridLength{22, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); headerContent.ColumnDefinitions().Append(c); }
      auto phones = VoiceIcon("headphones", Muted, 22).as<FrameworkElement>(); phones.Name(L"CallHeaderIcon"); phones.VerticalAlignment(VerticalAlignment::Center); headerContent.Children().Append(phones);
      auto title=Line(net::Wide(voice.roomName)+(voice.reconnecting?L" — Reconectando…":voice.connecting?L" — Ligando o áudio…":L""),20,Foreground,600); title.Name(L"CallTitle"); title.CharacterSpacing(-15); title.VerticalAlignment(VerticalAlignment::Center);
      Grid::SetColumn(title, 1); headerContent.Children().Append(title);
      auto people = Square("persons", callPeople ? L"Esconder quem está aqui" : L"Ver quem está aqui", 32, 18, callPeople ? Foreground : Muted, Surface2, [this] { callPeople = !callPeople; callSignature.clear(); Render(); });
      people.Name(L"CallPeople"); people.VerticalAlignment(VerticalAlignment::Center); if (callPeople) people.Background(Tint(Default, 0.5));
      Grid::SetColumn(people, 2); headerContent.Children().Append(people);
      heading.Child(headerContent); if (!full) callView.Children().Append(heading);
      std::vector<Seat> seats;
      for (auto const& room : state.model.rooms) if (room.id == voice.room) {
        for (auto const& person : room.people) {
          auto name = net::Wide(person.name.empty() ? person.username : person.name);
          bool mine = person.username == voice.self;
          Seat seat; seat.key = "p:" + person.username; seat.name = name; seat.label = name + (mine ? L" (você)" : L""); seat.access = name + L", participante da sala"; seat.mine = mine;
          auto it = members.find(person.username);
          seat.person = it != members.end() ? it->second : app::ChatPerson{person.username, person.name};
          if (person.camera || (mine && voice.sharingCamera)) seat.picture = [this, who = person.username] { return MediaPanel(who, "cam").as<FrameworkElement>(); };
          if (person.deafened) seat.marks.push_back({"volume-xmark", DangerSoftFg}); else if (person.muted) seat.marks.push_back({"microphone-slash", DangerSoftFg});
          if (!mine && PersonalState().Silenced(person.username)) { seat.marks.push_back({"microphone-slash", Muted}); seat.access += L", silenciado por você"; }
          seat.ring = [this, who = person.username](Border r) { callFaces[who] = r; };
          if (!mine && person.screen && !viewingScreens.count(person.username)) seat.watch = [this, who = person.username] { WatchScreen(who); };
          if (!mine) seat.menu = [this, who = person.username, name](FrameworkElement at, auto point) { OpenVoiceVolume(at, who, name, point); };
          seats.push_back(std::move(seat));
        }
        for (auto const& person : room.people) if (person.screen && (person.username == voice.self || viewingScreens.count(person.username))) {
          auto name = net::Wide(person.name.empty() ? person.username : person.name);
          bool mine = person.username == voice.self;
          Seat seat; seat.key = "s:" + person.username; seat.screen = true; seat.mine = mine; seat.name = name;
          seat.label = seat.access = L"Tela de " + name; seat.bigLabel = name + L" está mostrando a tela";
          seat.picture = [this, who = person.username] { return MediaPanel(who, "screen").as<FrameworkElement>(); };
          seat.ring = [this, who = person.username](Border r) { callFaces[who + "/screen"] = r; };
          if (!mine) {
            seat.stop = [this, who = person.username] { StopWatching(who); };
            seat.volume = [this, who = person.username, name] { return LiveVolume(who, name); };
          }
          seats.push_back(std::move(seat));
        }
      }
      // The bar: an island 8 under the blocks, padding 8, the 44 squares 8
      // apart, and "Sair" on the right in the only red.
      auto controls = Island(); controls.Name(L"CallBar"); controls.Margin(Thickness{4,8,8,8}); controls.Padding(Thickness{8,8,8,8}); Grid::SetRow(controls,2);
      Grid bar; StackPanel toggles; toggles.Orientation(Orientation::Horizontal); toggles.Spacing(8);
      bool micOff = !voice.Sending();
      auto mute = CallSquare(micOff ? "microphone-slash" : "microphone", micOff ? DangerSoftFg : Foreground, Default, 0x2e2e31,
          voice.deafened ? L"Seu microfone está desligado porque você não está ouvindo" : voice.muted ? L"Ligar o microfone" : L"Desligar o microfone",
          [this]{auto v=ReadState().voice;service->MuteVoice(!v.muted,v.deafened);});
      mute.Name(L"CallMute"); mute.IsEnabled(!voice.deafened);
      auto deaf = CallSquare(voice.deafened ? "volume-xmark" : "headphones", voice.deafened ? DangerSoftFg : Foreground, Default, 0x2e2e31,
          voice.deafened ? L"Voltar a ouvir" : L"Não ouvir ninguém", [this]{auto v=ReadState().voice;service->MuteVoice(v.muted,!v.deafened);});
      deaf.Name(L"CallDeafen");
      auto screen = RoomScreenButton(voice.sharingScreen,44);
      screen.Name(L"ShareScreen");
      toggles.Children().Append(mute);toggles.Children().Append(deaf);toggles.Children().Append(CameraControls(false));toggles.Children().Append(screen);toggles.Children().Append(QualityButton(true));
      Button leave; HarmonyPointer(leave); leave.Name(L"CallLeave"); leave.Height(44); leave.MinHeight(0); leave.MinWidth(0); leave.Padding(Thickness{20,0,20,0});
      leave.CornerRadius(CornerRadius{8,8,8,8}); leave.BorderThickness(Thickness{0,0,0,0}); leave.Background(Brush(0xdb3b3e)); leave.HorizontalAlignment(HorizontalAlignment::Right);
      leave.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(0xe15451)); leave.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(0xe15451));
      StackPanel words; words.Orientation(Orientation::Horizontal); words.Spacing(8);
      auto door = VoiceIcon("handset-arrow-out", Foreground, 20).as<FrameworkElement>(); door.VerticalAlignment(VerticalAlignment::Center); words.Children().Append(door);
      auto sair = Line(L"Sair", 14, Foreground, 500); sair.LineHeight(21); sair.CharacterSpacing(0); sair.VerticalAlignment(VerticalAlignment::Center); words.Children().Append(sair);
      leave.Content(words); NegativeButton(leave); Automation::AutomationProperties::SetName(leave, L"Sair da chamada"); ToolTipService::SetToolTip(leave, box_value(L"Sair da chamada"));
      leave.Click([this](auto const&, auto const&) { if (!exiting) service->LeaveVoice(); });
      if (!full) { bar.Children().Append(toggles); bar.Children().Append(leave); controls.Child(bar); callView.Children().Append(controls); }
      DrawStage(roomStage, callView, 1, seats, controls, toggles, leave);
      root.Children().Append(callView);
    }
    for(auto& [where,ring]:callFaces) {auto it=voice.speaking.find(where.substr(0,where.find('/')));bool speaking=it!=voice.speaking.end()&&it->second;ring.Visibility(speaking?Visibility::Visible:Visibility::Collapsed);}
  }
  uint64_t renders = 0; double renderMs = 0; // read by the chat audit
  void Render() {
    if(exiting||!service)return;
    RefreshTrayMenu();
    struct Clock { double& total; LARGE_INTEGER t0; Clock(double& t) : total(t) { QueryPerformanceCounter(&t0); }
      ~Clock() { LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f); total += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart); } } clock{renderMs};
    ++renders;
    auto timer = perf::Get("ui.draw").Begin();
    auto state = service ? ReadState() : app::State{};
    diagnosticHistory.InCall(InCall());
    diagnosticHistory.Active(settings && settingsTab=="diag" && mediaShown);
    SyncQualityTimer();
    if(updater_) updater_->SessionChanged(state.signedIn);
    if (state.signedIn) SoundRoom(state);
    // A session that ends during a call's full screen (revoked elsewhere)
    // must not leave the sign-in page full screen with the mouse hidden.
    if (!state.signedIn && fullStage) { LeaveCallFull(); callSignature.clear(); directSignature.clear(); }
    // A direct call's screen is a transmission too: the same notice, and the
    // same exclusion of this window from the capture.
    bool callScreen = service && service->CallRead().screen;
    if(state.voice.sharingScreen||callScreen) transmissionPendingUntil=0;
    TransmissionNotice(state.voice.sharingScreen||callScreen||GetTickCount64()<transmissionPendingUntil);
    if (settings && settingsSignedIn != state.signedIn) {
      diagnosticHistory.Active(false);
      ClearAccountSecrets();
      settings = false; settingsOverlay=nullptr; settingsPreviousError=nullptr; if (meter) meter.Stop(); page = nullptr; showingRooms = false;
    }
    // Refused attempts keep their fields for correction; successful sign-in
    // must clear secrets even though member references outlive the login page.
    if (state.signedIn) {
      if (forgotDialog) forgotDialog.Hide();
      if (signupDialog) signupDialog.Hide();
      if (password && !password.Password().empty()) password.Password(L"");
      if (code && !code.Text().empty()) code.Text(L"");
    }
    if (state.signedIn && state.needsEmail) { EmailGate(); timer.Done(); return; }
    if (emailGateShowing) { emailGateShowing=false; ClearEmail(); page=nullptr; showingRooms=false; }
    if (settings) { if (state.signedIn) DrawDirectCall(); RefreshVoiceDevices(state.voice); RefreshNoiseStatus(state.voice); RefreshVoiceTest(state.voice); if (error) error.Text(net::Wide(state.syncError.empty() ? state.error : state.syncError)); timer.Done(); return; }
    if (state.signedIn) {
      if (!showingRooms) ShellPage();
      DrawVoiceBar(state.voice);
      std::string signature = state.voice.room + state.voice.self + (showingCall ? "|call" : "|chat");
      for (auto& r : state.model.rooms) {
        signature += "|" + r.id + "|" + r.name;
        for (auto& p : r.people) signature += "|" + p.username + "|" + p.name + p.avatar + (p.muted ? "m" : "_") + (p.deafened ? "d" : "_")+(p.screen?"s":"_")+(p.camera?"v":"_");
      }
      service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& p : s.members) signature += "|" + p.username + p.avatar + p.nickname + p.displayName; });
      bool redraw = signature != roomsSignature;
      // The rooms are drawn here and placed by the channel column, in the
      // folder each one sits in (DrawChannels, D12b), so the column redraws
      // after them.
      std::map<std::string, std::string> roomNames; bool manageRooms = false;
      service->ChatModel().Inspect([&](app::ChatState const& s) { manageRooms = s.canModerate; for (auto const& r : s.rooms) { roomNames[r.id] = r.name; signature += "|n" + r.name; } });
      signature += manageRooms ? "|own" : "";
      redraw = signature != roomsSignature;
      if (redraw) { roomList.Children().Clear(); voiceFaces.clear(); voiceNames.clear(); voiceGroups.clear(); voiceHandles.clear(); ++voiceGroupsVersion; roomsSignature = signature; }

      status.Text(state.model.live ? L"Ao vivo" : L"Reconectando…");
      error.Text(net::Wide(state.syncError.empty() ? state.error : state.syncError));
      error.Visibility((state.error.empty() && state.syncError.empty()) ? Visibility::Collapsed : Visibility::Visible);
      if (redraw) for (auto const& room : state.model.rooms) {
        StackPanel group; group.Spacing(0); group.Margin(Thickness{0,2,0,0});
        StackPanel titleRow; titleRow.Orientation(Orientation::Horizontal); titleRow.Spacing(6);
        titleRow.Height(36); titleRow.Margin(Thickness{8,0,8,0});
        bool active=showingCall && room.id==state.voice.room;
        titleRow.Children().Append(VoiceIcon("headphones",room.id==state.voice.room?0x17c964:0xa1a1aa,18));
        auto shownName = roomNames.contains(room.id) ? roomNames[room.id] : room.name;
        auto title = Text(net::Wide(shownName), 16, active?0xfcfcfc:0xa1a1aa); title.FontWeight(winrt::Windows::UI::Text::FontWeight{uint16_t(active?600:500)}); title.CharacterSpacing(-15); title.VerticalAlignment(VerticalAlignment::Center);
        titleRow.Children().Append(title);
        auto enter = Action(L"", [this, id = room.id] {
          if (dragSwallow) return;
          showingCall = true; callSignature.clear();
          // Entering folds the people away, as on the web; only on the way in.
          if (ReadState().voice.room != id) { callPeople = false; service->JoinVoice(id); } else Render();
        });
        enter.Name(L"VoiceRoomRow"); enter.Content(titleRow); enter.Height(36); enter.MinHeight(36); enter.Padding(Thickness{0,0,0,0});
        enter.HorizontalAlignment(HorizontalAlignment::Stretch); enter.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        enter.Background(active ? Brush(0x27272a) : Tint(0, 0));
        Automation::AutomationProperties::SetName(enter, L"Entrar na sala " + net::Wide(room.name));
        Grid roomLine; roomLine.Children().Append(enter);
        if (manageRooms) MenuAtPointer(enter, RoomMenu(room.id, shownName));
        voiceHandles.insert_or_assign(room.id, roomLine);
        if(active) { Border mark; mark.Width(3); mark.Height(36); mark.CornerRadius(CornerRadius{0,2,2,0}); mark.Background(Brush(Accent)); mark.HorizontalAlignment(HorizontalAlignment::Left); mark.Margin(Thickness{-8,0,0,0}); roomLine.Children().Append(mark); }
        group.Children().Append(roomLine);
        Automation::AutomationProperties::SetName(group, net::Wide(room.name) + L", sala de voz");
        for (auto const& person : room.people) {
          app::ChatPerson peer{person.username, person.name, person.avatar};
          service->ChatModel().Inspect([&](app::ChatState const& s) { for (auto const& p : s.members) if (p.username == person.username) peer = p; });
          auto name = PersonName(peer);
          Grid row; row.Height(36); row.VerticalAlignment(VerticalAlignment::Center);
          for(auto width:{GridLength{28,GridUnitType::Pixel},GridLength{6,GridUnitType::Pixel},GridLength{1,GridUnitType::Star},GridLength{0,GridUnitType::Auto}}) { ColumnDefinition col; col.Width(width); row.ColumnDefinitions().Append(col); }
          row.Padding(Thickness{32,0,8,0});
          auto avatar = PhotoFace(peer, 28); avatar.Name(L"VoiceAvatar");
          Grid face; face.Children().Append(avatar);
          Border ring; ring.Width(28); ring.Height(28); ring.CornerRadius(CornerRadius{14,14,14,14}); ring.BorderBrush(Brush(Success)); ring.BorderThickness(Thickness{2,2,2,2}); ring.IsHitTestVisible(false); ring.Visibility(Visibility::Collapsed);
          face.Children().Append(ring); row.Children().Append(face);
          auto line = Text(name, 16, Muted); line.LineHeight(24); line.FontWeight(Weight(500)); line.CharacterSpacing(-15); line.VerticalAlignment(VerticalAlignment::Center);
          if (person.username==state.voice.self) { Documents::Run suffix; suffix.Text(L" (você)"); line.Inlines().Append(suffix); } // follows the name's colour, hover included
          line.TextWrapping(TextWrapping::NoWrap); line.TextTrimming(TextTrimming::CharacterEllipsis); Grid::SetColumn(line,2); row.Children().Append(line); voiceFaces[person.username] = ring; voiceNames[person.username] = line;
          StackPanel marks; marks.Name(L"VoiceStatusMarks"); marks.Orientation(Orientation::Horizontal); marks.Spacing(4); marks.VerticalAlignment(VerticalAlignment::Center);
          // Grey marks brighten with the name on hover; the red mute mark stays red.
          auto addMark=[&](const char* icon,uint32_t color){auto glyph=VoiceIcon(icon,color,14).as<FrameworkElement>();glyph.VerticalAlignment(VerticalAlignment::Center);marks.Children().Append(glyph);};
          if(person.screen) addMark("display-pulse",Muted);
          if(person.camera) addMark("camera",Muted);
          unsigned greyMarks=marks.Children().Size();
          if(person.muted || person.deafened) addMark(person.deafened ? "volume-xmark" : "microphone-slash",DangerSoftFg);
          auto fillMarks=[marks,greyMarks](uint32_t ink) {for(unsigned i=0;i<greyMarks;++i) IconInk(marks.Children().GetAt(i),ink);};
          Grid::SetColumn(marks,3); row.Children().Append(marks);
          auto personButton = Action(L"", [] {}); personButton.Margin(Thickness{0,2,0,0}); personButton.Content(row); personButton.Height(36); personButton.MinHeight(36);
          personButton.Name(person.username==state.voice.self?L"VoiceSelf":L"VoicePerson");
          personButton.Padding(Thickness{0,0,0,0}); personButton.Background(Tint(0,0));
          personButton.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(Surface2));
          personButton.PointerEntered([line,fillMarks](auto const&,auto const&) { line.Foreground(Brush(Foreground)); fillMarks(Foreground); });
          personButton.PointerExited([this,line,fillMarks,who=person.username](auto const&,auto const&) { auto s=ReadState(); auto it=s.voice.speaking.find(who); line.Foreground(Brush(it!=s.voice.speaking.end() && it->second ? Foreground : Muted)); fillMarks(Muted); });
          personButton.HorizontalAlignment(HorizontalAlignment::Stretch); personButton.HorizontalContentAlignment(HorizontalAlignment::Stretch);
          Automation::AutomationProperties::SetName(personButton, name + L", ver perfil");
          personButton.Click([this,peer](auto const&,auto const&) { OpenProfile(peer); });
          MenuAtPointer(personButton, PersonMenu(peer));
          group.Children().Append(personButton);
        }
        voiceGroups.insert_or_assign(room.id, group);
      }
      for (auto& [who, face] : voiceFaces) {
        auto it = state.voice.speaking.find(who); bool speaking = it != state.voice.speaking.end() && it->second;
        face.Visibility(speaking ? Visibility::Visible : Visibility::Collapsed);
        auto button = FindAncestor(voiceNames[who], who == state.voice.self ? L"VoiceSelf" : L"VoicePerson").try_as<Button>();
        voiceNames[who].Foreground(Brush(speaking || (button && button.IsPointerOver()) ? Foreground : Muted));
      }
      DrawChat(state);
      DrawCall(state);
      ReportPresence();
      DrawDirectCall();
      app::installer::Ready();
      if(!startupUsable && state.model.live) {startupUsable=true;startup::Mark("signed-in-usable");}
    } else {
      if (directLayer) { directLayer.Children().Clear(); directSignature.clear(); Ringing(false); ForgetCallNotice(); }
      if (!page || showingRooms || showingCode != state.needsCode) LoginPage(state);
      error.Text(net::Wide(state.syncError.empty() ? state.error : state.syncError));
      error.Visibility((state.error.empty() && state.syncError.empty()) ? Visibility::Collapsed : Visibility::Visible);
      UpdateSubmit();
      if (!state.busy && !state.needsCode) app::installer::Ready(false);
      submit.Content(box_value(state.busy ? L"Entrando…" : L"Entrar"));
    }
    timer.Done();
  }
  void LoginPage(app::State const& state) {
    ResetAccountView();
    showingRooms = false; showingCode = state.needsCode; recoveryCode = false; recoveryButton = nullptr;
    if (password && state.needsCode) password.Password(L"");
    root.Children().Clear(); root.ColumnDefinitions().Clear();
    page = StackPanel(); page.Spacing(16); page.MaxWidth(384); page.Margin(Thickness{24,24,24,24});
    page.HorizontalAlignment(HorizontalAlignment::Stretch); page.VerticalAlignment(VerticalAlignment::Center);
    StackPanel heading; heading.Spacing(4); heading.Margin(Thickness{0,0,0,8});
    auto wordmark=Text(L"harmony",20);wordmark.FontWeight(Weight(600));heading.Children().Append(wordmark); heading.Children().Append(Text(L"Entre com a sua conta.", 13, 0xa1a1aa));
    page.Children().Append(heading);
    if (state.needsCode) {
      StackPanel field; field.Spacing(4);
      factorLabel = Text(state.method == "email" ? L"Código recebido por e-mail" : L"Código do app", 13, Muted);
      factorLabel.Name(L"FactorLabel"); field.Children().Append(factorLabel);
      code = HarmonyTextBox(); Automation::AutomationProperties::SetName(code, factorLabel.Text()); field.Children().Append(code); page.Children().Append(field);
      if (state.method == "totp") {
        recoveryButton = Action(L"Usar um código de recuperação", [this] { ToggleRecoveryCode(); });
        recoveryButton.Name(L"RecoveryCode"); recoveryButton.Background(Tint(0, 0)); recoveryButton.Foreground(Brush(Muted));
        recoveryButton.HorizontalAlignment(HorizontalAlignment::Left); recoveryButton.Padding(Thickness{0});
        page.Children().Append(recoveryButton);
      }
    } else {
      StackPanel identifier; identifier.Spacing(4); identifier.Children().Append(Text(L"Nome ou e-mail", 13, 0xa1a1aa)); user = HarmonyTextBox();
      Automation::AutomationProperties::SetName(user, L"Nome ou e-mail"); identifier.Children().Append(user); page.Children().Append(identifier);
      StackPanel secret; secret.Spacing(4); secret.Children().Append(Text(L"Senha", 13, 0xa1a1aa)); password = HarmonyPasswordBox();
      Automation::AutomationProperties::SetName(password, L"Senha"); secret.Children().Append(password); page.Children().Append(secret);
    }
    error = Text(L"", 13, 0xf5645b); error.Visibility(Visibility::Collapsed); page.Children().Append(error);
    submit = Action(L"Entrar", [this] { Submit(); }); submit.Style(Resources().Lookup(box_value(L"HarmonyLoginButton")).as<Style>()); submit.HorizontalAlignment(HorizontalAlignment::Stretch); page.Children().Append(submit);
    submit.Resources().Insert(box_value(L"ButtonBackgroundDisabled"),Brush(0x27272a));
    submit.Resources().Insert(box_value(L"ButtonForegroundDisabled"),Brush(0xa1a1aa));
    submit.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0x3592f9));
    if (state.needsCode) code.TextChanged([this](auto const&,auto const&) { UpdateSubmit(); });
    else {
      user.TextChanged([this](auto const&,auto const&) { UpdateSubmit(); });
      password.PasswordChanged([this](auto const&,auto const&) { UpdateSubmit(); });
    }
    if (state.needsCode) page.Children().Append(LoginLink(L"Voltar", [this] { service->RestartLogin(); }));
    else {
      // Both ways in under the button, one line: the invite was alone in the
      // window's corner, far from the form it belongs to.
      Grid links; ColumnDefinition left; left.Width(GridLength{1,GridUnitType::Star}); links.ColumnDefinitions().Append(left);
      ColumnDefinition right; right.Width(GridLength{0,GridUnitType::Auto}); links.ColumnDefinitions().Append(right);
      auto forgot=LoginLink(L"Esqueci a senha",[this] { ForgotPassword(); }); forgot.Name(L"ForgotPassword"); links.Children().Append(forgot);
      auto invite=LoginLink(L"Tenho um convite",[this] {SignupDialog();}); invite.Name(L"OpenInvite"); Grid::SetColumn(invite,1); links.Children().Append(invite);
      page.Children().Append(links);
      page.Children().Append(Text(L"Não existe cadastro aberto. Para entrar é preciso um convite do dono.",13,Muted));
    }
    root.Children().Append(page);
    StackPanel footer;footer.Orientation(Orientation::Horizontal);footer.Spacing(24);footer.Margin(Thickness{24,24,24,24});footer.HorizontalAlignment(HorizontalAlignment::Right);footer.VerticalAlignment(VerticalAlignment::Bottom);

    footer.Children().Append(LoginLink(L"Ajustes",[this] {SettingsPage();}));root.Children().Append(footer);
  }
  void ToggleRecoveryCode() {
    recoveryCode = !recoveryCode;
    factorLabel.Text(recoveryCode ? L"Código de recuperação" : L"Código do app");
    Automation::AutomationProperties::SetName(code, factorLabel.Text());
    recoveryButton.Content(box_value(recoveryCode ? L"Usar o código do app" : L"Usar um código de recuperação"));
    code.Text(L"");
  }
  void Submit() {
    if (!service || service->Read().busy) return;
    if (showingCode) {
      auto value = to_string(code.Text()); if (value.empty()) return; service->Code(std::move(value));
    } else {
      auto name = to_string(user.Text()), value = to_string(password.Password());
      if (name.empty() || value.empty()) return;
      // Keep the fields on a refused attempt, as Auth.tsx does. A successful
      // login replaces this page; an error must not erase what needs fixing.
      service->Login(std::move(name), std::move(value));
    }
  }
  void RoomsPage() {
    roomsSignature.clear(); barSignature.clear(); callSignature.clear(); callView=nullptr; callFaces.clear();
    showingRooms = true; page = nullptr; root.Children().Clear(); root.ColumnDefinitions().Clear();
    ColumnDefinition left; left.Width(GridLength{256, GridUnitType::Pixel}); root.ColumnDefinitions().Append(left);
    ColumnDefinition right; right.Width(GridLength{1, GridUnitType::Star}); root.ColumnDefinitions().Append(right);
    Grid column;
    RowDefinition header; header.Height(GridLength{64,GridUnitType::Pixel}); column.RowDefinitions().Append(header);
    RowDefinition listRow; listRow.Height(GridLength{1,GridUnitType::Star}); column.RowDefinitions().Append(listRow);
    RowDefinition bottom; bottom.Height(GridLength{0,GridUnitType::Auto}); column.RowDefinitions().Append(bottom);
    auto wordmark=Text(L"harmony",20); wordmark.Margin(Thickness{24,0,24,0}); wordmark.VerticalAlignment(VerticalAlignment::Center); column.Children().Append(wordmark);
    StackPanel body; body.Spacing(0); body.Margin(Thickness{8,8,8,8});
    auto heading=Text(L"VOZ",13,0xa1a1aa); heading.Name(L"VoiceHeading"); heading.CharacterSpacing(60);
    heading.Margin(Thickness{8,4,0,4}); heading.FontWeight(winrt::Windows::UI::Text::FontWeight{600}); body.Children().Append(heading);
    roomList = StackPanel(); body.Children().Append(roomList);
    ScrollViewer scroll; scroll.Content(body); ImmediateMouseWheel(scroll);
    Border listIsland; listIsland.Name(L"RoomsIsland"); listIsland.Background(Brush(0x18181b)); listIsland.CornerRadius(CornerRadius{8,8,8,8});
    listIsland.Margin(Thickness{8,8,4,0}); listIsland.Child(scroll); Grid::SetRow(listIsland,1); column.Children().Append(listIsland);
    auto settingsButton = Action(L"Ajustes", [this] { SettingsPage(); }); settingsButton.Margin(Thickness{8,8,8,8}); settingsButton.Background(Brush(0x18181b));
    settingsButton.Height(32); settingsButton.MinHeight(32);
    Border settingsIsland; settingsIsland.Name(L"SettingsIsland"); settingsIsland.Background(Brush(0x18181b)); settingsIsland.CornerRadius(CornerRadius{8,8,8,8});
    voiceBar = StackPanel(); voiceBar.Spacing(0);
    StackPanel bottomContent; bottomContent.Children().Append(voiceBar); bottomContent.Children().Append(settingsButton);
    settingsButton.Name(L"AccountSettings");
    accountSettings = settingsButton;
    settingsIsland.Margin(Thickness{8,8,4,8}); settingsIsland.Child(bottomContent);
    Grid::SetRow(settingsIsland, 2); column.Children().Append(settingsIsland); root.Children().Append(column);
    StackPanel notice; notice.Margin(Thickness{32,32,32,32}); notice.Spacing(16); notice.VerticalAlignment(VerticalAlignment::Center);
    notice.Children().Append(Text(L"Salas de voz",20)); status = Text(L"Conectando…",13,0xa1a1aa); notice.Children().Append(status);
    notice.Children().Append(Text(L"Veja quem está nas salas.",15,0xa1a1aa));
    error = Text(L"",13,0xf5645b); notice.Children().Append(error);
    roomNotice=notice; Grid::SetColumn(notice,1); root.Children().Append(notice);
  }
  void VoiceSettingsPage() { OpenSettings("av"); }
  StackPanel VoiceControls() {
    if(designOutput.empty()) service->VoiceDevices();
    StackPanel controls; controls.Name(L"VoiceSettings"); controls.Spacing(8);
    auto heading=[&](const wchar_t* label) { auto t=Text(label,12,0xa1a1aa); t.CharacterSpacing(60); controls.Children().Append(t); };
    heading(L"MICROFONE"); voiceInput=ComboBox(); voiceInput.Name(L"VoiceInput");
    auto deviceStyle=[](ComboBox box) { box.Height(40); box.HorizontalAlignment(HorizontalAlignment::Stretch); box.Padding(Thickness{12,0,12,0}); box.Background(Brush(0x232325)); box.CornerRadius(CornerRadius{8,8,8,8}); box.BorderThickness(Thickness{0}); box.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); box.FontSize(15); box.FontWeight(winrt::Windows::UI::Text::FontWeight{500}); };
    deviceStyle(voiceInput);
        voiceOutput=ComboBox(); voiceOutput.Name(L"VoiceOutput"); deviceStyle(voiceOutput);
    shownInputs.clear(); shownOutputs.clear(); RefreshVoiceDevices(ReadState().voice);
    voiceInput.SelectionChanged([this](auto const&,auto const&) { if(!syncingDevices && voiceInput.SelectedIndex()>=0) service->VoiceDeviceChoice(true,shownInputs.at(voiceInput.SelectedIndex()).id); });
    voiceOutput.SelectionChanged([this](auto const&,auto const&) { if(!syncingDevices && voiceOutput.SelectedIndex()>=0) service->VoiceDeviceChoice(false,shownOutputs.at(voiceOutput.SelectedIndex()).id); });
    Border microphone; microphone.Name(L"MicrophoneGain"); microphone.Height(44); microphone.Padding(Thickness{12,0,12,0}); microphone.Background(Brush(0x232325)); microphone.CornerRadius(CornerRadius{8,8,8,8});
    Grid micLine; ColumnDefinition micLabel; micLabel.Width(GridLength{76,GridUnitType::Pixel}); micLine.ColumnDefinitions().Append(micLabel);
    ColumnDefinition micSlider; micSlider.Width(GridLength{1,GridUnitType::Star}); micLine.ColumnDefinitions().Append(micSlider);
    ColumnDefinition micValue; micValue.Width(GridLength{52,GridUnitType::Pixel}); micLine.ColumnDefinitions().Append(micValue);
    auto micText=Text(L"Microfone",13); micText.VerticalAlignment(VerticalAlignment::Center); micLine.Children().Append(micText);
    Slider micLevel; HarmonyPointer(micLevel); micLevel.Value(ReadState().voice.micGain*100); micLevel.Maximum(200); micLevel.Height(20); micLevel.MinHeight(20); Grid::SetColumn(micLevel,1); micLine.Children().Append(micLevel);
    auto micPercent=Text(std::to_wstring(int(ReadState().voice.micGain*100))+L"%",13,0xa1a1aa);
    micLevel.ValueChanged([this,micPercent](auto const&,auto const& args) { micPercent.Text(std::to_wstring(int(args.NewValue()))+L"%"); if(designOutput.empty()) service->VoiceMicGain(args.NewValue()/100); else designState.voice.micGain=args.NewValue()/100; }); micPercent.VerticalAlignment(VerticalAlignment::Center); micPercent.HorizontalAlignment(HorizontalAlignment::Right); Grid::SetColumn(micPercent,2); micLine.Children().Append(micPercent); microphone.Child(micLine);
    StackPanel switches; switches.Spacing(0);
    auto state=ReadState().voice;
    auto noise=StartupToggle(state.noise && !state.dfn,[](bool){});
    auto echo=StartupToggle(state.echo,[](bool){}), agc=StartupToggle(state.agc,[](bool){});
    noise.IsEnabled(!state.dfn); noise.Opacity(state.dfn ? 0.5 : 1.0);
    auto apply=[this,n=make_weak(noise),e=make_weak(echo),a=make_weak(agc)] {
      auto ns=n.get(), ec=e.get(), gc=a.get(); if(!ns || !ec || !gc) return;
      bool savedNoise=ns.IsEnabled() ? ns.IsOn() : ReadState().voice.noise;
      if(designOutput.empty()) service->VoiceMicFilters(ec.IsOn(),savedNoise,gc.IsOn());
      else { designState.voice.echo=ec.IsOn(); designState.voice.noise=savedNoise; designState.voice.agc=gc.IsOn(); }
    };
    auto add=[&](ToggleSwitch box,const wchar_t* label) {
      box.Header(box_value(label)); box.Height(34);
      box.Toggled([apply](auto const&,auto const&) { apply(); }); switches.Children().Append(box);
    };
    auto dfn=StartupToggle(state.dfn,[this](bool on) { if(designOutput.empty()) service->VoiceNoise(on,ReadState().voice.strength); else designState.voice.dfn=on; });
    dfn.Name(L"DfnToggle"); dfn.Header(box_value(L"Tirar ruído de fundo (DeepFilterNet)")); dfn.Height(34); switches.Children().Append(dfn);
    add(noise,L"Tirar ruído de fundo (libwebrtc)");
    add(echo,L"Cancelar eco"); add(agc,L"Nivelar automaticamente");
    noise.Name(L"NoiseToggle"); echo.Name(L"EchoToggle"); agc.Name(L"AgcToggle");
    dfn.Toggled([this,noise](auto const& sender,auto const&) {
      bool on=sender.template as<ToggleSwitch>().IsOn();
      noise.IsEnabled(false); noise.IsOn(!on && ReadState().voice.noise);
      noise.IsEnabled(!on); noise.Opacity(on ? 0.5 : 1.0);
    });
    ComboBox cameraChoice; HarmonyPointer(cameraChoice);deviceStyle(cameraChoice);cameraChoice.Name(L"CameraChoice");cameraChoice.Items().Append(box_value(L"Padrão do Windows"));
    auto cameras=camera::Devices();int chosen=0;
    for(size_t i=0;i<cameras.size();++i) {cameraChoice.Items().Append(box_value(net::Wide(cameras[i].name)));if(cameras[i].id==state.cameraID)chosen=int(i)+1;}
    cameraChoice.SelectedIndex(chosen);
    cameraChoice.SelectionChanged([this,cameras](auto const& sender,auto const&) {int index=sender.template as<ComboBox>().SelectedIndex();if(index<0)return;auto v=ReadState().voice;service->Camera(v.sharingCamera,index?cameras[index-1].id:"");});
    Border strength; strength.Name(L"DfnStrength"); strength.Background(Brush(0x232325)); strength.CornerRadius(CornerRadius{8,8,8,8}); strength.Height(44); strength.Padding(Thickness{12,0,12,0});
    Grid line; ColumnDefinition label; label.Width(GridLength{76,GridUnitType::Pixel}); line.ColumnDefinitions().Append(label);
    ColumnDefinition sliderColumn; sliderColumn.Width(GridLength{1,GridUnitType::Star}); line.ColumnDefinitions().Append(sliderColumn);
    ColumnDefinition value; value.Width(GridLength{52,GridUnitType::Pixel}); line.ColumnDefinitions().Append(value);
    auto title=Text(L"Força",13); title.VerticalAlignment(VerticalAlignment::Center); line.Children().Append(title);
    Slider slider; HarmonyPointer(slider); slider.Name(L"DfnSlider"); slider.Minimum(10); slider.Maximum(100); slider.Value(ReadState().voice.strength); slider.IsEnabled(ReadState().voice.dfn); slider.Height(20); slider.MinHeight(20); Grid::SetColumn(slider,1); line.Children().Append(slider);
    auto db=Text(net::Wide(std::to_string(ReadState().voice.strength)+" dB").c_str(),13,0xa1a1aa);
    slider.ValueChanged([this,db](auto const&,auto const& args) { auto value=int(args.NewValue()); db.Text(net::Wide(std::to_string(value)+" dB")); if(designOutput.empty()) service->VoiceNoise(ReadState().voice.dfn,value); else designState.voice.strength=value; });
    dfn.Toggled([slider](auto const& sender,auto const&) { slider.IsEnabled(sender.template as<ToggleSwitch>().IsOn()); }); db.HorizontalAlignment(HorizontalAlignment::Right); db.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(db,2); line.Children().Append(db); strength.Child(line);
    noiseStatus=Text(L"",13,0xa1a1aa); noiseStatus.TextWrapping(TextWrapping::Wrap);
    // Each control under its own heading (the strength used to sit under
    // CÂMERA and the microphone's volume under SAÍDA DE SOM).
    auto section=[&](const wchar_t* label) { auto t=Text(label,12,0xa1a1aa); t.CharacterSpacing(60); t.Margin(Thickness{0,controls.Children().Size()?12.0:0.0,0,0}); controls.Children().Append(t); };
    controls.Children().Clear();
    section(L"MICROFONE"); controls.Children().Append(voiceInput); controls.Children().Append(microphone);
    section(L"SAÍDA DE SOM"); controls.Children().Append(voiceOutput);
    section(L"FILTROS DO MICROFONE"); controls.Children().Append(switches); controls.Children().Append(strength); controls.Children().Append(noiseStatus);
    // "Ouvir minha voz" (the owner, 29/10/2026): the microphone through the
    // filters above, back into the headphones, as the others hear it.
    voiceTestShown=ReadState().voice.voiceTest;
    voiceTestToggle=StartupToggle(voiceTestShown,[this](bool on) { voiceTestShown=on; if(designOutput.empty()) service->TestVoice(on); });
    voiceTestToggle.Name(L"VoiceTestToggle"); voiceTestToggle.Header(box_value(L"Ouvir minha voz")); voiceTestToggle.Height(34);
    voiceTestNote=Text(L"",13,0xa1a1aa); voiceTestNote.TextWrapping(TextWrapping::Wrap);
    section(L"TESTE DE VOZ"); controls.Children().Append(voiceTestToggle); controls.Children().Append(voiceTestNote);
    RefreshVoiceTest(ReadState().voice);
    section(L"CÂMERA"); controls.Children().Append(cameraChoice);
    RefreshNoiseStatus(ReadState().voice);
    error=Text(L"",13,0xf5645b); controls.Children().Append(error);
    return controls;
  }
  // The switch follows the service only when the service changes its mind
  // (entering a room, a device that would not open), never mid-click.
  void RefreshVoiceTest(const app::VoiceState& voice) {
    if(!voiceTestToggle) return;
    if(voice.voiceTest!=voiceTestShown && designOutput.empty()) { voiceTestShown=voice.voiceTest; voiceTestToggle.IsOn(voice.voiceTest); }
    std::string text=!voice.voiceTestError.empty() ? voice.voiceTestError :
      InCall() ? "Use fone de ouvido: você ouve a sua voz na chamada, com os filtros acima. Continua ligado com os Ajustes fechados, até sair da chamada; mudo, ela some." :
      "Use fone de ouvido: você ouve a sua voz com os filtros acima, como os outros ouvem. No alto-falante ela volta para o microfone.";
    voiceTestNote.Text(net::Wide(text));
  }
  bool InCall() { return !ReadState().voice.room.empty() || (service && service->CallRead().phase != app::CallPhase::Idle); }
  void RefreshNoiseStatus(const app::VoiceState& voice) {
    if(!noiseStatus) return;
    auto text=voice.room.empty() ? "Entre numa sala para verificar o filtro." :
      "Filtro em uso: "+voice.noiseFilter+" — "+voice.noiseReason;
    noiseStatus.Text(net::Wide(text));
  }
  // Ajustes as the web's panel (Panels.tsx Settings): one card, at most
  // 880 x 720, the tabs down its left in 208, the X in the content's corner,
  // and the chosen tab's content beside them. Changing tab changes only the
  // content: "Voz e vídeo" used to replace the whole page, menu and all.
  std::string settingsTab = "av";
  Grid settingsOverlay{nullptr};
  TextBlock settingsPreviousError{nullptr};
  ContentDialog logoutDialog{nullptr};
  winrt::Windows::Foundation::IAsyncAction AskLogout() {
    if (logoutDialog) co_return;
    auto lifetime = get_strong();
    ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark);
    dialog.Background(Brush(Surface)); dialog.BorderThickness(Thickness{0}); dialog.CornerRadius(CornerRadius{8, 8, 8, 8});
    dialog.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(400.0));
    dialog.Resources().Insert(box_value(L"ContentDialogBorderWidth"), box_value(Thickness{0}));
    dialog.Resources().Insert(box_value(L"ContentDialogBackground"), Brush(Surface));
    dialog.PrimaryButtonStyle(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
    dialog.CloseButtonStyle(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
    dialog.Resources().Insert(box_value(L"ContentDialogSmokeFill"), Tint(0, .7));
    dialog.Resources().Insert(box_value(L"ContentDialogSeparatorThickness"), box_value(Thickness{0}));
    dialog.Resources().Insert(box_value(L"ContentDialogTopOverlay"), Tint(0, 0));
    dialog.Resources().Insert(box_value(L"ContentDialogPadding"), box_value(Thickness{20, 20, 20, 20}));
    dialog.Title(Text(L"Sair desta conta?", 20));
    dialog.Loaded([this](auto const& sender, auto const&) {
      auto shown = sender.template as<ContentDialog>();
      auto commands = FindDesignPart(shown, L"CommandSpace").as<Grid>();
      auto primary = FindDesignPart(shown, L"PrimaryButton").as<Button>();
      auto cancel = FindDesignPart(shown, L"CloseButton").as<Button>();
      // The default button's style comes from a StaticResource inside WinUI's
      // template. Local properties survive that style switch; replacing a
      // resource on the dialog does not. Cancel remains the safe default.
      for (auto b : {primary, cancel}) {
        b.FontFamily(FontFamily(L"ms-appx:///assets/Inter.ttf#Inter")); b.FontSize(14); b.FontWeight(Weight(500));
        b.Height(44); b.MinHeight(44); b.MinWidth(0); b.Padding(Thickness{16, 0, 16, 0});
        b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0}); b.Foreground(Brush(Foreground));
      }
      NegativeButton(primary); NegativeButton(cancel);
      for (auto key : {L"AccentButtonBackgroundPointerOver", L"AccentButtonBackgroundPressed"}) cancel.Resources().Insert(box_value(key), Brush(Default));
      for (auto key : {L"AccentButtonForegroundPointerOver", L"AccentButtonForegroundPressed"}) cancel.Resources().Insert(box_value(key), Brush(Foreground));
      Grid::SetColumn(cancel, 2); Grid::SetColumn(primary, 4);
      for (uint32_t i = 0; i < commands.ColumnDefinitions().Size(); ++i)
        commands.ColumnDefinitions().GetAt(i).Width(i == 0 ? GridLength{1, GridUnitType::Star} : i == 1 ? GridLength{0, GridUnitType::Pixel} : i == 3 ? GridLength{8, GridUnitType::Pixel} : GridLength{0, GridUnitType::Auto});
    });
    StackPanel body; body.Spacing(12);
    body.Children().Append(Text(L"Neste aparelho, o que você deixou escrito e não mandou vai embora junto. Para voltar, entre de novo com a sua senha.", 13, Muted));
    app::ChatPerson self;
    service->ChatModel().Inspect([&](app::ChatState const& s) { self.username = s.owner; for (auto const& p : s.members) if (p.username == s.owner) self = p; });
    StackPanel who; who.Orientation(Orientation::Horizontal); who.Spacing(12);
    who.Children().Append(PhotoFace(self, 32));
    StackPanel names; names.VerticalAlignment(VerticalAlignment::Center);
    names.Children().Append(Text(PersonName(self), 13)); names.Children().Append(Text(L"@" + net::Wide(self.username), 13, Muted));
    who.Children().Append(names);
    Border identity; identity.CornerRadius(CornerRadius{8, 8, 8, 8}); identity.Background(Brush(Surface2)); identity.Padding(Thickness{12, 12, 12, 12}); identity.Child(who); body.Children().Append(identity);
    dialog.Content(body); dialog.PrimaryButtonText(L"Sair"); dialog.CloseButtonText(L"Cancelar"); dialog.DefaultButton(ContentDialogButton::Close);
    logoutDialog = dialog;
    try {
      auto result = co_await dialog.ShowAsync();
      logoutDialog = nullptr;
      if (result == ContentDialogResult::Primary && !exiting) service->Logout();
    } catch (hresult_error const&) { logoutDialog = nullptr; logf("logout confirmation failed"); }
  }
  void SettingsPage() { OpenSettings(settingsTab); }
  void CloseSettings() {
    if(!settings)return;
    diagnosticHistory.Active(false);
    ClearAccountSecrets();
    if (meter) meter.Stop();
    // In a room or call the switch is the call's sidetone and stays on.
    if (ReadState().voice.voiceTest && !InCall() && designOutput.empty()) service->TestVoice(false);
    voiceTestToggle=nullptr; voiceTestNote=nullptr;
    uint32_t at=0; if (settingsOverlay && root.Children().IndexOf(settingsOverlay,at)) root.Children().RemoveAt(at);
    rolesPanel=nullptr; settingsOverlay=nullptr; settings=false; error=settingsPreviousError; settingsPreviousError=nullptr; Render();
  }
  void OpenSettings(std::string tab) {
    diagnosticHistory.Active(false);
    ClearAccountSecrets();
    bool signedIn = ReadState().signedIn;
    // Cargos only for whoever manages roles, the owner always (Panels.tsx).
    rolesTab = signedIn && CanManageRoles();
    if (!signedIn && (tab == "av" || tab == "conta")) tab = "app";
    if (tab == "cargos" && !rolesTab) tab = "conta";
    // Another tab stops the voice test, as closing Ajustes does.
    if (tab != "av" && ReadState().voice.voiceTest && !InCall() && designOutput.empty()) service->TestVoice(false);
    settingsTab = tab;
    diagnosticHistory.Active(tab=="diag" && mediaShown);
    noiseStatus = nullptr; voiceTestToggle = nullptr; voiceTestNote = nullptr; voiceInput = nullptr; voiceOutput = nullptr; measurements = nullptr; rolesPanel = nullptr;
    CloseProfile();
    if (!settings) settingsPreviousError=error;
    uint32_t overlayAt=0;
    if (settingsOverlay && root.Children().IndexOf(settingsOverlay,overlayAt)) root.Children().RemoveAt(overlayAt);
    settingsOverlay=Grid(); settingsOverlay.Name(L"SettingsOverlay"); settingsOverlay.Background(Tint(0,.65));
    Grid::SetColumnSpan(settingsOverlay,(std::max)(1,int(root.ColumnDefinitions().Size())));
    Grid::SetRowSpan(settingsOverlay,(std::max)(1,int(root.RowDefinitions().Size())));
    settingsOverlay.Tapped([this](auto const&,auto const&) { CloseSettings(); });
    settings = true; settingsSignedIn = signedIn; if (meter) meter.Stop();
    Border card; card.Name(L"SettingsModal"); card.MaxWidth(880); card.MaxHeight(720); card.Margin(Thickness{16,16,16,16}); card.Padding(Thickness{24,24,24,24});
    card.Tapped([](auto const&,Input::TappedRoutedEventArgs const& e) { e.Handled(true); });
    card.CornerRadius(CornerRadius{16, 16, 16, 16}); card.Background(Brush(0x18181b));
    Grid layout; layout.ColumnSpacing(16); ColumnDefinition nav; nav.Width(GridLength{208, GridUnitType::Pixel}); layout.ColumnDefinitions().Append(nav);
    ColumnDefinition content; content.Width(GridLength{1, GridUnitType::Star}); layout.ColumnDefinitions().Append(content);
    StackPanel menu; menu.Spacing(8);
    auto title = Text(L"Ajustes", 20); title.Height(40); title.Padding(Thickness{8, 6, 0, 0}); menu.Children().Append(title);
    StackPanel tabs; tabs.Spacing(4); tabs.Name(L"SettingsTabs");
    Button first{nullptr};
    auto addTab = [&](std::string id, std::wstring const& label, std::string icon) {
      bool on = id == tab;
      Button b; HarmonyPointer(b); b.Height(40); b.MinHeight(40); b.HorizontalAlignment(HorizontalAlignment::Stretch); b.HorizontalContentAlignment(HorizontalAlignment::Left);
      b.Padding(Thickness{12, 0, 12, 0}); b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0});
      b.Background(on ? Brush(0x27272a) : SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
      b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(on ? 0x27272a : 0x232325));
      b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(0x27272a));
      StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8);
      auto glyph = VoiceIcon(icon, on ? 0xfcfcfc : 0xa1a1aa, 16).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph);
      auto words = Text(label, 16, on ? 0xfcfcfc : 0xa1a1aa); words.FontWeight(winrt::Windows::UI::Text::FontWeight{500}); words.TextWrapping(TextWrapping::NoWrap);
      words.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(words);
      b.Content(row); b.Name(net::Wide("SettingsTab-" + id)); Automation::AutomationProperties::SetName(b, label);
      b.Click([this, id](auto const&, auto const&) { if (id != settingsTab) OpenSettings(id); });
      tabs.Children().Append(b); if (on) first = b;
    };
    if (signedIn) addTab("av", L"Voz e vídeo", "microphone");
    addTab("app", L"Aplicativo", "gear");
    if (signedIn) addTab("conta", L"Conta", "person");
    if (rolesTab) addTab("cargos", L"Cargos", "persons");
    addTab("diag", L"Diagnóstico", "file-text");
    menu.Children().Append(tabs); layout.Children().Append(menu);

    StackPanel body; body.Spacing(16);
    if (tab == "av") body.Children().Append(VoiceControls());
    else if (tab == "app") {
      auto startup = StartupToggle(app::StartupEnabled(), [this](bool enabled) {
        if (!designOutput.empty()) return;
        if (!app::SetStartup(enabled)) { error.Foreground(Brush(0xf5645b)); error.Text(L"Não foi possível mudar a abertura com o Windows."); }
        else logf("startup preference changed");
      }); startup.Name(L"Startup"); body.Children().Append(startup);
      auto hint = Text(L"Abre minimizado na bandeja.", 13, 0xa1a1aa); hint.Margin(Thickness{0, -12, 0, 0}); body.Children().Append(hint);
      body.Children().Append(PipToggle());
      auto pipHint = Text(L"Ao minimizar o harmony numa chamada direta com a câmera ou a tela do outro ligada, a imagem fica num canto da tela, por cima de tudo.", 13, Muted);
      pipHint.TextWrapping(TextWrapping::Wrap); pipHint.Margin(Thickness{0, -12, 0, 0}); body.Children().Append(pipHint);
      if (signedIn) {
        body.Children().Append(NoticesToggle());
        // The Ajustes' promise (the web's, 03/10/2026): who and where, never what.
        auto said = Text(L"Menção, mensagem direta e ligação, com o harmony aberto ou na bandeja. O aviso diz quem e onde, nunca o texto da mensagem. Canal silenciado não avisa.", 13, 0xa1a1aa);
        said.TextWrapping(TextWrapping::Wrap); said.Margin(Thickness{0, -12, 0, 0}); body.Children().Append(said);
        body.Children().Append(SoundsToggle());
        auto soundHint=Text(L"Toca em outra conversa, com a janela atrás ou em uma menção. Conversa silenciada não toca. O toque de ligação é separado.",13,Muted);
        soundHint.TextWrapping(TextWrapping::Wrap); soundHint.Margin(Thickness{0,-12,0,0}); body.Children().Append(soundHint);
      }
      auto quit = Action(L"Fechar programa", [this] { Quit(); }); quit.Name(L"QuitProgram"); body.Children().Append(quit);
      body.Children().Append(Text(L"Fechar a janela deixa o harmony na bandeja, com a chamada; sair do programa encerra tudo.", 13, 0xa1a1aa));
    } else if (tab == "conta") {
      app::ChatPerson self; std::string me;
      if (service) service->ChatModel().Inspect([&](app::ChatState const& s) { me = s.owner; for (auto const& p : s.members) if (p.username == me) self = p; });
      if (self.username.empty()) self.username = me;
      body.Spacing(24);
      ProfileControls(body,self);
      StackPanel you; you.Spacing(8); you.Children().Append(Text(L"VOCÊ",13,Muted));
      StackPanel facts; facts.Spacing(2);
      facts.Children().Append(AccountCard(AccountRow(L"Nome",PersonName(self))));
      facts.Children().Append(AccountCard(AccountRow(L"Usuário",L"@"+net::Wide(self.username))));
      you.Children().Append(facts); body.Children().Append(you);
      AccountControls(body);
      PersonalControls(body);

      auto logout = Action(L"Sair desta conta", [this] { AskLogout(); }); logout.Name(L"AskLogout"); NegativeButton(logout); logout.FontSize(14); logout.FontWeight(Weight(500));
      logout.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(0xe15451)); logout.Resources().Insert(box_value(L"ButtonBackgroundPressed"),Brush(0xe15451));
      logout.PointerEntered([](auto const& eventSender,auto const&) { auto logout = eventSender.template as<Button>(); logout.Foreground(Brush(Foreground)); });  body.Children().Append(logout);
      body.Children().Append(Text(L"Emojis: Twemoji, © Twitter e colaboradores, licença CC-BY 4.0.", 13, Muted));
    } else if (tab == "cargos") {
      RolesControls(body);
    } else if(tab=="net") {
      NetworkControls(body);
    } else {
      DiagnosticControls(body);
    }
    if (tab != "av") { error = Text(L"", 13, 0xf5645b); body.Children().Append(error); }
    ScrollViewer scroll;scroll.Name(L"SettingsScroll"); scroll.Content(body); ImmediateMouseWheel(scroll); scroll.Padding(Thickness{0, 0, 16, 0}); scroll.Margin(Thickness{0,40,32,0}); scroll.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    Grid::SetColumn(scroll, 1); layout.Children().Append(scroll);
    auto close = Action(L"", [this] { CloseSettings(); }); close.Name(L"CloseSettings");
    close.Width(32); close.Height(32); close.MinWidth(32); close.MinHeight(32); close.Padding(Thickness{0}); close.Margin(Thickness{0});
    close.Style(Resources().Lookup(box_value(L"HarmonyDangerButton")).as<Style>());
    close.Background(Brush(0xdb3b3e)); close.Content(VoiceIcon("xmark",0xfcfcfc,16));
    close.PointerEntered([](auto const& eventSender,auto const&) { auto close = eventSender.template as<Button>();IconInk(close.Content(),0xfcfcfc);});
    close.PointerExited([](auto const& eventSender,auto const&) { auto close = eventSender.template as<Button>();IconInk(close.Content(),0xfcfcfc);});
    close.HorizontalAlignment(HorizontalAlignment::Right); close.VerticalAlignment(VerticalAlignment::Top);
    Automation::AutomationProperties::SetName(close, L"Fechar ajustes"); ToolTipService::SetToolTip(close, box_value(L"fechar"));
    Grid::SetColumn(close, 1); layout.Children().Append(close);
    // Escape closes, as every panel on the web does.
    Input::KeyboardAccelerator escape; escape.Key(winrt::Windows::System::VirtualKey::Escape);
    escape.Invoked([this](auto const&, Input::KeyboardAcceleratorInvokedEventArgs const& e) { e.Handled(true); CloseSettings(); });
    card.KeyboardAccelerators().Append(escape);
    card.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
    card.Child(layout); settingsOverlay.Children().Append(card); root.Children().Append(settingsOverlay);
    if (first && !AuditingPersonal()) first.Focus(FocusState::Programmatic);
  }
  void RefreshVoiceDevices(app::VoiceState const& state) {
    if (!voiceInput || !voiceOutput) return;
    syncingDevices = true;
    auto update = [](ComboBox box, std::vector<app::VoiceDevice>& old, const std::vector<app::VoiceDevice>& current, const std::string& selected) {
      if (old != current) { box.Items().Clear(); for (auto& d : current) box.Items().Append(box_value(net::Wide(d.name))); old = current; }
      int index = 0; for (size_t i=0;i<current.size();++i) if (current[i].id == selected) index = int(i);
      box.SelectedIndex(index);
    };
    update(voiceInput,shownInputs,state.inputs,state.input); update(voiceOutput,shownOutputs,state.outputs,state.output);
    syncingDevices = false;
  }
  // CallKit.tsx Quality: three round bars, green, amber or red by round trip
  // and loss (under 150 ms and 2% good, from 300 ms or 5% bad). Two shapes:
  // the bare bars beside "Voz conectada" and the chip "Boa · 38 ms" in the
  // room's bar; both open the same three numbers (the owner, 22/10/2026:
  // "falta o que aparece na tela de chamada e mostrar mais detalhes").
  struct QualityMark { Button button{nullptr}; std::array<Border,3> bars{nullptr,nullptr,nullptr}; TextBlock words{nullptr}; };
  std::array<QualityMark,2> qualityMarks; /* the column's bars, the room's chip: the newest of each */ DispatcherTimer qualityTimer{nullptr};
  StackPanel qualityDetail{nullptr}; std::array<TextBlock,4> qualityValues{nullptr,nullptr,nullptr,nullptr};
  struct QualityReading { bool shown=false,bad=false,rough=false; network::Sample sample; };
  QualityReading ReadQuality() {
    QualityReading q; q.sample=network::Latest(false).Read();
    q.shown=!ReadState().voice.room.empty() && q.sample.rtt>=0;
    q.bad=q.sample.rtt>=300 || q.sample.loss>=5; q.rough=q.sample.rtt>=150 || q.sample.loss>=2; return q;
  }
  static uint32_t QualityTone(QualityReading const& q) { return q.bad?0xf5645b:q.rough?0xf5a524:Success; }
  static std::wstring QualityWord(QualityReading const& q) { return q.bad?L"ruim":q.rough?L"média":L"boa"; }
  void SyncQualityTimer() {
    if(!qualityTimer) return;
    if(!exiting && mediaShown && InCall()) {if(!qualityTimer.IsEnabled()) qualityTimer.Start();}
    else if(qualityTimer.IsEnabled()) qualityTimer.Stop();
  }
  void RefreshQuality() {
    auto q=ReadQuality(); int lit=q.bad?1:q.rough?2:3; auto tone=QualityTone(q); auto word=QualityWord(q);
    for (auto& m:qualityMarks) {
      if (!m.button) continue;
      m.button.Visibility(q.shown?Visibility::Visible:Visibility::Collapsed); if(!q.shown) continue;
      for (int i=0;i<3;++i) m.bars[i].Background(Brush(i<lit?tone:Surface3));
      auto ms=std::to_wstring(int(q.sample.rtt));
      if (m.words) m.words.Text(std::wstring(1,wchar_t(towupper(word[0])))+word.substr(1)+L" · "+ms+L" ms");
      ToolTipService::SetToolTip(m.button, box_value(L"Conexão "+word+L" — "+ms+L" ms. Clique para os detalhes."));
      Automation::AutomationProperties::SetName(m.button, L"Conexão "+word+L", "+ms+L" ms");
    }
    if (qualityDetail && q.shown) {
      qualityValues[0].Text(L"Conexão "+word); qualityValues[0].Foreground(Brush(tone));
      qualityValues[1].Text(std::to_wstring(int(q.sample.rtt))+L" ms");
      qualityValues[2].Text(q.sample.loss<0?L"—":std::to_wstring(int(std::lround(q.sample.loss)))+L"%");
      qualityValues[3].Text(q.sample.jitter<0?L"—":std::to_wstring(int(std::lround(q.sample.jitter)))+L" ms");
    }
  }
  void OpenQualityDetail(FrameworkElement target) {
    Flyout flyout; StackPanel content; content.Width(208); content.Spacing(4);
    qualityValues[0]=Line(L"",13,Success,600); qualityValues[0].Margin(Thickness{0,0,0,4}); content.Children().Append(qualityValues[0]);
    std::array<std::pair<wchar_t const*,wchar_t const*>,3> rows{{{L"Ida e volta",L"Quanto a sua voz demora para chegar e voltar"},{L"Perdido",L"Quanto da voz não chegou nos últimos segundos"},{L"Irregular",L"Quanto o ritmo da chegada varia"}}};
    for (int i=0;i<3;++i) {
      Grid row; ColumnDefinition name; name.Width(GridLength{1,GridUnitType::Star}); row.ColumnDefinitions().Append(name);
      ColumnDefinition value; value.Width(GridLength{0,GridUnitType::Auto}); row.ColumnDefinitions().Append(value);
      row.Children().Append(Line(rows[i].first,13,Muted,500)); qualityValues[i+1]=Line(L"",13,Foreground,600); Grid::SetColumn(qualityValues[i+1],1); row.Children().Append(qualityValues[i+1]);
      ToolTipService::SetToolTip(row, box_value(rows[i].second)); content.Children().Append(row);
    }
    content.Name(L"QualityDetail"); flyout.Content(content); qualityDetail=content; RefreshQuality();
    flyout.Closed([this,content](auto const&,auto const&){ if(qualityDetail==content) qualityDetail=nullptr; });
    flyout.ShowAt(target);
  }
  Button QualityButton(bool chip) {
    QualityMark m; StackPanel inside; inside.Orientation(Orientation::Horizontal); inside.Spacing(8); inside.VerticalAlignment(VerticalAlignment::Center);
    StackPanel bars; bars.Orientation(Orientation::Horizontal); bars.Spacing(1); bars.VerticalAlignment(VerticalAlignment::Center);
    for (int i=0;i<3;++i) { Border bar; bar.Width(3); bar.Height(i==0?6:i==1?10:14); bar.CornerRadius(CornerRadius{1.5,1.5,1.5,1.5}); bar.VerticalAlignment(VerticalAlignment::Bottom); bar.Background(Brush(Surface3)); m.bars[i]=bar; bars.Children().Append(bar); }
    inside.Children().Append(bars);
    if (chip) { m.words=Line(L"",13,Muted,500); m.words.VerticalAlignment(VerticalAlignment::Center); inside.Children().Append(m.words); }
    Button button; HarmonyPointer(button); button.Style(Resources().Lookup(box_value(L"HarmonyButton")).as<Style>());
    button.Content(inside); button.MinWidth(0); button.MinHeight(0); button.CornerRadius(CornerRadius{8,8,8,8}); button.BorderThickness(Thickness{0,0,0,0});
    if (chip) { button.Name(L"QualityChip"); button.Height(44); button.Padding(Thickness{16,0,16,0}); button.Background(Brush(Default)); }
    else { button.Name(L"QualityBars"); button.Height(24); button.Padding(Thickness{4,0,4,0}); button.Background(Tint(0,0));
      button.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"),Brush(Surface2)); button.Resources().Insert(box_value(L"ButtonBackgroundPressed"),Brush(Surface2)); }
    button.VerticalAlignment(VerticalAlignment::Center); button.Visibility(Visibility::Collapsed);
    button.Click([this](auto const& sender,auto const&){ if(!exiting) OpenQualityDetail(sender.template as<FrameworkElement>()); });
    m.button=button; qualityMarks[chip?1:0]=m; RefreshQuality();
    if (!qualityTimer) { qualityTimer=DispatcherTimer(); qualityTimer.Interval(std::chrono::seconds(1)); qualityTimer.Tick([this](auto const&,auto const&){ if(!exiting) RefreshQuality(); }); SyncQualityTimer(); }
    return button;
  }
  void DrawVoiceBar(app::VoiceState const& state) {
    if (!voiceBar) return;
    std::string key = state.connectionStep + state.inputStatus + state.outputStatus + state.room + state.roomName + state.self + (state.connecting?"c":"_") + (state.reconnecting?"r":"_") + (state.muted?"m":"_") + (state.deafened?"d":"_")+(state.sharingScreen?"s":"_")+(state.sharingCamera?"v":"_")+state.mediaNotice;
    if (key == barSignature) return; barSignature = key; voiceBar.Children().Clear();
    if (accountToggles) accountToggles.Children().Clear();
    if (state.room.empty()) return;
    // Voice.tsx VoiceBar: one line, pb-2 pl-4 pr-2 - the status at 13/500,
    // the room muted, and the room's own red "Sair" square of 32.
    Grid line; line.Name(L"VoiceBar"); line.Margin(Thickness{16,0,8,8}); line.ColumnSpacing(8);
    ColumnDefinition textColumn; textColumn.Width(GridLength{1,GridUnitType::Star}); line.ColumnDefinitions().Append(textColumn);
    for(int i=0;i<2;++i) { ColumnDefinition control; control.Width(GridLength{32,GridUnitType::Pixel}); line.ColumnDefinitions().Append(control); }
    // Voice.tsx: the status, the room, the bars, in one line. Short of room,
    // the room's name gives way first; the status and the bars never do.
    Grid statusRow; statusRow.ColumnSpacing(6);
    for (auto w : {GridLength{0,GridUnitType::Auto}, GridLength{1,GridUnitType::Star}, GridLength{0,GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); statusRow.ColumnDefinitions().Append(c); }
    auto status = Line(state.reconnecting?L"Reconectando…":state.connecting?L"Ligando o áudio…":L"Voz conectada",13,state.connecting||state.reconnecting?Muted:Foreground,600); // 500 reads thin at 13 px on Windows
    status.VerticalAlignment(VerticalAlignment::Center); statusRow.Children().Append(status);
    auto room = Line(net::Wide(state.roomName),13,Muted,500); room.TextTrimming(TextTrimming::CharacterEllipsis); room.TextWrapping(TextWrapping::NoWrap);
    room.VerticalAlignment(VerticalAlignment::Center); room.HorizontalAlignment(HorizontalAlignment::Left); Grid::SetColumn(room,1); statusRow.Children().Append(room);
    auto bars=QualityButton(false); bars.Margin(Thickness{-4,0,0,0}); Grid::SetColumn(bars,2); statusRow.Children().Append(bars);
    line.Children().Append(statusRow);
    auto share = RoomScreenButton(state.sharingScreen,32); share.Name(L"VoiceShareScreen"); Grid::SetColumn(share,1); line.Children().Append(share);
    auto leave = Square("handset-arrow-out", L"Sair da sala", 32, 20, 0xfcfcfc, 0xe15451, [this] { service->LeaveVoice(); });
    leave.Background(Brush(0xdb3b3e)); Grid::SetColumn(leave,2); line.Children().Append(leave); voiceBar.Children().Append(line);
    // Microphone and hearing on the account's line, 32 on surface-2 and red
    // when switched off; deafened turns the microphone off as well, and looks it.
    if (accountToggles) {
      bool micOff = state.muted || state.deafened;
      auto mute = Square(micOff?"microphone-slash":"microphone", micOff?L"Ligar o microfone":L"Desligar o microfone", 32, 20, micOff?0xf5645b:0xfcfcfc, 0x323235,
                         [this] { auto v = ReadState().voice; service->MuteVoice(!v.muted,v.deafened); });
      mute.Name(L"VoiceMute"); mute.Background(Brush(0x232325)); mute.IsEnabled(!state.deafened);
      if (state.deafened) Automation::AutomationProperties::SetName(mute, L"Seu microfone está desligado porque você não está ouvindo");
      auto deafen = Square(state.deafened?"volume-xmark":"headphones", state.deafened?L"Voltar a ouvir":L"Não ouvir ninguém", 32, 20, state.deafened?0xf5645b:0xfcfcfc, 0x323235,
                           [this] { auto v = ReadState().voice; service->MuteVoice(v.muted,!v.deafened); });
      deafen.Name(L"VoiceDeafen"); deafen.Background(Brush(0x232325));
      accountToggles.Children().Append(mute); accountToggles.Children().Append(deafen);
    }
    auto notice = [&](std::wstring const& text, uint32_t color) { auto t = Text(text,13,color); t.TextWrapping(TextWrapping::Wrap); t.Margin(Thickness{16,0,8,8}); voiceBar.Children().Append(t); };
    // A closed window keeps transmitting its last picture with the notice
    // (W13): the owner stops it or picks another window.
    if(state.mediaNotice=="window_closed") notice(L"A janela fechou: quem assiste vê o aviso. Pare ou escolha outra janela.",0xf5a524);
    if(state.mediaNotice=="capture_lost") notice(L"A transmissão parou: a captura foi perdida.",0xf5645b);
    if(state.mediaNotice=="minimized") notice(L"Janela minimizada: quem assiste vê a última imagem.",0xa1a1aa);
    if(state.mediaNotice=="camera_unavailable") notice(L"Não consegui abrir a câmera. Confira permissão e Ajustes.",0xf5645b);
    // The web's words for the server's limit on one connection (api.ts).
    if(state.mediaNotice=="reconnect_needed") notice(L"Saia da sala e entre de novo para compartilhar outra vez.",0xf5645b);
    if(state.mediaNotice=="screen_offer_failed") notice(L"Não consegui começar a transmissão. Tente de novo.",0xf5645b);
    if(state.inputStatus=="failed" || state.outputStatus=="failed") notice(L"Dispositivo indisponível. Confira os Ajustes de voz.",0xf5645b);
    else if(state.inputStatus=="opening" || state.outputStatus=="opening") notice(L"Aguardando áudio do dispositivo…",0xa1a1aa);
  }
  void UpdateSubmit() {
    if (!submit || settings || showingRooms) return;
    bool filled = showingCode ? code && !code.Text().empty() : user && password && !user.Text().empty() && !password.Password().empty();
    submit.IsEnabled(!ReadState().busy && filled);
  }
  void CopyStatus(bool copied) {
    error.Name(L"CopyFeedback"); error.Foreground(Brush(copied ? 0x0485f7 : 0xf5645b));
    error.Text(copied ? L"Registro copiado." : L"Não foi possível copiar o registro.");
  }
};
}
int __stdcall wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
  startup::Mark("native-entry");
  // Every swap chain owner holds a gpu::DeviceLease, so the device can go
  // when the last one does (W13: ~200 MB kept after a call otherwise).
  gpu::EnableIdleDeviceRelease();
  if(!app::GuardTestParent())return 3;
  try { if (auto root = app::installer::TestRoot(); !root.empty()) {
    controlClassStorage += L".Installer." + root.filename().wstring();
    ControlClass = controlClassStorage.c_str();
  } } catch (...) { return 2; }
  logf("harmony nativo starting");
  perf::NameThread(L"harmony-ui");
  try {
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if(argc==3 && std::wstring(argv[1])==L"--apply-update-after") {auto pid=wcstoul(argv[2],nullptr,10);LocalFree(argv);return app::installer::ApplyAfter(pid);}
    if(argc==4&&std::wstring(argv[1])==L"--camera-worker") {auto pipe=_wcstoui64(argv[2],nullptr,10);auto id=net::Narrow(argv[3]);LocalFree(argv);return camera::WorkerMain(pipe,id);}
#ifndef HARMONY_PUBLISH
    if(argc==2&&std::wstring(argv[1])==L"--camera-deadline-test") {LocalFree(argv);return camera::DeadlineSelfTest();}
    std::filesystem::path selfTest, voiceTest,screenTest, directTest;
#endif
    std::filesystem::path shortcut, shortcutTarget;
    for (int i=1; i<argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--bandeja") startHidden = true;
#ifndef HARMONY_PUBLISH
      else if (arg == L"--resource-audit") {
        if(resource_meter::Output().empty()) {LocalFree(argv);return 2;}
        resourceAudit=true;designOutput=resource_meter::Output().parent_path();
        SetEnvironmentVariableW(L"HARMONY_URL",L"http://127.0.0.1:9");
      }
      else if (arg == L"--autoteste-ui" && i+1<argc) uiTestOutput = argv[++i];
      else if (arg == L"--autoteste-voz" && i+1<argc) voiceTest = argv[++i];
      else if (arg == L"--autoteste-tela" && i+1<argc) screenTest = argv[++i];
      else if (arg == L"--autoteste" && i+1<argc) selfTest = argv[++i];
      else if (arg == L"--design-audit-w3" && i+1<argc) { voiceDesign = true; designOutput = argv[++i]; }
      else if (arg == L"--design-audit-w4" && i+1<argc) { screenDesign = true; designOutput = argv[++i]; }
      else if (arg == L"--design-audit-call" && i+1<argc) { callDesign = true; designOutput = argv[++i]; }
      else if (arg == L"--design-audit" && i+1<argc) designOutput = argv[++i];
      else if (arg == L"--chat-audit" && i+1<argc) chatOutput = argv[++i];
      else if (arg == L"--call-audit" && i+1<argc) callOutput = argv[++i];
#endif
      else if (arg == L"--atalho" && i+2<argc) { shortcut = argv[++i]; shortcutTarget = argv[++i]; }
#ifndef HARMONY_PUBLISH
      else if (arg == L"--autoteste-chamada" && i+1<argc) directTest = argv[++i];
#endif
      // A click on a notice of a copy that is gone starts the program
      // through COM (toast.cpp): it opens, registers its activator, and
      // Windows then hands it the click (NoticeAction checks the call is
      // still there before answering anything).
      else if (arg.rfind(L"----AppNotificationActivated:", 0) == 0 || arg == L"-Embedding") {}
      else { LocalFree(argv); return 2; }
    }
    LocalFree(argv);
    if (!shortcut.empty()) {
      init_apartment(apartment_type::single_threaded);
      return app::CreateShortcut(shortcut,shortcutTarget) ? 0 : 1;
    }
#ifndef HARMONY_PUBLISH
    if (!voiceTest.empty()) return app::VoiceSelfTest(voiceTest, app::Environment(L"HARMONY_URL"));
    if (!directTest.empty()) return app::DirectSelfTest(directTest, app::Environment(L"HARMONY_URL"));
    if (!screenTest.empty()) return app::ScreenSelfTest(screenTest, app::Environment(L"HARMONY_URL"));
    if (!selfTest.empty()) {
      auto base = app::Environment(L"HARMONY_URL");
      if (base.empty()) return 2;
      return app::SelfTest(selfTest, base);
    }
#endif
    // A measuring run (--design-audit*, --chat-audit) is not the program: it
    // must neither be turned away by the copy the owner has open nor take its
    // place, so it does not take part in the one-copy rule.
    bool measuring = !uiTestOutput.empty() || !designOutput.empty() || !chatOutput.empty() || !callOutput.empty();
    auto name = MutexName(); HANDLE instance = measuring ? nullptr : CreateMutexW(nullptr, FALSE, name.c_str());
    if (!instance && !measuring) return 1;
    if (!measuring && GetLastError() == ERROR_ALREADY_EXISTS) {
      for (int i=0; i<50; ++i) {
        if (auto window = FindWindowW(ControlClass,ControlClass)) {
          DWORD owner{}; GetWindowThreadProcessId(window,&owner); AllowSetForegroundWindow(owner);
          if(!startHidden) PostMessageW(window,OpenMessage,0,0); break;
        }
        Sleep(100);
      }
      CloseHandle(instance); return 0;
    }
    // The W6 round's notices go out under a name of their own, so that it
    // never touches the identity of the copy the owner has installed.
    check_hresult(SetCurrentProcessExplicitAppUserModelID(callOutput.empty() ? app::AppID : L"Harmony.Native.Windows.Audit"));
    startup::Mark("before-winui");
    init_apartment(apartment_type::single_threaded);
    app::installer::Updater updater(!measuring);
    Application::Start([&](auto const&) { make<App>(&updater); }); if (instance) CloseHandle(instance); return 0;
  } catch (hresult_error const& e) { logf("startup failed HRESULT %ld", e.code().value); return 1; }
    catch (...) { logf("startup failed"); return 1; }
}
