#include "../src/direct-call.h"
#include "../src/notices.h"
#include "../../native/src/log.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
int checks = 0;
void Check(bool ok, const char* name) { ++checks; if (!ok) throw std::runtime_error(name); }
json::Value J(const char* text) { return *json::Parse(text); }
struct Media : app::DirectMedia {
  std::vector<std::pair<bool, std::string>>* cameras;
  void Camera(bool on, const std::string& id) override { cameras->emplace_back(on, id); }
  int* closes; std::vector<std::string>* remote; bool muted = false;
  Media(int* c, std::vector<std::string>* r) : closes(c), remote(r) {}
  bool Open(bool, const json::Value&, std::string*) override { return true; }
  void Remote(const std::string& kind, const json::Value&) override { remote->push_back(kind); }
  void Mute(bool v) override { muted = v; }
  void Close() override { ++*closes; }
};
struct Fake {
  struct Request { std::string method, path; json::Value body; };
  int64_t now = 0; int closes = 0, changes = 0, opens = 0;
  std::vector<std::pair<bool, std::string>> cameras;
  std::vector<Request> requests; std::vector<std::string> remote;
  std::function<void(std::string, json::Value)> send, oldSend;
  std::function<void(bool)> status;
  std::function<net::Response(const std::string&, const std::string&)> intercept;
  std::unique_ptr<app::DirectCall> call;
  Fake() {
    call = std::make_unique<app::DirectCall>([this](const auto& method, const auto& path, const auto* body) {
      // Reentrant View on another thread proves HTTP never holds the view lock.
      std::thread reader([&] { (void)call->View(); }); reader.join();
      requests.push_back({method, path, body ? *body : json::Value{}});
      if (intercept) return intercept(method, path);
      return net::Response{200, R"({"id":"one","polite":false,"from":{"username":"bob","display_name":"Bob"},"ice_servers":[]})"};
    }, [this](auto s, auto t) {
      oldSend = send; send = s; status = t; ++opens;
      auto media = std::make_unique<Media>(&closes, &remote); media->cameras = &cameras; return media;
    }, [this] { ++changes; (void)call->View(); }, [this] { return now; });
  }
  ~Fake() { call.reset(); }
  void Ring() { call->Ring({"bob", "Bob"}); }
  void Incoming() { call->Event("call", J(R"({"id":"one","from":{"username":"bob","display_name":"Bob"}})")); }
  void Accepted() { call->Event("call_accepted", J(R"({"id":"one","peer":{"username":"bob","display_name":"Bob"}})")); }
  size_t Count(const std::string& suffix) const { size_t n = 0; for (const auto& r : requests) if (r.path.ends_with(suffix)) ++n; return n; }
};
void Reconciles() {
  const char* answered = R"({"id":"one","polite":false,"answered":true,"from":{"username":"me","display_name":"Me"},"to":{"username":"bob","display_name":"Bob"},"ice_servers":[]})";
  const char* waiting = R"({"id":"one","polite":false,"answered":false,"from":{"username":"me","display_name":"Me"},"to":{"username":"bob","display_name":"Bob"},"ice_servers":[]})";
  { Fake f; f.Ring(); f.send("offer", J(R"({"type":"offer","sdp":"sdp-private-w6"})"));
    f.intercept = [&](const std::string& method, const std::string&) { return net::Response{200, method == "GET" ? waiting : "{}"}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Calling && f.Count("/signal") == 0, "reconcile leaves an unanswered call ringing");
    f.intercept = [&](const std::string& method, const std::string&) { return net::Response{200, method == "GET" ? answered : "{}"}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Connecting && f.call->View().peer.username == "bob" && f.Count("/signal") == 1, "an accept missed while the gateway was down is recovered and the offer goes out");
    int closes = f.closes; f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Connecting && f.closes == closes && f.Count("/signal") == 1, "reconciling an accepted call again changes nothing"); }
  { Fake f; f.Ring();
    f.intercept = [](const std::string&, const std::string&) { return net::Response{404, "{}"}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Idle, "a call that ended while the gateway was down is cleared"); }
  { Fake f; f.Ring();
    f.intercept = [](const std::string&, const std::string&) { return net::Response{0, ""}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Calling, "a failed lookup never ends a call"); }
  { Fake f; f.Incoming();
    f.intercept = [&](const std::string& method, const std::string&) { return net::Response{200, method == "GET" ? answered : "{}"}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Idle, "a ring answered on another device stops ringing"); }
  { Fake f; f.Ring(); f.Accepted(); f.status(true);
    Check(f.call->View().phase == app::CallPhase::Live, "fixture reaches a live call");
    int closes = f.closes;
    f.intercept = [](const std::string&, const std::string&) { return net::Response{404, "{}"}; };
    f.call->Reconcile();
    Check(f.call->View().phase == app::CallPhase::Live && f.closes == closes, "a live call survives a server that forgot it"); }
}
void Calls() {
  Fake f; f.Ring();
  Check(f.call->View().phase == app::CallPhase::Calling, "outgoing rings");
  f.send("ice", J(R"({"candidate":"candidate-private-w6"})"));
  f.send("offer", J(R"({"type":"offer","sdp":"sdp-private-w6"})"));
  f.send("ice", J(R"({"candidate":"second-private-w6"})"));
  Check(f.Count("/signal") == 0, "no signaling before accept");
  f.Accepted();
  Check(f.requests.size() == 4 && f.requests[1].body["kind"].Str() == "offer" && f.requests[2].body["kind"].Str() == "ice" && f.requests[3].body["payload"]["candidate"].Str() == "second-private-w6", "offer then candidates in order");
  f.send("ice", J("{}")); Check(f.Count("/signal") == 4, "accepted signals go immediately");
  f.call->Mute(true); Check(f.requests.back().body.Dump() == J(R"({"mic":false,"cam":false,"screen":false})").Dump(), "mute sends exact state");
  f.call->Event("call_state", J(R"({"id":"one","mic":false,"cam":true,"screen":true})"));
  auto v = f.call->View(); Check(v.peerMuted && v.peerCam && v.peerScreen, "peer state updates");
  f.call->Event("call_state", J(R"({"id":"other","mic":true})")); Check(f.call->View().peerMuted, "foreign state ignored");
  f.status(true); Check(f.call->View().phase == app::CallPhase::Live && f.call->View().mediaConnected, "media establishes live");
  f.now = 100; f.status(false); Check(!f.call->View().mediaConnected && f.call->View().notice == "Reconectando…", "media loss preserves call");
  f.status(true); Check(f.call->View().liveSince == 0, "reconnection preserves zero-based first-live clock");
  f.call->Event("call_signal", J(R"({"id":"one","kind":"answer","payload":{"sdp":"remote-private-w6"}})"));
  Check(f.remote == std::vector<std::string>{"answer"}, "remote media receives answer");
  f.call->Hangup(); Check(f.closes == 1 && f.call->View().phase == app::CallPhase::Idle, "hangup closes once");
  auto seq = f.call->View().sequence; f.status(true); Check(f.call->View().sequence == seq, "closed media callback ignored");
  auto stale = f.send; f.Ring(); auto n = f.requests.size(); stale("offer", J("{}")); Check(n == f.requests.size(), "old generation cannot signal new call");
  Check(f.call->View().sequence >= uint64_t(f.changes), "sequence changes monotonically");
  for (const auto& line : LogLines()) Check(line.find("sdp-private-w6") == std::string::npos && line.find("candidate-private-w6") == std::string::npos && line.find("remote-private-w6") == std::string::npos, "logs contain no media payload");
}
void RacesAndTimeouts() {
  Fake f; f.Ring(); f.call->Event("call", J(R"({"id":"crossed","from":{"username":"alice"}})"));
  Check(f.requests.back().path == "/api/calls/crossed/hangup" && f.requests.back().body["reason"].Str() == "hangup" && f.call->View().id == "one", "crossed call alone is hung up");
  Check(f.call->NextTickDelay()==45000,"ring deadline is 45 seconds");
  f.now = 44999; Check(f.call->NextTickDelay()==1,"ring deadline retains last millisecond"); f.call->Tick(); Check(f.call->View().phase == app::CallPhase::Calling, "before deadline still rings");
  f.now = 45000; f.call->Tick(); Check(f.requests.back().body["reason"].Str() == "missed" && f.call->View().phase == app::CallPhase::Idle, "caller expires at 45s");
  Check(!f.call->NextTickDelay(),"idle call needs no wakeup");
  auto n = f.requests.size(); f.call->Tick(); Check(n == f.requests.size(), "expiration runs once");
  f.Incoming(); n = f.requests.size(); f.now += 45000; f.call->Tick(); Check(f.requests.size() == n && f.call->View().phase == app::CallPhase::Idle, "callee expires without HTTP");
  f.Incoming(); f.call->Event("call_taken", J(R"({"id":"one","by":"another-device"})"));
  Check(f.call->View().phase == app::CallPhase::Idle && f.call->View().notice.empty(), "other device taken is silent");
  f.Incoming(); f.call->Accept(); f.call->Event("call_taken", J(R"({"id":"one","by":"own-device"})"));
  Check(f.call->View().phase == app::CallPhase::Connecting, "own taken after accept ignored");
  f.call->Hangup(); f.Incoming(); f.call->Decline(); Check(f.requests.back().body["reason"].Str() == "rejected", "decline uses rejected");
  for (const char* reason : {"hangup", "rejected", "missed", "disconnected"}) {
    f.Ring(); auto e = J(R"({"id":"one"})"); e["reason"] = reason; int closes = f.closes;
    f.call->Event("call_ended", e);
    Check(f.closes == closes + 1 && f.call->View().notice == (std::string(reason) == "rejected" ? "Chamada recusada." : ""), "end reasons follow web and close media");
  }
  Check(!f.call->Event("message", J("{}")), "unrelated events unhandled");
  f.Ring(); f.call->Disconnect(); Check(f.call->View().phase == app::CallPhase::Idle, "disconnect ends locally");
}
void ErrorsAndOpen() {
  for (auto [code, text] : std::vector<std::pair<std::string, std::string>>{{"busy","Essa pessoa já está em outra chamada."},{"ring_limit","Espere um pouco antes de ligar de novo."},{"not_found","Não encontrado."},{"offline","Sem conexão com o servidor."}}) {
    Fake f; f.intercept = [code](const auto&, const auto&) { auto b = J("{}"); b["error"] = code; return net::Response{code == "offline" ? 0 : 409, b.Dump()}; };
    f.Ring(); Check(f.call->View().phase == app::CallPhase::Idle && f.call->View().notice == text, "start error uses exact web phrase");
  }
  Fake f; f.intercept = [](const auto&, const auto&) { return net::Response{200, R"({"answered":true,"polite":true})"}; };
  f.call->Open("a/b ?#ç"); Check(f.requests.back().path == "/api/calls/a%2Fb%20%3F%23%C3%A7" && f.call->View().phase == app::CallPhase::Idle, "escaped GET and answered does not ring");
  f.intercept = [](const auto&, const auto&) { return net::Response{404,""}; }; f.call->Open("one"); Check(f.call->View().phase == app::CallPhase::Idle, "404 does not ring");
  f.intercept = [](const auto&, const auto&) { return net::Response{200,R"({"answered":false,"polite":true,"from":{"username":"bob"}})"}; };
  f.call->Open("a/b"); Check(f.call->View().phase == app::CallPhase::Ringing, "unanswered GET rings");
  f.call->Decline(); Check(f.requests.back().path == "/api/calls/a%2Fb/hangup", "POST escapes id too");
}
void ImmediateMute() {
  Fake f; f.Ring();
  auto epoch = f.call->MuteImmediately(true);
  Check(!f.call->View().muted, "immediate media gate leaves model publication on worker");
  f.call->MuteImmediately(false); f.call->MuteImmediately(true);
  f.call->CommitMute(epoch);
  Check(f.call->View().muted, "queued mute publishes latest requested value");
  f.call->Hangup(); f.Ring(); f.call->CommitMute(epoch);
  Check(!f.call->View().muted, "stale mute commit cannot affect a new call");
}
void CameraSwitch() {
  Fake f;
  f.call->Camera(true, "idle"); f.Incoming(); f.call->Camera(true, "ringing");
  Check(f.cameras.empty(), "idle and ringing cannot open a camera");
  f.call->Accept(); f.call->Camera(true, "first");
  auto states = f.Count("/state"); auto sequence = f.call->View().sequence;
  f.call->Camera(true, "first");
  Check(f.cameras.size() == 1, "same enabled camera is redundant");
  f.call->Camera(true, "second");
  Check(f.cameras.size() == 2 && f.cameras.back() == std::pair<bool,std::string>{true,"second"}, "enabled camera changes source");
  Check(f.opens == 1 && f.closes == 0 && f.call->View().cam, "source replacement preserves media connection");
  Check(f.Count("/state") == states && f.call->View().sequence == sequence, "source replacement leaves camera state unchanged");
  f.call->Camera(false, "second"); f.call->Camera(false, "ignored");
  Check(f.cameras.size() == 3 && !f.cameras.back().first, "camera stops only once");
  f.call->Camera(true, "second");
  Check(f.cameras.size() == 4 && f.call->View().cam, "same source can be restarted");
  f.call->Hangup(); f.Ring(); f.call->Camera(true, "second");
  Check(f.cameras.size() == 5 && f.opens == 2, "new call starts its camera independently");
}
void CaptureFollow() {
  Fake f;f.Ring();f.Accepted();
  auto id=f.call->View().id;
  f.call->Screen(id,reinterpret_cast<void*>(1),nullptr,1080,30,true);
  auto states=f.Count("/state");
  f.call->MediaSignal("capture",J(R"({"screen":true})"));
  Check(f.call->View().screen&&f.Count("/state")==states,"follow keeps direct screen live without a state post");
  f.call->MediaSignal("capture",J(R"({"screen":false})"));
  Check(!f.call->View().screen&&f.Count("/state")==states+1,"ended capture still clears direct screen");
}
void MediaFailure() {
  app::DirectCall call([](const auto&, const auto&, const auto*) { return net::Response{201,R"({"id":"broken","ice_servers":[{"credential":"turn-private-w6"}]})"}; },
      [](auto, auto) -> std::unique_ptr<app::DirectMedia> { return {}; });
  call.Ring({"bob","Bob"});
  Check(call.View().phase == app::CallPhase::Idle && call.View().notice == "Não deu para completar a chamada.", "missing media fails safely");
  for (const auto& line : LogLines()) Check(line.find("turn-private-w6") == std::string::npos, "TURN credentials never logged");
  Fake f; f.Ring(); f.call->Mute(true); Check(f.Count("/state") == 0, "pre-answer mute does not send forbidden state");
  f.Accepted(); Check(f.requests.back().path.ends_with("/state") && !f.requests.back().body["mic"].Bool(true), "pre-answer mute sent after accept");
}
void Notices() {
  app::NoticeContext c{"alice"}; app::ChatMessage m; m.channel = "text"; m.author = {"bob","Bob","","Apelido"};
  m.body = "PRIVATE_ALPHA PRIVATE_BETA @todos PRIVATE_GAMMA";
  app::ChatConversation ch; ch.id = "text"; ch.name = "geral"; ch.kind = "text";
  Check(!app::MessageNotice(c,m,&ch), "ordinary channel message does not notify");
  m.mentions = {"alice", "bob"};
  for (const char* kind : {"text", "dm"}) {
    ch.kind = kind; auto notice = app::MessageNotice(c,m,&ch); Check(bool(notice), "mention and DM notify");
    Check(notice->tag == "text" && notice->id == "text", "conversation tag replaces previous notice");
    for (const char* word : {"PRIVATE_ALPHA","PRIVATE_BETA","@todos","PRIVATE_GAMMA"}) Check((notice->title + notice->body).find(word) == std::string::npos, "message body never appears including @todos");
    Check(notice->body == (ch.kind == "dm" ? "Mensagem direta" : "Te mencionou"), "DM wins mention");
    Check(notice->title.starts_with("Apelido"), "nickname takes precedence");
  }
  m.kind = "pin"; Check(!app::MessageNotice(c,m,&ch) && !app::MessageSound(c,m,&ch,""), "a pin line neither notifies nor sounds, as on the web"); m.kind.clear();
  ch.muted = true; Check(!app::MessageNotice(c,m,&ch), "muted conversation suppressed"); ch.muted = false;
  c.enabled = false; Check(!app::MessageNotice(c,m,&ch) && !app::CallNotice(c,"c",{"bob","Bob"}), "disabled switch suppresses"); c.enabled = true;
  c.seen = true; Check(!app::MessageNotice(c,m,&ch) && !app::CallNotice(c,"c",{"bob","Bob"}), "foreground suppresses"); c.seen = false;
  m.author.username = "alice"; Check(!app::MessageNotice(c,m,&ch), "own message suppressed"); m.author.username = "bob";
  m.author.nickname.clear(); ch.peer.username = "bob"; ch.peer.nickname = "Beto";
  Check(app::MessageNotice(c,m,&ch)->title == "Beto", "conversation peer supplies private nickname absent from event"); ch.peer.nickname.clear();
  Check(app::MessageNotice(c,m,&ch)->title == "Bob", "display name fallback");
  m.author.displayName.clear(); Check(app::MessageNotice(c,m,&ch)->title == "bob", "username fallback matches nameOf without added at");
  auto call = app::CallNotice(c,"c",{"bob","Beto"}); Check(call && call->target == app::Notice::Target::Call && call->tag == "call-c" && call->title == "Beto está te ligando", "direct call notice");
  auto ring = J(R"({"channel_id":"room","channel_name":"Jogos","from":{"username":"bob","display_name":"Bob","nickname":"Beto"}})");
  auto room = app::RoomNotice(c,ring); Check(room && room->target == app::Notice::Target::Room && room->body == "para a sala Jogos" && room->tag == "ring-room" && room->title == "Beto está te chamando", "room notice");
  c.enabled = false; Check(!app::RoomNotice(c,ring), "room respects switch"); c.enabled = true; c.seen = true; Check(!app::RoomNotice(c,ring), "room respects seen");
  c = {"alice",true,true}; m.mentions.clear(); m.channel="text"; m.author.username="bob"; ch.muted=false;
  Check(!app::MessageSound(c,m,&ch,"text"), "visible focused active conversation stays quiet");
  Check(app::MessageSound(c,m,&ch,"other"), "other conversation sounds even in focused window");
  m.mentions={"alice"}; Check(app::MessageSound(c,m,&ch,"text"), "mention sounds even in focused active conversation");
  ch.muted=true; Check(!app::MessageSound(c,m,&ch,"other"), "muted conversation silences mentions too"); ch.muted=false;
  m.author.username="alice"; Check(!app::MessageSound(c,m,&ch,"other"), "own message never sounds"); m.author.username="bob";
  c.enabled=false; Check(!app::MessageSound(c,m,&ch,"other"), "sound preference silences independently"); c.enabled=true;
  c.seen=false; m.mentions.clear(); Check(app::MessageSound(c,m,&ch,"text"), "unfocused active conversation sounds");
  auto path = std::filesystem::path("test-output/w6-preferences.json"); app::NoticePreferences p; p.enabled = false; p.sound = false;
  Check(p.Save(path), "preferences save"); app::NoticePreferences loaded; Check(loaded.Load(path) && !loaded.enabled && !loaded.sound, "preferences persist switch");
  { std::ofstream out(path); out << "{\"enabled\":\"false\"}"; } Check(!loaded.Load(path) && !loaded.enabled && !loaded.sound, "corrupt preferences retain current value");
  { std::ofstream out(path); out << "{\"enabled\":false}"; }
  Check(loaded.Load(path) && !loaded.enabled && loaded.sound, "old preference files default sound on without changing notices");
  Check(!p.Save("test-output/nonexistent-w6-directory/preferences.json"), "save reports inaccessible path");
}
}
int main() {
  try { Calls(); Reconciles(); CameraSwitch(); ImmediateMute(); CaptureFollow(); RacesAndTimeouts(); ErrorsAndOpen(); MediaFailure(); Notices(); std::cout << "W6: " << checks << " checks passed\n"; return 0; }
  catch (const std::exception& e) { std::cerr << "W6 FAILED: " << e.what() << '\n'; return 1; }
}
