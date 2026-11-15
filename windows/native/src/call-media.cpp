#include "call-media.h"

#include <algorithm>
#include <cmath>
#include <future>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "camera.h"
#include "loopback.h"
#include "media-policy.h"
#include "log.h"
#include "perf.h"
#if HARMONY_GPU
#include "../gpu/gpu.h"
#endif

namespace callmedia {
namespace {

class LocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("call set local: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

class RemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("call set remote: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

// The parameterless setLocalDescription: an offer or an answer, whichever the
// state calls for, the call perfect negotiation is written with.
bool SetLocalImplicit(webrtc::PeerConnectionInterface* pc) {
  auto obs = webrtc::make_ref_counted<LocalObserver>();
  auto f = obs->promise.get_future();
  pc->SetLocalDescription(obs);
  return f.get();
}

}  // namespace

Connection::Connection(Options options) : options_(std::move(options)) {
  thread_ = std::thread([this] { Loop(); });
}

Connection::~Connection() {
  Close();
  {
    std::lock_guard<std::mutex> lock(qmu_);
    stop_ = true;
  }
  qcv_.notify_all();
  if (thread_.joinable()) thread_.join();
  tone_stop_ = true;
  if (tone_.joinable()) tone_.join();
  mic_ = nullptr;
  factory_ = nullptr;
  // The device is let go on the worker thread it was made on: released
  // elsewhere it crashed in its destructor about one run in three (W1, 06/10).
  if (worker_) worker_->BlockingCall([this] { audio_ = {}; });
}

void Connection::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(qmu_);
    if (stop_) return;
    queue_.push_back(std::move(task));
  }
  qcv_.notify_all();
}

// One signalling step at a time (direct.ts serialise).
void Connection::Loop() {
  perf::NameThread(L"harmony-call");
  std::unique_lock<std::mutex> lock(qmu_);
  while (true) {
    if (queue_.empty()) {
      if (stop_) return;
      qcv_.wait_for(lock, std::chrono::seconds(1));
      if (queue_.empty()) {
        if (!stop_) { lock.unlock(); Tick(); lock.lock(); }
        continue;
      }
    }
    auto task = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    task();
    lock.lock();
  }
}

// Once a second on the call's thread: what reaches the speaker (the round
// reads it from the log), and a lost path tried again every 3 s.
void Connection::Tick() {
  if (!pc_ || closed_) return;
  ++ticks_;
  if(pending_screen_source_) {
    if(pending_screen_source_->state()==webrtc::MediaSourceInterface::kLive)CommitScreen();
    else if(pending_screen_source_->state()==webrtc::MediaSourceInterface::kEnded||GetTickCount64()>=pending_screen_deadline_) {
      pending_screen_source_=nullptr;
      logf("call screen replacement unavailable; retained previous picture");
      if(!screen_)Ended("screen");
    }
  }
#if HARMONY_GPU
  if(screen_&&screen_window_) {
    auto followed=gpu::CaptureWindow(screen_capture_);
    if(followed&&followed!=screen_window_) {
      screen_window_=followed;
      if(screen_sound_) {
        Send(3,nullptr);screen_audio_=nullptr;
        DWORD pid=0;GetWindowThreadProcessId(followed,&pid);
        auto loop=pid?loopback::SystemAudioSource::Create(pid,false):nullptr;
        if(loop)screen_audio_=factory_->CreateAudioTrack("screen_audio",loop.get());
        Send(3,screen_audio_.get());
      }
      // This is local capture bookkeeping, never a signalling offer or a
      // capture-ended event to the direct-call model.
      auto payload=json::Value::Object();payload["screen"]=true;Emit("capture",payload);
    }
  }
#endif
  if(networkStats_)pc_->GetStats(networkStats_.get());
  if (camera_ && camera_->GetSource()->state() == webrtc::MediaSourceInterface::kEnded) {
    logf("call camera ended"); camera_ = nullptr; Send(1, nullptr); Ended("camera"); AttachVideos();
  }
  if (screen_ && screen_->GetSource()->state() == webrtc::MediaSourceInterface::kEnded) {
    // The window's sound goes with its picture.
    logf("call screen ended"); screen_ = nullptr; screen_audio_ = nullptr; Send(2, nullptr); Send(3, nullptr); Ended("screen"); AttachVideos();
  }
  if (connected_ && audio_.renderLevel) {
    bool fresh = GetTickCount64() - audio_.renderAt->load() < 1000;
    double peak = audio_.renderPeak ? audio_.renderPeak->exchange(0) : audio_.renderLevel->load();
    logf("call playout level %.3f %s", fresh ? peak : 0.0, fresh ? "active" : "idle");
    int camera = frames_[1].count.exchange(0), screen = frames_[2].count.exchange(0);
    if (camera || screen) logf("call video in camera %d screen %d fps", camera, screen);
  }
  if (restarts_ > 0 && !connected_ && ticks_ % 3 == 0) {
    if (pc_->signaling_state() == webrtc::PeerConnectionInterface::kHaveLocalOffer) {
      // The restart offer, or its answer, was lost while the line was down
      // (the socket can be gone): send it again rather than ask for another,
      // which libwebrtc only makes from a stable state (direct.ts retrySignaling).
      logf("call ICE still not connected: offer sent again");
      Emit("offer", LocalJson());
    } else {
      logf("call ICE still not connected: restart again");
      pc_->RestartIce();
    }
  }
}

// Which codec each picture slot takes, on both sides of the call: the side
// that answers sets it on the slots the offer brought, before its answer.
// The screen prefers H.264, then VP9 and VP8 (the owner's decision); the
// camera goes VP8 or VP9, because the GPU's H.264 encoder takes only
// textures and a camera delivers I420 (it would send nothing). Never AV1.
void Connection::Prefer() {
  auto trs = pc_->GetTransceivers();
  for (size_t slot = 1; slot <= 2 && slot < trs.size(); ++slot) {
    if (trs[slot]->media_type() != webrtc::MediaType::VIDEO) continue;
    auto caps = factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    auto rank = [slot](const webrtc::RtpCodecCapability& c) {
      if (slot == 2) return c.name == "H264" ? 0 : c.name == "VP9" ? 1 : c.name == "VP8" ? 2 : 3;
      return c.name == "VP8" ? 0 : c.name == "VP9" ? 1 : 3;
    };
    std::stable_sort(caps.codecs.begin(), caps.codecs.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    caps.codecs.erase(std::remove_if(caps.codecs.begin(), caps.codecs.end(), [&](const auto& c) {
      return c.name == "AV1" || (slot == 1 && c.name == "H264");
    }), caps.codecs.end());
    auto pref = trs[slot]->SetCodecPreferences(caps.codecs);
    if (!pref.ok()) logf("call %s codec preferences: %s", slot == 2 ? "screen" : "camera", pref.message());
  }
}

// A picture this side said it sends and does not: the camera would not open,
// the shared window closed. The model takes it off (direct-call.cpp) and
// tells the other side, rather than leave them a frozen tile.
void Connection::Ended(const char* what) {
  auto payload = json::Value::Object(); payload[what] = false;
  Emit("capture", payload);
}

// The room's meter (voice.cpp Session::Meter): linear RMS of the last 512
// mono samples, so any sound above -60 dBFS can light the ring.
struct Connection::Meter : webrtc::AudioTrackSinkInterface {
  explicit Meter(std::atomic<double>* out) : out_(out) {}
  void OnData(const void* data, int bits, int, size_t channels, size_t frames) override {
    if (!data || bits != 16 || !channels || !frames) return;
    auto samples = static_cast<const int16_t*>(data);
    for (size_t i = 0; i < frames; ++i) {
      int mono = 0; for (size_t c = 0; c < channels; ++c) mono += samples[i * channels + c];
      ring_[index_++ % 512] = double(mono) / double(channels) / 32768.0;
    }
    double sum = 0; for (auto v : ring_) sum += v * v;
    out_->store(std::sqrt(sum / 512)); at_.store(GetTickCount64());
  }
  std::atomic<double>* out_;
  std::atomic<uint64_t> at_{0};
  double ring_[512]{};
  size_t index_ = 0;
};
namespace {
std::atomic<double> mine_level{0}, their_level{0};
std::atomic<uint64_t>* mine_at = nullptr;
std::atomic<uint64_t>* their_at = nullptr;
std::mutex levels_mu;
}
void Levels(double* mine, double* theirs) {
  std::lock_guard lock(levels_mu);
  auto now = GetTickCount64();
  // A meter that stopped hearing anything (muted at the source, a track gone)
  // reads zero after 100 ms, as the room's does.
  *mine = mine_at && now - mine_at->load() < 100 ? mine_level.load() : 0;
  *theirs = their_at && now - their_at->load() < 100 ? their_level.load() : 0;
}

// The two meters: this side's microphone at its source, and the voice that
// arrives in slot 0 (a sink on the remote track, as the room's receptions).
void Connection::Meters() {
  if (!own_meter_ && audio_.mic) {
    own_meter_ = std::make_unique<Meter>(&mine_level);
    audio_.mic->AddSink(own_meter_.get());
    std::lock_guard lock(levels_mu); mine_at = &own_meter_->at_;
  }
  if (!pc_) return;
  auto trs = pc_->GetTransceivers();
  if (trs.empty()) return;
  auto track = trs[0]->receiver()->track();
  if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind || track.get() == peer_voice_.get()) return;
  if (peer_voice_ && peer_meter_) peer_voice_->RemoveSink(peer_meter_.get());
  peer_voice_ = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(static_cast<webrtc::AudioTrackInterface*>(track.get()));
  if (!peer_meter_) peer_meter_ = std::make_unique<Meter>(&their_level);
  peer_voice_->AddSink(peer_meter_.get());
  std::lock_guard lock(levels_mu); their_at = &peer_meter_->at_;
}

// "Surdo": what arrives in slots 0 and 3 plays at zero. The track keeps
// arriving (the meter still lights their ring); only the speaker is quiet.
// Otherwise each plays at the volume chosen for that person.
void Connection::ApplyDeafen() {
  if (!pc_) return;
  auto trs = pc_->GetTransceivers();
  for (size_t slot : {size_t(0), size_t(3)}) {
    if (slot >= trs.size()) continue;
    auto track = trs[slot]->receiver()->track();
    if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) continue;
    double volume = deafened_ ? 0.0 : slot == 0 ? voice_volume_ : screen_volume_;
    if (auto* source = static_cast<webrtc::AudioTrackInterface*>(track.get())->GetSource()) source->SetVolume(volume);
  }
}
void Connection::Volumes(double voice, double screen) {
  Post([this, voice, screen] { voice_volume_ = voice; screen_volume_ = screen; ApplyDeafen(); });
}
void Connection::Deafen(bool deafened) {
  Post([this, deafened] { deafened_ = deafened; ApplyDeafen(); logf("call deafened=%d", int(deafened)); });
}

void Connection::Emit(const std::string& kind, json::Value payload) {
  if (closed_ || !options_.send) return;
  options_.send(kind, std::move(payload));
}

bool Connection::Open(bool polite, const json::Value& ice_servers, std::string* error) {
  std::promise<bool> done;
  auto f = done.get_future();
  Post([&] { done.set_value(Build(polite, ice_servers, error)); });
  return f.get();
}

bool Connection::Build(bool polite, const json::Value& ice, std::string* error) {
  polite_ = polite;
  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create();
  signaling_ = webrtc::Thread::Create();
  network_->SetName("harmony-call-network", nullptr);
  worker_->SetName("harmony-call-worker", nullptr);
  signaling_->SetName("harmony-call-signaling", nullptr);
  network_->Start();
  worker_->Start();
  signaling_->Start();
  // The voice room's pipeline: the same echo canceller and noise filter, so
  // a person sounds the same in a call as in a room.
  audio_ = worker_->BlockingCall([this] {
    return audio::CreatePipeline(options_.test_tone, {}, options_.simulated_device, false);
  });
  if (!audio_.device) {
    if (error) *error = "audio";
    return false;
  }
  audio_.filter->echo = options_.echo;
  audio_.filter->noise = options_.noise;
  audio_.filter->agc = options_.agc;
  audio_.filter->micGain = options_.micGain;
  audio_.filter->enabled = options_.dfn;
  audio_.filter->strength = options_.strength;
  ApplySidetone();
  ChooseDevices();
#if HARMONY_GPU
  // The room's video coders: H.264 on the graphics card (the owner's choice
  // for a screen, 19/09/2026), falling back to the processor.
  auto encoders = gpu::CreateEncoderFactory();
  auto decoders = gpu::CreateDecoderFactory();
#else
  auto encoders = webrtc::CreateBuiltinVideoEncoderFactory();
  auto decoders = webrtc::CreateBuiltinVideoDecoderFactory();
#endif
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_.get(), worker_.get(), signaling_.get(), audio_.device, webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(), std::move(encoders),
      // libwebrtc's own mixer, not the room's: ReceiveMixer silences every
      // stream it has no gain for, and a browser's SSRCs are not known here
      // until its first packet. One person, one volume.
      std::move(decoders), nullptr, audio_.engineProcessing);
  if (!factory_) {
    if (error) *error = "factory";
    return false;
  }
  if (options_.loopback) {
    webrtc::PeerConnectionFactoryInterface::Options o;
    o.network_ignore_mask = 0;
    factory_->SetOptions(o);
  }
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // The polite side's rollback is the browser's implicit one: an offer that
  // lands on a pending offer of ours undoes ours first.
  config.enable_implicit_rollback = true;
  // Never an empty list on purpose (the 18/09 lesson): the server sends STUN
  // and TURN with short-lived credentials.
  for (const auto& s : ice.Elements()) {
    webrtc::PeerConnectionInterface::IceServer server;
    const auto& urls = s["urls"];
    if (urls.IsArray())
      for (const auto& u : urls.Elements()) server.urls.push_back(u.Str());
    else if (urls.IsString())
      server.urls.push_back(urls.Str());
    server.username = s["username"].Str();
    server.password = s["credential"].Str();
    if (!server.urls.empty()) config.servers.push_back(server);
  }
  logf("call ice servers: %zu", config.servers.size());
  auto result = factory_->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(this));
  if (!result.ok()) {
    logf("call connection failed: %s", result.error().message());
    if (error) *error = "connection";
    return false;
  }
  pc_ = result.MoveValue();
  networkStats_=webrtc::make_ref_counted<network::Poller>(true);
  networkStats_->also=[](webrtc::RTCStatsReport const& report){audio::RecordStats(report);};
  { std::lock_guard lock(muteMu_); muteSource_ = audio_.mic; muteSource_->Mute(muted_); }
  mic_ = factory_->CreateAudioTrack("mic", audio_.mic.get());
  mic_->set_enabled(!muted_);
#ifndef HARMONY_PUBLISH
  if (options_.test_tone && !options_.simulated_device) {
    // The test tone is pumped into the microphone, as the voice room does it
    // (voice.cpp TestTone): the device's own capture is ignored in that mode.
    auto mic = audio_.mic; auto filter = audio_.filter;
    tone_ = std::thread([this, mic, filter] {
      std::vector<int16_t> block(480); uint64_t n = 0;
      auto next = std::chrono::steady_clock::now();
      while (!tone_stop_) {
        // Beeps, 200 ms in every second: a steady tone is what the noise
        // filter is there to remove, and it does.
        for (auto& sample : block) { sample = (n % 48000) < 9600 ? int16_t(6000 * std::sin(6.283185307179586 * 440 * double(n) / 48000)) : 0; ++n; }
        filter->nsApplied = false;
        mic->Deliver(block.data(), 48000, 1, 480);
        next += std::chrono::milliseconds(10); std::this_thread::sleep_until(next);
      }
    });
  }
#endif
  if (!polite) {
    // All four slots at once, sendrecv, in the fixed order; anything later
    // is a track dropped into a slot already negotiated (replaceTrack).
    const webrtc::MediaType kinds[] = {webrtc::MediaType::AUDIO, webrtc::MediaType::VIDEO, webrtc::MediaType::VIDEO,
                                       webrtc::MediaType::AUDIO};
    for (int i = 0; i < 4; ++i) {
      webrtc::RtpTransceiverInit init;
      init.direction = webrtc::RtpTransceiverDirection::kSendRecv;
      auto tr = pc_->AddTransceiver(kinds[i], init);
      if (i == 0 && tr.ok()) tr.value()->sender()->SetTrack(mic_.get());
    }
    Prefer();
  }
  logf("call connection built (%s)", polite ? "polite" : "impolite");
  return true;
}

// The devices chosen in Ajustes, before anything opens them; one that is
// gone falls back to the Windows default, as in a voice room.
void Connection::ChooseDevices() {
  worker_->BlockingCall([this] {
    auto device = audio_.device;
    auto choose = [&](bool capture, const std::string& id) {
      int32_t result = -1;
      if (!id.empty()) {
        auto count = capture ? device->RecordingDevices() : device->PlayoutDevices();
        for (uint16_t index = 0; index < count; ++index) {
          char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
          auto status = capture ? device->RecordingDeviceName(index, name, guid) : device->PlayoutDeviceName(index, name, guid);
          if (!status && id == guid) {
            result = capture ? device->SetRecordingDevice(index) : device->SetPlayoutDevice(index);
            break;
          }
        }
        if (result) logf("call device %s not found; Windows default", capture ? "capture" : "playout");
      }
      if (result) {
        capture ? device->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice)
                : device->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
      }
    };
    choose(true, options_.input);
    choose(false, options_.output);
  });
}

json::Value Connection::LocalJson() {
  std::string type, sdp;
  signaling_->BlockingCall([&] {
    const auto* local = pc_->local_description();
    if (!local) return;
    local->ToString(&sdp);
    type = local->type();
  });
  json::Value d = json::Value::Object();
  if (type.empty()) return d;
  d["type"] = type;
  d["sdp"] = sdp;
  return d;
}

void Connection::OnNegotiationNeededEvent(uint32_t event_id) {
  Post([this, event_id] {
    if (closed_ || !pc_ || !pc_->ShouldFireNegotiationNeededEvent(event_id)) return;
    making_offer_ = true;
    if (SetLocalImplicit(pc_.get())) Emit("offer", LocalJson());
    making_offer_ = false;
  });
}

void Connection::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  json::Value c = json::Value::Object();
  c["candidate"] = candidate->ToString();
  c["sdpMid"] = candidate->sdp_mid();
  c["sdpMLineIndex"] = candidate->sdp_mline_index();
  Post([this, c] { Emit("ice", c); });
}

void Connection::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  using S = webrtc::PeerConnectionInterface::PeerConnectionState;
  Post([this, state] {
    if (closed_ || !pc_) return;
    logf("call connection %d", int(state));
    bool now = state == S::kConnected;
    if (now != connected_) {
      connected_ = now;
      if (now && restarts_) logf("call ICE back after %d restart(s); same connection", restarts_);
      if (now) restarts_ = 0;
      if (options_.status) options_.status(now);
    }
    if (state == S::kDisconnected || state == S::kFailed) {
      // One repair before anybody is told, on both sides: perfect negotiation
      // keeps the two restarts from colliding (direct.ts networkChanged).
      ++restarts_;
      logf("call ICE restart (%s)", state == S::kFailed ? "failed" : "disconnected");
      pc_->RestartIce();
    }
  });
}

void Connection::Remote(const std::string& kind, const json::Value& payload) {
  Post([this, kind, payload] { Signal(kind, payload); });
}

// One message from the other side: perfect negotiation, with the reasons in
// direct.ts signal().
void Connection::Signal(const std::string& kind, const json::Value& payload) {
  if (closed_ || !pc_) return;
  if (kind == "ice") {
    if (ignore_offer_) return;  // belongs to an offer we pretended not to hear
    if (!has_remote_) { early_candidates_.push_back(payload); return; }
    AddCandidate(payload);
    return;
  }
  std::string type = payload["type"].Str();
  if (type != "offer" && type != "answer") return;
  bool offer = type == "offer";
  bool ready = !making_offer_ && pc_->signaling_state() == webrtc::PeerConnectionInterface::kStable;
  bool collision = offer && !ready;
  ignore_offer_ = !polite_ && collision;
  if (ignore_offer_) {
    logf("call offer ignored (glare, impolite)");
    return;
  }
  if (collision) logf("call glare: rolling my offer back (polite)");
  // An answer with nothing of ours pending is a late duplicate: applying it
  // would fail the connection's state machine for nothing.
  if (!offer && pc_->signaling_state() != webrtc::PeerConnectionInterface::kHaveLocalOffer) {
    logf("call stray answer ignored");
    return;
  }
  auto desc = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer,
                                               payload["sdp"].Str());
  if (!desc) {
    logf("call %s unreadable", type.c_str());
    return;
  }
  auto obs = webrtc::make_ref_counted<RemoteObserver>();
  auto f = obs->promise.get_future();
  pc_->SetRemoteDescription(std::move(desc), obs);
  if (!f.get()) return;
  if (!has_remote_) {
    has_remote_ = true;
    auto early = std::move(early_candidates_); early_candidates_.clear();
    if (!early.empty()) logf("call %zu early candidate(s) applied", early.size());
    for (auto const& c : early) AddCandidate(c);
  }
  if (!offer) {
    if (camera_) Price(1); if (screen_) Price(2);
    // The side that called gets its peer's voice here, with no picture
    // involved: the ring's meter cannot wait for a video panel.
    Meters(); ApplyDeafen();
    return;
  }
  // The offer is what brings the slots to the side that answers, and a slot
  // made by somebody else's offer is born receive-only: the voice goes into
  // the first one, sendrecv, before the answer goes back. The other slots
  // turn sendrecv now too, empty, so that a camera or a screen later is only
  // a track dropped in (replaceTrack), not another negotiation (direct.ts,
  // DirectCall.kt do the same).
  Send(0, mic_.get());
  {
    auto trs = pc_->GetTransceivers();
    for (size_t slot = 1; slot < 4 && slot < trs.size(); ++slot)
      if (trs[slot]->direction() != webrtc::RtpTransceiverDirection::kSendRecv)
        trs[slot]->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
    if (polite_) Prefer();
  }
  // Whatever was switched on while the slots were not here yet goes in too.
  if (camera_) Send(1, camera_.get());
  if (screen_) Send(2, screen_.get());
  if (screen_audio_) Send(3, screen_audio_.get());
  if (SetLocalImplicit(pc_.get())) Emit("answer", LocalJson());
  // A new negotiation can change the agreed codecs: choose again.
  if (camera_) Price(1);
  if (screen_) Price(2);
  AttachVideos();
}

void Connection::AddCandidate(const json::Value& payload) {
  // sdpMid missing or null stays empty; a missing index is 0 (§7).
  auto cand = webrtc::IceCandidate::Create(payload["sdpMid"].Str(),
                                           int(std::clamp<int64_t>(payload["sdpMLineIndex"].Int(), 0, 64)),
                                           payload["candidate"].Str());
  if (!cand) return;
  // The result comes back on libwebrtc's thread: it reads nothing of ours.
  pc_->AddIceCandidate(std::move(cand), [](webrtc::RTCError e) {
    if (!e.ok()) logf("call candidate: %s", e.message());
  });
}

// One slot: the track this side sends in it, sendrecv (a slot made by the
// other side's offer is born receive-only), with the room's ceilings.
void Connection::Send(size_t slot, webrtc::MediaStreamTrackInterface* track) {
  if (!pc_) return;
  auto trs = pc_->GetTransceivers();
  if (slot >= trs.size()) return;  // the slots come with the first offer
  auto& tr = trs[slot];
  if (track && tr->direction() != webrtc::RtpTransceiverDirection::kSendRecv)
    tr->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
  if (tr->sender()->track().get() == track) return;
  tr->sender()->SetTrack(track);
  if (track && (slot == 1 || slot == 2)) Price(slot);
}

// The ceilings are the room's (media.ts, media-policy.h): a screen shared in a
// call costs what a screen in a room costs. A screen is a game or a film:
// keep the frames, shrink the picture (maintain-framerate, CLAUDE.md).
void Connection::Price(size_t slot) {
  auto trs = pc_->GetTransceivers();
  if (slot >= trs.size()) return;
  auto sender = trs[slot]->sender();
  auto ceilings = [&](webrtc::RtpParameters& params) {
    if (slot == 2) {
      params.encodings[0].max_bitrate_bps = media::ScreenCeiling(screen_height_, screen_fps_);
      params.encodings[0].max_framerate = screen_fps_;
      params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
    } else {
      params.encodings[0].max_bitrate_bps = 900000;  // CAMERA_BITRATE
    }
  };
  // The codec this side sends in is the first of the OTHER side's list unless
  // said otherwise, and the web offers VP8 first: when this side answered,
  // its screen went out in VP8. The screen is H.264 (the owner's
  // decision), the camera VP8 (the GPU's H.264 takes no camera frames). Of
  // the H.264 lines the browser agreed, only those our encoder makes are
  // taken (42001f is refused, 42e01f is not), so each is tried; a refusal
  // spends the parameters, hence GetParameters again every time.
  const char* wanted = slot == 2 ? "H264" : "VP8";
  auto agreed = sender->GetParameters().codecs;
  for (auto const& c : agreed) {
    if (c.name != wanted) continue;
    auto params = sender->GetParameters();
    if (params.encodings.empty()) return;
    ceilings(params); params.encodings[0].codec = c;
    if (sender->SetParameters(params).ok()) {
      auto profile = c.parameters.find("profile-level-id");
      logf("call %s codec %s %s", slot == 2 ? "screen" : "camera", wanted, profile == c.parameters.end() ? "" : profile->second.c_str());
      return;
    }
  }
  auto params = sender->GetParameters();
  if (params.encodings.empty()) return;
  ceilings(params);
  auto r = sender->SetParameters(params);
  logf("call %s codec %s not agreed; the negotiated one%s%s", slot == 2 ? "screen" : "camera", wanted, r.ok() ? "" : ": ", r.ok() ? "" : r.message());
}

void Connection::Camera(bool on, std::string id) {
  Post([this, on, id = std::move(id)] {
    if (closed_ || !factory_) return;
    camera_ = nullptr;
    if (on) {
      auto source = camera::Open(id);
      if (source) {
        camera_ = factory_->CreateVideoTrack(source, "cam");
        camera_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);
      }
      logf("call camera %s", camera_ ? "on" : "unavailable");
      if (!camera_) Ended("camera");
    } else logf("call camera off");
    Send(1, camera_.get());
    AttachVideos();
  });
}

void Connection::Screen(HWND window, HMONITOR monitor, int height, int fps, bool sound) {
  Post([this, window, monitor, height, fps, sound] {
    if (closed_ || !factory_ || (!window) == (!monitor)) return;
#if HARMONY_GPU
    screen_height_ = height == 480 || height == 720 ? height : 1080;
    screen_fps_ = fps == 15 || fps == 30 ? fps : 60;
    pending_screen_source_=window ? gpu::CreateWindowSource(window, screen_fps_, screen_height_) : gpu::CreateMonitorSource(monitor, screen_fps_, screen_height_);
    pending_screen_window_=window;pending_screen_sound_=sound;pending_screen_deadline_=GetTickCount64()+4000;
    if(pending_screen_source_&&pending_screen_source_->state()==webrtc::MediaSourceInterface::kLive)CommitScreen();
#else
    (void)window; (void)monitor; (void)height; (void)fps; (void)sound;
    logf("call screen: built without the GPU module");
#endif
  });
}

void Connection::CommitScreen() {
#if HARMONY_GPU
    auto source=pending_screen_source_;
    auto replacement=factory_->CreateVideoTrack(source,"screen");
    pending_screen_source_=nullptr;
    if(!replacement){if(!screen_)Ended("screen");return;}
    screen_=replacement;
    screen_capture_=source.get();
    screen_window_=pending_screen_window_;screen_sound_=pending_screen_sound_;
    if (screen_) screen_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);
    screen_audio_ = nullptr;
    if (screen_ && pending_screen_sound_) {
      // The PC's sound minus harmony's, or one window's process tree.
      DWORD pid = 0; if (pending_screen_window_) GetWindowThreadProcessId(pending_screen_window_, &pid);
      auto loop = loopback::SystemAudioSource::Create(pid, false);
      if (loop) screen_audio_ = factory_->CreateAudioTrack("screen_audio", loop.get());
    }
    logf("call screen %s height=%d fps=%d sound=%d", screen_ ? "on" : "unavailable", screen_height_, screen_fps_, bool(screen_audio_));
    if (!screen_) Ended("screen");
    AttachVideos();
    Send(2, screen_.get());
    Send(3, screen_audio_.get());
#endif
}

void Connection::StopScreen() {
  Post([this] {
    pending_screen_source_=nullptr;
    screen_ = nullptr; screen_audio_ = nullptr;
    Send(2, nullptr); Send(3, nullptr);
    logf("call screen off");
    AttachVideos();
  });
}

void Connection::Video(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach) {
  Post([this, slot, attach = std::move(attach)] {
    if (slot < 1 || slot > 4) return;
    if (video_attach_[slot] && video_shown_[slot]) video_attach_[slot](nullptr);
    video_shown_[slot] = nullptr;
    video_attach_[slot] = attach;
    AttachVideos();
  });
}

void Connection::AttachVideos() {
  if (!pc_) return;
  auto trs = pc_->GetTransceivers();
  for (int slot = 1; slot <= 2; ++slot) {
    webrtc::VideoTrackInterface* found = nullptr;
    if (size_t(slot) < trs.size()) {
      auto track = trs[slot]->receiver()->track();
      if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) found = static_cast<webrtc::VideoTrackInterface*>(track.get());
    }
    if (found != counted_[slot].get()) {
      if (counted_[slot]) counted_[slot]->RemoveSink(&frames_[slot]);
      counted_[slot] = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(found);
      if (found) found->AddOrUpdateSink(&frames_[slot], webrtc::VideoSinkWants{});
    }
    if (!video_attach_[slot]) continue;
    if (found != video_shown_[slot]) { video_shown_[slot] = found; video_attach_[slot](found); }
  }
  // This side's own camera and screen, as its own preview.
  for (int slot = 3; slot <= 4; ++slot) {
    if (!video_attach_[slot]) continue;
    webrtc::VideoTrackInterface* mine = slot == 3 ? camera_.get() : screen_.get();
    if (mine != video_shown_[slot]) { video_shown_[slot] = mine; video_attach_[slot](mine); }
  }
  Meters();
  ApplyDeafen();
}
void Connection::Uncount() {
  for (int slot = 1; slot <= 2; ++slot) if (counted_[slot]) { counted_[slot]->RemoveSink(&frames_[slot]); counted_[slot] = nullptr; }
}

void Connection::Device(bool capture, std::string id) {
  Post([this, capture, id = std::move(id)] {
    if (!audio_.device) return;
    (capture ? options_.input : options_.output) = id;
    ChooseDevice(capture, id);
  });
}

// The voice room's switch (voice.cpp ChooseDevice), without its recovery
// ladder: stop, choose (or the Windows default if it is gone), start again.
void Connection::ChooseDevice(bool capture, const std::string& id) {
  bool ok = worker_->BlockingCall([&] {
    auto device = audio_.device;
    bool active = capture ? device->Recording() : device->Playing();
    capture ? device->StopRecording() : device->StopPlayout();
    int32_t result = -1;
    if (!id.empty()) {
      auto count = capture ? device->RecordingDevices() : device->PlayoutDevices();
      for (uint16_t index = 0; index < count; ++index) {
        char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
        auto status = capture ? device->RecordingDeviceName(index, name, guid) : device->PlayoutDeviceName(index, name, guid);
        if (!status && id == guid) { result = capture ? device->SetRecordingDevice(index) : device->SetPlayoutDevice(index); break; }
      }
    }
    if (result) result = capture ? device->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice)
                                  : device->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
    if (!result) result = capture ? device->InitRecording() : device->InitPlayout();
    if (!result && active) result = capture ? device->StartRecording() : device->StartPlayout();
    return result == 0;
  });
  logf("call device switched capture=%d default=%d ok=%d", capture, id.empty(), ok);
}

void Connection::Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double micGain) {
  Post([this, dfn, strength, echo, noise, agc, micGain] {
    if (!audio_.filter) return;
    audio_.filter->enabled = dfn; audio_.filter->strength = strength;
    audio_.filter->echo = echo; audio_.filter->noise = noise; audio_.filter->agc = agc; audio_.filter->micGain = micGain;
  });
}

void Connection::Mute(bool muted) {
  { std::lock_guard lock(muteMu_); muted_ = muted; if (muteSource_) muteSource_->Mute(muted); }
  Post([this] {
    if (mic_) mic_->set_enabled(!muted_);
    ApplySidetone();
  });
}

void Connection::Sidetone(bool on) {
  Post([this, on] { options_.sidetone = on; ApplySidetone(); logf("call sidetone=%d", int(on)); });
}

void Connection::Close() {
  std::promise<void> done;
  auto f = done.get_future();
  bool posted = false;
  {
    std::lock_guard<std::mutex> lock(qmu_);
    if (!stop_) {
      posted = true;
      queue_.push_back([this, &done] {
        if (!closed_) {
          closed_ = true;
          tone_stop_ = true;
          for (int slot = 1; slot <= 4; ++slot) { if (video_attach_[slot] && video_shown_[slot]) video_attach_[slot](nullptr); video_attach_[slot] = nullptr; }
          {
            std::lock_guard lock(levels_mu); mine_at = nullptr; their_at = nullptr;
          }
          if (peer_voice_ && peer_meter_) peer_voice_->RemoveSink(peer_meter_.get());
          if (own_meter_ && audio_.mic) audio_.mic->RemoveSink(own_meter_.get());
          peer_voice_ = nullptr;
          Uncount();
          camera_ = nullptr; screen_ = nullptr; screen_audio_ = nullptr;pending_screen_source_=nullptr;
          if (pc_) pc_->Close();
          pc_ = nullptr;
          logf("call media closed");
        }
        done.set_value();
      });
    }
  }
  qcv_.notify_all();
  if (posted) f.wait();
}

}  // namespace callmedia
