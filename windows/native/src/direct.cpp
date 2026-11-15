#include "direct.h"

#include <algorithm>
#include <future>

#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "log.h"
#include "perf.h"

namespace direct {
namespace {

class LocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("direct set local: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

class RemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("direct set remote: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

// The parameterless setLocalDescription: an offer or an answer, whichever the
// state calls for — the same call perfect negotiation is written with.
bool SetLocalImplicit(webrtc::PeerConnectionInterface* pc) {
  auto obs = webrtc::make_ref_counted<LocalObserver>();
  auto f = obs->promise.get_future();
  pc->SetLocalDescription(obs);
  return f.get();
}

}  // namespace

json::Value Call::LocalJson() {
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

void Call::CloseConnection() {
  live_ = false;
  if (pc_) pc_->Close();
}

Call::Call(net::Client* client) : client_(client) {
  thread_ = std::thread([this] { Loop(); });
}

Call::~Call() {
  {
    std::lock_guard<std::mutex> lock(qmu_);
    stop_ = true;
  }
  qcv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (pc_) pc_->Close();
  pc_ = nullptr;
  mic_ = nullptr;
  factory_ = nullptr;
  if (adm_) {
    worker_->BlockingCall([this] {
      adm_->Terminate();
      adm_ = nullptr;
    });
  }
}

void Call::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(qmu_);
    queue_.push_back(std::move(task));
  }
  qcv_.notify_all();
}

// One signalling step at a time (direct.ts serialise).
void Call::Loop() {
  perf::NameThread(L"harmony-direct");
  std::unique_lock<std::mutex> lock(qmu_);
  while (!stop_) {
    if (queue_.empty()) {
      qcv_.wait(lock);
      continue;
    }
    auto task = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    task();
    lock.lock();
  }
}

bool Call::Build(bool polite, const json::Value& ice) {
  polite_ = polite;
  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create();
  signaling_ = webrtc::Thread::Create();
  network_->Start();
  worker_->Start();
  signaling_->Start();
  adm_ = worker_->BlockingCall([] {
    return webrtc::CreateAudioDeviceModule(webrtc::CreateEnvironment(),
                                           webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  });
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_.get(), worker_.get(), signaling_.get(), adm_, webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(), webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);
  if (!factory_) return false;
  const std::wstring& host = client_->host();
  if (host == L"127.0.0.1" || host == L"localhost") {
    webrtc::PeerConnectionFactoryInterface::Options o;
    o.network_ignore_mask = 0;
    factory_->SetOptions(o);
  }
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // The polite side's rollback is the browser's implicit one: an offer that
  // lands on a pending offer of ours undoes ours first.
  config.enable_implicit_rollback = true;
  for (const auto& s : ice.Elements()) {
    webrtc::PeerConnectionInterface::IceServer server;
    for (const auto& u : s["urls"].Elements()) server.urls.push_back(u.Str());
    if (s["urls"].IsString()) server.urls.push_back(s["urls"].Str());
    server.username = s["username"].Str();
    server.password = s["credential"].Str();
    if (!server.urls.empty()) config.servers.push_back(server);
  }
  auto result = factory_->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(this));
  if (!result.ok()) return false;
  pc_ = result.MoveValue();
  webrtc::AudioOptions audio;
  audio.echo_cancellation = true;
  mic_ = factory_->CreateAudioTrack("mic", factory_->CreateAudioSource(audio).get());
  if (!polite) {
    // All four slots at once, sendrecv, fixed order; everything later is a
    // track dropped into a slot that is already negotiated.
    const webrtc::MediaType kinds[] = {webrtc::MediaType::AUDIO, webrtc::MediaType::VIDEO, webrtc::MediaType::VIDEO,
                                       webrtc::MediaType::AUDIO};
    for (int i = 0; i < 4; ++i) {
      webrtc::RtpTransceiverInit init;
      init.direction = webrtc::RtpTransceiverDirection::kSendRecv;
      auto tr = pc_->AddTransceiver(kinds[i], init);
      if (i == 0 && tr.ok()) tr.value()->sender()->SetTrack(mic_.get());
    }
  }
  logf("direct connection built (%s)", polite ? "polite" : "impolite");
  return true;
}

bool Call::Ring(const std::string& username, std::string* error) {
  std::promise<bool> done;
  auto f = done.get_future();
  Post([&] {
    json::Value body = json::Value::Object();
    body["username"] = username;
    auto r = client_->Post("/api/calls", body);
    if (r.status != 201) {
      *error = "ring " + std::to_string(r.status) + " " + r.Json()["error"].Str();
      done.set_value(false);
      return;
    }
    json::Value res = r.Json();
    id_ = res["id"].Str();
    logf("direct ringing @%s", username.c_str());
    done.set_value(Build(res["polite"].Bool(), res["ice_servers"]));
  });
  return f.get();
}

void Call::AcceptNext() {
  Post([this] { accept_next_ = true; });
}

void Call::Send(const std::string& kind, const json::Value& payload) {
  // While it rings there is no device on the other side yet and the server
  // answers 409 not_answered: hold everything, offer first (outbox).
  if (!live_) {
    outbox_.emplace_back(kind, payload);
    return;
  }
  json::Value body = json::Value::Object();
  body["kind"] = kind;
  body["payload"] = payload;
  auto r = client_->Post("/api/calls/" + id_ + "/signal", body);
  if (r.status != 204) logf("direct signal %s -> %d", kind.c_str(), r.status);
}

void Call::FlushOutbox() {
  auto pending = std::move(outbox_);
  outbox_.clear();
  for (auto& [kind, payload] : pending) Send(kind, payload);
}

void Call::OnNegotiationNeededEvent(uint32_t event_id) {
  Post([this, event_id] {
    if (!pc_ || !pc_->ShouldFireNegotiationNeededEvent(event_id)) return;
    making_offer_ = true;
    if (SetLocalImplicit(pc_.get())) Send("offer", LocalJson());
    making_offer_ = false;
  });
}

void Call::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  json::Value c = json::Value::Object();
  c["candidate"] = candidate->ToString();
  c["sdpMid"] = candidate->sdp_mid();
  c["sdpMLineIndex"] = candidate->sdp_mline_index();
  Post([this, c] { Send("ice", c); });
}

void Call::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
  using S = webrtc::PeerConnectionInterface;
  Post([this, state] {
    logf("direct ICE %d", int(state));
    if (state == S::kIceConnectionConnected || state == S::kIceConnectionCompleted) {
      if (!connected_) logf("direct connected");
      connected_ = true;
      if (restarting_) {
        restarting_ = false;
        ++restarts_connected_;
        logf("direct ICE restart connected (%d)", restarts_connected_.load());
      }
    }
  });
}

void Call::RestartIce() {
  Post([this] {
    if (!pc_) return;
    restarting_ = true;
    logf("direct restartIce (on purpose)");
    pc_->RestartIce();
  });
}

// One message from the other side: perfect negotiation, with the reasons in
// direct.ts signal().
void Call::Signal(const json::Value& data) {
  if (data["id"].Str() != id_ || !pc_) return;
  const std::string kind = data["kind"].Str();
  const json::Value& payload = data["payload"];
  if (kind == "ice") {
    if (ignore_offer_) return;  // belongs to an offer we pretended not to hear
    std::string mid = payload["sdpMid"].Str();
    auto cand = webrtc::IceCandidate::Create(mid, int(std::clamp<int64_t>(payload["sdpMLineIndex"].Int(), 0, 64)), payload["candidate"].Str());
    if (cand) {
      pc_->AddIceCandidate(std::move(cand), [this](webrtc::RTCError e) {
        if (!e.ok() && !ignore_offer_) logf("direct candidate: %s", e.message());
      });
    }
    return;
  }
  std::string type = payload["type"].Str();
  bool offer = type == "offer";
  bool ready = !making_offer_ && pc_->signaling_state() == webrtc::PeerConnectionInterface::kStable;
  bool collision = offer && !ready;
  ignore_offer_ = !polite_ && collision;
  if (ignore_offer_) {
    ++offers_ignored_;
    logf("direct offer ignored (glare, impolite)");
    return;
  }
  if (collision) {
    ++rollbacks_;
    logf("direct glare: rolling my offer back (polite)");
  }
  auto desc = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer,
                                               payload["sdp"].Str());
  if (!desc) return;
  auto obs = webrtc::make_ref_counted<RemoteObserver>();
  auto f = obs->promise.get_future();
  pc_->SetRemoteDescription(std::move(desc), obs);
  if (!f.get() || !offer) return;
  if (polite_) {
    // The offer is what brings the slots to the side that answers; the voice
    // goes into the first one before the answer goes back.
    auto trs = pc_->GetTransceivers();
    if (!trs.empty() && !trs[0]->sender()->track()) {
      trs[0]->sender()->SetTrack(mic_.get());
      trs[0]->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
    }
  }
  if (SetLocalImplicit(pc_.get())) Send("answer", LocalJson());
}

void Call::OnSocketEvent(const std::string& type, const json::Value& data) {
  if (type == "call") {
    Post([this, data] {
      if (!accept_next_ || pc_) return;
      id_ = data["id"].Str();
      auto r = client_->Post("/api/calls/" + id_ + "/accept", json::Value::Object());
      if (r.status != 200) {
        logf("direct accept -> %d", r.status);
        return;
      }
      live_ = true;
      json::Value res = r.Json();
      logf("direct accepted call from @%s", data["from"]["username"].Str().c_str());
      Build(res["polite"].Bool(true), res["ice_servers"]);
    });
  } else if (type == "call_accepted") {
    Post([this, data] {
      if (data["id"].Str() != id_) return;
      live_ = true;
      logf("direct the other side answered");
      FlushOutbox();
    });
  } else if (type == "call_signal") {
    Post([this, data] { Signal(data); });
  } else if (type == "call_ended") {
    Post([this, data] {
      if (data["id"].Str() != id_) return;
      logf("direct call ended: %s", data["reason"].Str().c_str());
      CloseConnection();
    });
  }
}

std::string Call::Report() {
  std::promise<std::string> done;
  auto f = done.get_future();
  Post([&] {
    if (!pc_) {
      done.set_value("no connection");
      return;
    }
    std::string local_sdp, remote_sdp;
    bool has_local = false, has_remote = false;
    signaling_->BlockingCall([&] {
      if (const auto* d = pc_->local_description()) has_local = d->ToString(&local_sdp);
      if (const auto* d = pc_->remote_description()) has_remote = d->ToString(&remote_sdp);
    });
    auto ufrag = [](bool has, const std::string& sdp) {
      if (!has) return std::string("-");
      auto at = sdp.find("a=ice-ufrag:");
      if (at == std::string::npos) return std::string("?");
      at += 12;
      size_t end = sdp.find_first_of("\r\n", at);
      return sdp.substr(at, end - at);
    };
    char line[256];
    std::snprintf(line, sizeof line, "signaling=%d local=%s remote=%s ice=%d ignored=%d rollbacks=%d",
                  int(pc_->signaling_state()), ufrag(has_local, local_sdp).c_str(),
                  ufrag(has_remote, remote_sdp).c_str(), int(pc_->ice_connection_state()), offers_ignored_.load(),
                  rollbacks_.load());
    done.set_value(line);
  });
  return f.get();
}

void Call::Hangup() {
  std::promise<void> done;
  auto f = done.get_future();
  Post([&] {
    if (!id_.empty()) {
      json::Value body = json::Value::Object();
      body["reason"] = "hangup";
      client_->Post("/api/calls/" + id_ + "/hangup", body);
    }
    CloseConnection();
    done.set_value();
  });
  f.wait();
}

}  // namespace direct
