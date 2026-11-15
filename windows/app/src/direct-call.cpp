#include "direct-call.h"
#include "../../native/src/log.h"
#include <algorithm>
#include <chrono>

namespace app {
namespace {
std::string Escape(const std::string& s) {
  std::string out; const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
std::string Path(const std::string& id) { return "/api/calls/" + Escape(id); }
bool Good(const net::Response& r) { return r.status >= 200 && r.status < 300; }
CallPerson Person(const json::Value& v) { return {v["username"].Str(), v["display_name"].Str()}; }
std::string Error(const net::Response& r) {
  const auto code = r.Json()["error"].Str();
  if (!r.status || code == "offline") return "Sem conexão com o servidor.";
  if (code == "busy") return "Essa pessoa já está em outra chamada.";
  if (code == "ring_limit") return "Espere um pouco antes de ligar de novo.";
  if (code == "not_found") return "Não encontrado.";
  if (code == "taken") return "Atendida em outro aparelho.";
  if (code == "unauthenticated") return "Sua sessão terminou. Entre de novo.";
  return "Deu algo errado. Tente de novo.";
}
}
DirectCall::DirectCall(Transport t, DirectMediaFactory f, std::function<void()> changed, Clock clock)
    : transport_(std::move(t)), factory_(std::move(f)), changed_(std::move(changed)), clock_(std::move(clock)) {
  if (!clock_) clock_ = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
}
DirectCall::~DirectCall() { { std::lock_guard lock(muteMu_); muteMedia_ = nullptr; } ++generation_; answered_ = false; auto media = std::move(media_); if (media) media->Close(); }
CallView DirectCall::View() const { std::lock_guard lock(mu_); return view_; }
void DirectCall::Set(CallView v) {
  { std::lock_guard lock(mu_); v.sequence = view_.sequence + 1; view_ = std::move(v); }
  if (changed_) changed_();
}
void DirectCall::Finish(std::string notice) {
  ++generation_; answered_ = false; everConnected_ = false; outbox_.clear(); cameraId_.clear();
  { std::lock_guard lock(muteMu_); muteMedia_ = nullptr; requestedMute_ = false; muteActive_ = false; ++muteEpoch_; }
  auto media = std::move(media_); // Invalidates callbacks before Close.
  if (media) media->Close();
  CallView v; v.notice = std::move(notice); Set(std::move(v));
  logf("direct: finished");
}
void DirectCall::PostEnd(const std::string& id, const std::string& reason) {
  auto body = json::Value::Object(); body["reason"] = reason;
  auto r = transport_("POST", Path(id) + "/hangup", &body);
  logf("direct: hangup HTTP %d", r.status);
}
void DirectCall::End(const std::string& reason) {
  auto id = View().id; Finish(); if (!id.empty()) PostEnd(id, reason);
}
bool DirectCall::StartMedia(const json::Value& ice) {
  if (media_) return true;
  const auto generation = generation_;
  media_ = factory_([this, generation](std::string kind, json::Value payload) {
    if (generation == generation_) MediaSignal(std::move(kind), std::move(payload));
  }, [this, generation](bool connected) { if (generation == generation_) MediaStatus(connected); });
  std::string error;
  if (!media_ || !media_->Open(View().polite, ice, &error)) {
    // Adapter errors may contain SDP or credentials; never log or display them.
    auto id = View().id; Finish("Não deu para completar a chamada.");
    if (!id.empty()) PostEnd(id, "hangup"); return false;
  }
  { std::lock_guard lock(muteMu_); muteMedia_ = media_.get(); media_->Mute(requestedMute_); }
  media_->Deafen(View().deafened); VolumesChanged();
  for (int slot = 1; slot <= 4; ++slot) if (videos_[slot]) media_->Video(slot, videos_[slot]);
  logf("direct: media opened"); return true;
}
void DirectCall::Ring(const CallPerson& who) {
  if (View().phase != CallPhase::Idle || who.username.empty()) return;
  { std::lock_guard lock(muteMu_); muteActive_ = true; }
  CallView v; v.phase = CallPhase::Calling; v.peer = who; v.ringingSince = clock_(); Set(v);
  auto body = json::Value::Object(); body["username"] = who.username;
  auto r = transport_("POST", "/api/calls", &body); logf("direct: start HTTP %d", r.status);
  if (!Good(r)) { Finish(Error(r)); return; }
  auto data = r.Json(); v = View(); v.id = data["id"].Str(); v.polite = data["polite"].Bool();
  if (v.id.empty()) { Finish("Deu algo errado. Tente de novo."); return; }
  Set(v); StartMedia(data["ice_servers"]);
}
void DirectCall::Accept() {
  auto v = View(); if (v.phase != CallPhase::Ringing) return;
  { std::lock_guard lock(muteMu_); muteActive_ = true; }
  v.phase = CallPhase::Connecting; Set(v); auto body = json::Value::Object();
  auto r = transport_("POST", Path(v.id) + "/accept", &body); logf("direct: accept HTTP %d", r.status);
  if (!Good(r)) { Finish(Error(r)); return; }
  auto data = r.Json(); answered_ = true; v.polite = data["polite"].Bool(true); v.peer = Person(data["from"]); Set(v);
  StartMedia(data["ice_servers"]);
}
void DirectCall::Decline() { if (View().phase == CallPhase::Ringing) End("rejected"); }
void DirectCall::Hangup() { if (View().phase != CallPhase::Idle) End("hangup"); }
void DirectCall::Disconnect() { Hangup(); }
void DirectCall::Signal(const std::string& kind, const json::Value& payload) {
  auto body = json::Value::Object(); body["kind"] = kind; body["payload"] = payload;
  auto r = transport_("POST", Path(View().id) + "/signal", &body);
  logf("direct: signal %s HTTP %d", kind.c_str(), r.status);
}
void DirectCall::MediaSignal(std::string kind, json::Value payload) {
  if (kind == "capture") { CaptureEnded(payload); return; }
  if (kind != "offer" && kind != "answer" && kind != "ice") return;
  auto v = View(); if (v.id.empty() || v.phase == CallPhase::Idle || v.phase == CallPhase::Ringing) return;
  if (!answered_) { outbox_.emplace_back(std::move(kind), std::move(payload)); logf("direct: signal queued"); }
  else Signal(kind, payload);
}
void DirectCall::MediaStatus(bool connected) {
  auto v = View(); if (!answered_ || !media_ || v.phase == CallPhase::Idle) return;
  if (v.mediaConnected == connected) return;
  v.mediaConnected = connected; v.phase = connected ? CallPhase::Live : CallPhase::Connecting;
  if (connected && !everConnected_) { v.liveSince = clock_(); everConnected_ = true; }
  v.notice = connected ? "" : "Reconectando…"; Set(v); logf("direct: media connected=%d", int(connected));
}
// What this side sends, told to the other (§7 call_state).
void DirectCall::PostState(const CallView& v) {
  auto body = json::Value::Object(); body["mic"] = !v.muted; body["cam"] = v.cam; body["screen"] = v.screen;
  auto r = transport_("POST", Path(v.id) + "/state", &body); logf("direct: state HTTP %d", r.status);
}
void DirectCall::Camera(bool on, const std::string& id) {
  auto v = View(); if (v.phase == CallPhase::Idle || v.phase == CallPhase::Ringing) return;
  if (v.cam == on && (!on || cameraId_ == id)) return;
  bool changed = v.cam != on;
  cameraId_ = on ? id : std::string();
  v.cam = on; if (changed) Set(v); if (media_) media_->Camera(on, id);
  if (answered_ && changed) PostState(v);
}
// The camera would not open or the shared window closed: what this side
// says it sends follows what it does.
void DirectCall::CaptureEnded(const json::Value& payload) {
  auto v = View(); if (v.phase == CallPhase::Idle) return;
  bool changed = false;
  if (payload.Has("camera") && !payload["camera"].Bool() && v.cam) { v.cam = false; changed = true; }
  if (payload.Has("screen") && !payload["screen"].Bool() && v.screen) { v.screen = false; changed = true; }
  if (!changed) return;
  Set(v); logf("direct: capture ended");
  if (answered_) PostState(v);
}
// A picker opened for one call must not share into the next (the id).
void DirectCall::Screen(const std::string& id, void* window, void* monitor, int height, int fps, bool sound) {
  auto v = View(); if (v.phase == CallPhase::Idle || v.phase == CallPhase::Ringing || v.id != id) return;
  bool was = v.screen; v.screen = true; Set(v); if (media_) media_->Screen(window, monitor, height, fps, sound);
  if (answered_ && !was) PostState(v);
}
void DirectCall::StopScreen() {
  auto v = View(); if (!v.screen) return;
  v.screen = false; Set(v); if (media_) media_->StopScreen();
  if (answered_) PostState(v);
}
// The tiles ask for the other side's pictures; a call that starts later gets
// them when its media is built (StartMedia).
void DirectCall::Video(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach) {
  if (slot < 1 || slot > 4) return;
  videos_[slot] = attach;
  if (media_) media_->Video(slot, std::move(attach));
}
void DirectCall::VolumesChanged() {
  if (!media_ || !volumes) return;
  auto [voice, screen] = volumes(View().peer.username); media_->Volumes(voice, screen);
}
void DirectCall::Device(bool capture, const std::string& id) { if (media_) media_->Device(capture, id); }
void DirectCall::Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double micGain) {
  if (media_) media_->Filters(dfn, strength, echo, noise, agc, micGain);
}
void DirectCall::Sidetone(bool on) { if (media_) media_->Sidetone(on); }
// "Surdo", as in a room: the other side's sound stops here and the
// microphone goes off with it; hearing again gives the microphone back the
// way it was. The other side sees a muted microphone (§7 has no deafened).
void DirectCall::Deafen(bool deafened) {
  auto v = View(); if (v.phase == CallPhase::Idle || v.phase == CallPhase::Ringing || v.deafened == deafened) return;
  if (deafened) mutedBeforeDeaf_ = v.muted;
  v.deafened = deafened; v.muted = deafened ? true : mutedBeforeDeaf_; Set(v);
  { std::lock_guard lock(muteMu_); requestedMute_ = v.muted; if (muteMedia_) muteMedia_->Mute(v.muted); }
  if (media_) media_->Deafen(deafened);
  logf("direct: deafened=%d", int(deafened));
  if (answered_) PostState(v);
}
uint64_t DirectCall::MuteImmediately(bool muted) {
  // Only the short publication lock crosses threads; model/HTTP stay on the worker.
  std::lock_guard lock(muteMu_);
  auto v = View();
  if (!muteActive_ || v.phase == CallPhase::Idle || v.phase == CallPhase::Ringing || v.deafened) return 0;
  requestedMute_ = muted;
  if (muteMedia_) muteMedia_->Mute(muted);
  return muteEpoch_;
}
void DirectCall::CommitMute(uint64_t epoch) {
  auto v = View();
  if (v.phase == CallPhase::Idle || v.deafened) return;
  { std::lock_guard lock(muteMu_); if (!epoch || epoch != muteEpoch_) return; v.muted = requestedMute_; }
  if (v.muted == View().muted) return;
  Set(v);
  if (answered_) PostState(v);
}
void DirectCall::Mute(bool muted) { CommitMute(MuteImmediately(muted)); }
bool DirectCall::Event(const std::string& type, const json::Value& data) {
  if (type != "call" && type != "call_accepted" && type != "call_taken" && type != "call_ended" && type != "call_signal" && type != "call_state") return false;
  auto v = View(); const auto id = data["id"].Str(); if (id.empty()) return true;
  logf("direct: event %s", type.c_str());
  if (type == "call") {
    if (id == v.id) return true;
    if (v.phase != CallPhase::Idle) { PostEnd(id, "hangup"); return true; }
    v = {}; v.id = id; v.peer = Person(data["from"]); v.phase = CallPhase::Ringing; v.ringingSince = clock_(); Set(v); return true;
  }
  if (id != v.id || v.phase == CallPhase::Idle) return true;
  if (type == "call_accepted" && v.phase == CallPhase::Calling) {
    answered_ = true; v.phase = CallPhase::Connecting; v.peer = Person(data["peer"]); Set(v);
    auto pending = std::move(outbox_); outbox_.clear();
    // Defensive ordering if the adapter delivered an ICE callback first.
    std::stable_partition(pending.begin(), pending.end(), [](const auto& p) { return p.first != "ice"; });
    for (const auto& [kind, payload] : pending) Signal(kind, payload);
    if (v.muted || v.cam || v.screen) PostState(v);
  } else if (type == "call_taken" && v.phase == CallPhase::Ringing) {
    // Own accept moved to Connecting before HTTP; its queued taken is ignored.
    Finish();
  } else if (type == "call_ended") {
    Finish(data["reason"].Str() == "rejected" ? "Chamada recusada." : "");
  } else if (type == "call_state" && answered_) {
    v.peerMuted = !data["mic"].Bool(); v.peerCam = data["cam"].Bool(); v.peerScreen = data["screen"].Bool(); Set(v);
  } else if (type == "call_signal" && answered_ && media_) {
    const auto kind = data["kind"].Str(); if (kind == "offer" || kind == "answer" || kind == "ice") media_->Remote(kind, data["payload"]);
  }
  return true;
}
void DirectCall::Open(const std::string& id) {
  if (id.empty() || View().phase != CallPhase::Idle) return;
  auto r = transport_("GET", Path(id), nullptr); logf("direct: open HTTP %d", r.status);
  if (r.status == 404) return;
  if (!Good(r)) { Finish(Error(r)); return; }
  auto data = r.Json(); if (data["answered"].Bool() || !data["polite"].Bool()) return;
  data["id"] = id; Event("call", data);
}
void DirectCall::Reconcile() {
  // The gateway was down: an accept or an end published meanwhile never arrived.
  // Ask once. Only a call that never reached media may be ended by "not found":
  // a server restart forgets calls, and an established pair is alive on its own.
  auto v = View(); if (v.phase == CallPhase::Idle || v.id.empty()) return;
  const auto id = v.id;
  auto r = transport_("GET", Path(id), nullptr); logf("direct: reconcile HTTP %d", r.status);
  v = View(); if (v.id != id || v.phase == CallPhase::Idle) return;
  if (r.status == 404) { if (v.phase != CallPhase::Live) Finish(); return; }
  if (!Good(r)) return;
  auto data = r.Json(); if (!data["answered"].Bool()) return;
  if (v.phase == CallPhase::Calling) {
    auto accepted = json::Value::Object(); accepted["id"] = id; accepted["peer"] = data["to"];
    Event("call_accepted", accepted);
  } else if (v.phase == CallPhase::Ringing) Finish(); // answered on another device
}
std::optional<int64_t> DirectCall::NextTickDelay() const {
  auto v=View();
  if(v.phase!=CallPhase::Calling && v.phase!=CallPhase::Ringing) return {};
  return std::max<int64_t>(0,45000-(clock_()-v.ringingSince));
}
void DirectCall::Tick() {
  auto v = View(); if (v.phase != CallPhase::Calling && v.phase != CallPhase::Ringing) return;
  if (clock_() - v.ringingSince < 45000) return;
  logf("direct: ring expired"); if (v.phase == CallPhase::Calling) End("missed"); else Finish();
}
}
