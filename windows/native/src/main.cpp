// harmony-w1: the W1 proof (PLANO-WINDOWS §4). No chat screen: it signs in,
// joins a voice room, talks with echo cancellation, shares one window as a
// three-layer H.264 screen through the GPU path, and watches one person's
// screen full-screen. The "Desempenho" panel is born here.
//
//   harmony-w1.exe --url http://localhost:8091 --user ana [--room Geral]
//                  [--share "window title part"] [--fps 60] [--watch bia]
//                  [--no-mic] [--no-system-audio] [--seconds N] [--no-panel]
//   password in HARMONY_PASSWORD; HARMONY_LOG_FILE and HARMONY_PERF_FILE
//   receive the log and the 1 Hz meter lines (the bench reads them).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "log.h"
#include "net.h"
#include "api/make_ref_counted.h"
#include "direct.h"
#include "json.h"
#include "perf.h"
#include "stats.h"
#include "voice.h"
#if HARMONY_GPU
#include "../gpu/gpu.h"
#endif

namespace {

struct Args {
  std::string url = "http://localhost:8091";
  std::string user, room, share, watch;
  int fps = 60;
  bool mic = true;
  bool system_audio = true;
  bool panel = true;
  bool fullscreen = false;  // the watched screen opens full screen
  bool idle = false;        // signed in, socket open, no room: "in the tray"
  int seconds = 0;  // 0 = until the window closes
  // The direct-call proof: ring this person, or answer the first call; then
  // restartIce at these wall-clock instants (ms since 1970), on both sides.
  std::string call;
  bool answer = false;
  long long glare_at = 0;
  int glare_count = 5, glare_period_ms = 5000;
};

Args Parse(int argc, wchar_t** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = net::Narrow(argv[i]);
    auto next = [&]() { return i + 1 < argc ? net::Narrow(argv[++i]) : std::string(); };
    if (k == "--url") a.url = next();
    else if (k == "--user") a.user = next();
    else if (k == "--room") a.room = next();
    else if (k == "--share") a.share = next();
    else if (k == "--watch") a.watch = next();
    else if (k == "--fps") a.fps = std::atoi(next().c_str());
    else if (k == "--seconds") a.seconds = std::atoi(next().c_str());
    else if (k == "--no-mic") a.mic = false;
    else if (k == "--call") a.call = next();
    else if (k == "--answer") a.answer = true;
    else if (k == "--glare-at") a.glare_at = std::atoll(next().c_str());
    else if (k == "--glare-count") a.glare_count = std::atoi(next().c_str());
    else if (k == "--glare-period") a.glare_period_ms = std::atoi(next().c_str());
    else if (k == "--no-system-audio") a.system_audio = false;
    else if (k == "--no-panel") a.panel = false;
    else if (k == "--fullscreen") a.fullscreen = true;
    else if (k == "--idle") a.idle = true;
  }
  return a;
}

// The first visible top-level window whose title contains the text.
HWND FindWindowByTitle(const std::wstring& part) {
  struct Ctx {
    std::wstring part;
    HWND found = nullptr;
  } ctx{part};
  EnumWindows(
      [](HWND h, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t title[512];
        GetWindowTextW(h, title, 512);
        if (std::wstring(title).find(c->part) != std::wstring::npos) {
          c->found = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}

// Counts the frames of the watched screen when there is no GPU presenter yet.
class CountingSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  void OnFrame(const webrtc::VideoFrame& frame) override {
    static perf::Stage& stage = perf::Get("watch.frames");
    stage.Record(0);
    stage.SetValue(int64_t(frame.width()) * 10000 + frame.height());
  }
};

HWND g_video = nullptr;
HWND g_panel_text = nullptr;
std::atomic<bool> g_quit{false};
WINDOWPLACEMENT g_place{sizeof(g_place)};

// F11 / double-click: real full screen, the whole monitor (the D8 shell only
// filled its own window — W0 finding).
void ToggleFullscreen(HWND h) {
  LONG style = GetWindowLongW(h, GWL_STYLE);
  if (style & WS_OVERLAPPEDWINDOW) {
    MONITORINFO mi{sizeof(mi)};
    if (GetWindowPlacement(h, &g_place) && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) {
      SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
  } else {
    SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(h, &g_place);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_KEYDOWN:
      if (w == VK_F11) ToggleFullscreen(h);
      if (w == VK_ESCAPE && !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) ToggleFullscreen(h);
      return 0;
    case WM_LBUTTONDBLCLK:
      ToggleFullscreen(h);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_CLOSE:
      g_quit = true;
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_SIZE:
      if (g_panel_text) MoveWindow(g_panel_text, 0, 0, LOWORD(l), HIWORD(l), TRUE);
      return 0;
    case WM_CLOSE:
      ShowWindow(h, SW_HIDE);  // the panel hides; the program keeps going
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

HWND CreateVideoWindow(HINSTANCE inst, const std::wstring& title) {
  WNDCLASSW wc{};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = VideoProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = L"HarmonyW1Video";
  RegisterClassW(&wc);
  return CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                         CW_USEDEFAULT, 1280, 760, nullptr, nullptr, inst, nullptr);
}

HWND CreatePanel(HINSTANCE inst) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = PanelProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"HarmonyW1Panel";
  RegisterClassW(&wc);
  HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Desempenho", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40,
                           40, 620, 720, nullptr, nullptr, inst, nullptr);
  g_panel_text = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 0,
                                 0, 600, 680, h, nullptr, inst, nullptr);
  HFONT mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH,
                           L"Consolas");
  SendMessageW(g_panel_text, WM_SETFONT, (WPARAM)mono, TRUE);
  RECT r;
  GetClientRect(h, &r);
  MoveWindow(g_panel_text, 0, 0, r.right, r.bottom, TRUE);
  return h;
}

}  // namespace

long long EpochMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
  return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// The direct-call proof (PLANO-WINDOWS W1): both sides renegotiating at once.
int RunDirect(net::Client* client, const Args& args) {
  direct::Call call(client);
  net::Socket socket(client, [&](const std::string& t, const json::Value& d) { call.OnSocketEvent(t, d); }, nullptr);
  socket.Start();
  std::string error;
  if (args.answer) {
    call.AcceptNext();
    logf("direct waiting for a call");
  } else if (!call.Ring(args.call, &error)) {
    logf("direct failed: %s", error.c_str());
    socket.Stop();
    return 1;
  }
  for (int i = 0; i < 600 && !call.connected(); ++i) Sleep(100);
  if (!call.connected()) {
    logf("direct never connected");
    call.Hangup();
    socket.Stop();
    return 1;
  }
  logf("direct report: %s", call.Report().c_str());
  for (int i = 0; args.glare_at && i < args.glare_count; ++i) {
    long long at = args.glare_at + (long long)i * args.glare_period_ms;
    while (EpochMs() < at) Sleep(EpochMs() + 20 < at ? 10 : 0);
    call.RestartIce();
    Sleep(3000);
    logf("direct report after restart %d: %s", i + 1, call.Report().c_str());
  }
  Sleep(args.seconds > 0 ? args.seconds * 1000 : 2000);
  logf("direct final: %s", call.Report().c_str());
  Sleep(2000);  // the other side is read while the call is still up
  call.Hangup();
  socket.Stop();
  return 0;
}

// One line per second for the bench (scripts/bancada/nativo.mjs): the meter
// and the engine's raw counters, as JSON after "#json ".
std::string BenchLine(const perf::Snapshot& snap, const json::Value& rtc, const std::string& encoder) {
  json::Value v = json::Value::Object();
  v["s"] = snap.seconds;
  v["cpu"] = snap.process_cpu_percent;
  v["ram"] = int64_t(snap.working_set_bytes);
  v["codificador"] = encoder;
  json::Value stages = json::Value::Object();
  for (const auto& r : snap.stages) {
    json::Value st = json::Value::Object();
    st["in"] = int64_t(r.in);
    st["out"] = int64_t(r.out);
    st["drop"] = int64_t(r.dropped);
    st["avg"] = r.avg_ms;
    st["max"] = r.max_ms;
    st["v"] = r.value;
    stages[r.name] = st;
  }
  v["etapas"] = stages;
  json::Value threads = json::Value::Object();
  for (const auto& t : snap.threads) threads[t.name] = t.cpu_percent;
  v["tarefas"] = threads;
  v["rtc"] = rtc;
  return "#json " + v.Dump() + "\n";
}

// A crash says where it happened, in the log, before the process goes.
LONG WINAPI OnCrash(EXCEPTION_POINTERS* e) {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)e->ExceptionRecord->ExceptionAddress, &mod);
  logf("CRASH code=0x%08lx at module+0x%llx", e->ExceptionRecord->ExceptionCode,
       (unsigned long long)((char*)e->ExceptionRecord->ExceptionAddress - (char*)mod));
  return EXCEPTION_CONTINUE_SEARCH;
}

#include "camera.h"
int wmain(int argc, wchar_t** argv) {
  if(argc==4&&std::wstring(argv[1])==L"--camera-worker")return camera::WorkerMain(_wcstoui64(argv[2],nullptr,10),net::Narrow(argv[3]));
  if(argc==2&&std::wstring(argv[1])==L"--camera-deadline-test")return camera::DeadlineSelfTest();
  SetUnhandledExceptionFilter(OnCrash);
  perf::NameThread(L"harmony-main");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Args args = Parse(argc, argv);
  char pw[256] = {};
  DWORD pwlen = GetEnvironmentVariableA("HARMONY_PASSWORD", pw, sizeof pw);
  SetEnvironmentVariableA("HARMONY_PASSWORD", nullptr);  // not inherited, not left lying around
  if (args.user.empty() || pwlen == 0 || pwlen >= sizeof pw) {
    std::fprintf(stderr, "usage: harmony-w1 --user NAME [--url U] [--room R] [--share TITLE] [--watch USER]\n"
                         "       (password in HARMONY_PASSWORD)\n");
    return 2;
  }
  // Not the user: it may be an e-mail address, and the log never has one.
  logf("harmony-w1 starting: url=%s room=%s share=%s watch=%s fps=%d", args.url.c_str(),
       args.room.c_str(), args.share.empty() ? "-" : "yes", args.watch.empty() ? "-" : args.watch.c_str(), args.fps);

  net::Client client(args.url);
  std::string error;
  std::string password(pw, pwlen);
  bool signed_in = client.Login(args.user, password, &error, [](const std::string& kind) {
    std::fprintf(stderr, kind == "email" ? "codigo enviado por e-mail: " : "codigo do app autenticador: ");
    char code[64] = {};
    if (!std::fgets(code, sizeof code, stdin)) return std::string();
    std::string s(code);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
  });
  SecureZeroMemory(password.data(), password.size());
  if (!signed_in) {
    SecureZeroMemory(pw, sizeof pw);
    logf("login failed: %s", error.c_str());
    return 1;
  }
  SecureZeroMemory(pw, sizeof pw);
  logf("signed in");

  if (!args.call.empty() || args.answer) return RunDirect(&client, args);

  HINSTANCE inst = GetModuleHandleW(nullptr);
  HWND panel = args.panel ? CreatePanel(inst) : nullptr;
  if (!args.watch.empty()) g_video = CreateVideoWindow(inst, L"harmony — " + net::Wide(args.watch));
  if (g_video && args.fullscreen) ToggleFullscreen(g_video);

  voice::Options opt;
  opt.room = args.room;
  opt.mic = args.mic;
  opt.share_fps = args.fps;
  opt.share_audio = args.system_audio;
  opt.watch = args.watch;
  if (!args.share.empty()) {
    opt.share_window = FindWindowByTitle(net::Wide(args.share));
    if (!opt.share_window) {
      logf("no visible window with '%s' in the title", args.share.c_str());
      return 1;
    }
  }
#if HARMONY_GPU
  std::unique_ptr<gpu::Presenter> presenter = g_video ? gpu::Presenter::Create(g_video) : nullptr;
  webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink = presenter.get();
#else
  CountingSink counting;
  webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink = &counting;
#endif
  // on_watch runs on the voice thread, the final removal on this one.
  std::mutex attached_mu;
  webrtc::VideoTrackInterface* attached = nullptr;
  opt.on_watch = [&](webrtc::VideoTrackInterface* track) {
    std::lock_guard<std::mutex> lock(attached_mu);
    if (attached) attached->RemoveSink(sink);
    attached = track;
    if (track && sink) track->AddOrUpdateSink(sink, webrtc::VideoSinkWants());
  };

  voice::Session session(&client, opt);
  net::Socket socket(
      &client,
      [&](const std::string& type, const json::Value& data) {
        if (!args.idle) session.OnSocketEvent(type, data);
      },
      [&] {
        if (!args.idle) session.OnSocketOpen();
      });
  socket.Start();
  if (args.idle) {
    logf("idle: signed in with the socket open, no room");
  } else if (!session.Start(&error)) {
    logf("voice failed: %s", error.c_str());
    socket.Stop();
    return 1;
  }

  // The meter's single reader: once per second, into the panel, the log file
  // the bench reads, and HARMONY_PERF_FILE.
  FILE* perf_file = nullptr;
  char perf_path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("HARMONY_PERF_FILE", perf_path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) perf_file = std::fopen(perf_path, "a");
  auto poller = webrtc::make_ref_counted<stats::Poller>();
  UINT_PTR tick = SetTimer(nullptr, 0, 1000, nullptr);
  ULONGLONG started = GetTickCount64();
  MSG msg;
  while (!g_quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == tick) {
      perf::Snapshot snap = perf::TakeSnapshot();
      poller->Request(session.pc());
      std::string text = perf::Format(snap) + poller->Text();
#if HARMONY_GPU
      text = "codificador: " + gpu::ActiveEncoderName() + "\n" + text;
#endif
      if (g_panel_text) {
        std::string crlf;
        for (char c : text) {
          if (c == '\n') crlf += '\r';
          crlf += c;
        }
        SetWindowTextW(g_panel_text, net::Wide(crlf).c_str());
      }
      if (perf_file) {
        std::string encoder;
#if HARMONY_GPU
        encoder = gpu::ActiveEncoderName();
#endif
        text += BenchLine(snap, poller->Counters(), encoder);
        std::fprintf(perf_file, "%llu\n%s", (unsigned long long)GetTickCount64(), text.c_str());
        std::fflush(perf_file);
      }
      if (args.seconds > 0 && GetTickCount64() - started >= ULONGLONG(args.seconds) * 1000) break;
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  {
    std::lock_guard<std::mutex> lock(attached_mu);
    if (attached) attached->RemoveSink(sink);
    attached = nullptr;
  }
  session.Leave();
  socket.Stop();
  if (perf_file) std::fclose(perf_file);
  if (panel) DestroyWindow(panel);
  logf("harmony-w1 done");
  return 0;
}
