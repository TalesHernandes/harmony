#include "voice.h"
#include "sync-rtp-probe.h"
#include <fstream>
#include <filesystem>
#ifdef HARMONY_ENGINE_TEST
#include "engine-test-control.h"
#endif

#include <algorithm>
#include <future>
#include <sstream>
#include <iphlpapi.h>
#include "api/stats/rtcstats_objects.h"
#include "network-stats.h"
#include "rtc_base/logging.h"
#include <cmath>
#include <atomic>
#include <mmdeviceapi.h>
#include "receive-gain.h"
#include "screen-bitrate.h"
#if !defined(HARMONY_ENGINE_TEST)
#include "camera.h"
#endif

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/field_trials.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#ifdef HARMONY_ENGINE_TEST
#include "api/video_codecs/sdp_video_format.h"
// Match the production factory's SDP capabilities without opening a GPU.
template<class Factory> class EngineFormats : public Factory {
 public:
  explicit EngineFormats(std::unique_ptr<Factory> base):base_(std::move(base)){}
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    std::vector<webrtc::SdpVideoFormat> formats;
    for (const char* profile : {"42e02a", "64002a"})
      formats.emplace_back("H264", webrtc::SdpVideoFormat::Parameters{
          {"packetization-mode", "1"}, {"profile-level-id", profile}, {"level-asymmetry-allowed", "1"}});
    for (auto& format : base_->GetSupportedFormats())
      if (format.name == "VP8" || format.name == "VP9") formats.push_back(format);
    return formats;
  }
  auto Create(const webrtc::Environment& env,const webrtc::SdpVideoFormat& format) -> decltype(std::declval<Factory>().Create(env,format)) override {return base_->Create(env,format);}
 private:std::unique_ptr<Factory> base_;
};
#endif
#include "log.h"
#include "perf.h"
#if HARMONY_GPU
#include "../gpu/gpu.h"
#endif

namespace voice {
namespace {

// The three layers of a shared screen, largest first: the server adopts them
// in the order the offer names them (media.ts SCREEN_LAYERS). Only the top
// one carries the owner's ceiling — 15 Mbit/s at 1080p60, the number he chose
// on 19/09/2026 watching his own game; the small ones are sized for a phone.
struct Layer {
  const char* rid;
  double scale;
  int max_bps;
  double fps;
};
constexpr Layer kLayers[] = {
    {"alta", 1, 15'000'000, 60},
    {"media", 2, 4'000'000, 30},
    {"baixa", 4, 800'000, 15},
};
// How long a layer keeps going after nobody needs it (media.ts SHRINK_WAIT):
// switching back on costs a full picture.
constexpr int kShrinkWaitMs = 5000;

class CreateObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::string sdp;
    desc->ToString(&sdp);
    delete desc;
    promise.set_value(sdp);
  }
  void OnFailure(webrtc::RTCError error) override {
    logf("voice create failed: %s", error.message());
    promise.set_value("");
  }
  std::promise<std::string> promise;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("voice set local failed: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) logf("voice set remote failed: %s", error.message());
    promise.set_value(error.ok());
  }
  std::promise<bool> promise;
};

template<class T>
T Await(std::future<T>& done,const char* step,int timeoutMs=5000) {
  logf("media open step=%s waiting deadline_ms=%d",step,timeoutMs);
  if(done.wait_for(std::chrono::milliseconds(timeoutMs))!=std::future_status::ready) {
    logf("media open timeout step=%s deadline_ms=%d",step,timeoutMs);
    return T{};
  }
  logf("media open step=%s completed",step);
  return done.get();
}

bool SetRemote(webrtc::PeerConnectionInterface* pc, webrtc::SdpType type, const std::string& sdp) {
  webrtc::SdpParseError err;
  auto desc = webrtc::CreateSessionDescription(type, sdp, &err);
  if (!desc) {
    logf("voice remote sdp did not parse: %s", err.description.c_str());
    return false;
  }
  auto obs = webrtc::make_ref_counted<SetRemoteObserver>();
  auto done = obs->promise.get_future();
  pc->SetRemoteDescription(std::move(desc), obs);
  return Await(done,"remote_description");
}

bool SetLocal(webrtc::PeerConnectionInterface* pc, webrtc::SdpType type, const std::string& sdp) {
  auto desc = webrtc::CreateSessionDescription(type, sdp);
  if (!desc) return false;
  auto obs = webrtc::make_ref_counted<SetLocalObserver>();
  auto done = obs->promise.get_future();
  pc->SetLocalDescription(std::move(desc), obs);
  return Await(done,"local_description");
}

std::string Create(webrtc::PeerConnectionInterface* pc, bool offer) {
  auto obs = webrtc::make_ref_counted<CreateObserver>();
  auto done = obs->promise.get_future();
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  if (offer)
    pc->CreateOffer(obs.get(), opts);
  else
    pc->CreateAnswer(obs.get(), opts);
  return Await(done,offer?"create_offer":"create_answer");
}

int64_t NowMs() {
#ifdef HARMONY_ENGINE_TEST
 return engine_test::now;
#else
 return perf::Stage::Now() / 1'000'000;
#endif
}

// The active endpoints in the order libwebrtc's ADM numbers them after its two
// default entries (core_audio_utility_win.cc GetDeviceNamesInternal walks the
// same EnumAudioEndpoints collection). Read here because the ADM's own walk
// leaks every device it lists (a ComPtr reused with GetAddressOf, :508), and
// asking it for N names walks the list N times: ~56 registry keys per join.
std::vector<DeviceInfo> Endpoints(bool capture) {
  // PKEY_Device_FriendlyName, spelled out to keep devpkey's GUID linkage out.
  static const PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
  std::vector<DeviceInfo> result;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) return result;
  IMMDeviceCollection* collection = nullptr;
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection))) {
    UINT count = 0; collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* device = nullptr;
      if (FAILED(collection->Item(i, &device))) continue;
      DeviceInfo info; LPWSTR id = nullptr;
      if (SUCCEEDED(device->GetId(&id))) { info.id = net::Narrow(id); CoTaskMemFree(id); }
      IPropertyStore* properties = nullptr;
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) info.name = net::Narrow(name.pwszVal);
        PropVariantClear(&name); properties->Release();
      }
      device->Release();
      if (!info.id.empty()) result.push_back(std::move(info));
    }
    collection->Release();
  }
  enumerator->Release();
  return result;
}

}  // namespace

class Session::Meter : public webrtc::AudioTrackSinkInterface {
 public:
  void OnData(const void* data, int bits, int, size_t channels, size_t frames) override {
    // Empty/invalid callbacks are not fresh audio; never extend a speaking ring.
    if (!data || bits != 16 || !channels || !frames) return;
    auto samples = static_cast<const int16_t*>(data);
    // Linear RMS of the last 512 mono samples, as the web's speaking.ts reads
    // float samples: any sound above -60 dBFS counts, not only a loud voice.
    std::lock_guard lock(mu);
    for (size_t i = 0; i < frames; ++i) {
      int mono = 0; for (size_t c = 0; c < channels; ++c) mono += samples[i * channels + c];
      ring[index++ % 512] = double(mono) / double(channels) / 32768.0;
    }
    double sum = 0; for (auto v : ring) sum += v * v;
    amplitude = std::sqrt(sum / 512); at = NowMs();
  }
  double Read(int64_t now) { std::lock_guard lock(mu); return now - at < 100 ? amplitude : 0; }
 private:
  std::mutex mu;
  double ring[512]{};
  size_t index = 0;
  double amplitude = 0;
  int64_t at = 0;
};
struct Session::Flow : webrtc::RTCStatsCollectorCallback {
  network::Reducer network{false};
  std::atomic<uint64_t> incoming{0}, outgoing{0};
  uint64_t beforeIn = 0, beforeOut = 0;
  std::mutex video_mu;
  std::map<std::string,std::pair<uint64_t,uint64_t>> video;
  std::map<uint32_t,uint32_t> video_pli;
  int startup_ticks = 0;
  struct BandwidthLog {double bps=-1,rtt=-1;uint64_t at=0;};
  std::map<std::string,BandwidthLog> bandwidth_logs;
  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    network.Report(*report);
    audio::RecordStats(*report);
    uint64_t in = 0, out = 0;
    for (auto* s : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>())
      if (s->kind.value_or("") == "audio") in += s->bytes_received.value_or(0);
    for (auto* s : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>())
      if (s->kind.value_or("") == "audio") out += s->bytes_sent.value_or(0);
    incoming = in; outgoing = out;
    for (auto* s : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
      if (!s->nominated.value_or(false)) continue;
      auto bps=s->available_outgoing_bitrate.value_or(-1),rtt=1000*s->current_round_trip_time.value_or(-1);
      auto& previous=bandwidth_logs[s->id()];auto now=GetTickCount64();
      auto changed=[](double a,double b){return (a<0)!=(b<0) || std::abs(a-b)>std::abs(b)*.1;};
      if(!previous.at || now-previous.at>=5000 || changed(bps,previous.bps) || changed(rtt,previous.rtt)) {
        logf("voice bandwidth available_bps=%.0f rtt_ms=%.1f",bps,rtt);previous={bps,rtt,now};
      }
    }
    for (auto* s : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      auto rid = s->rid.value_or("");
      if (rid != "alta" && rid != "media" && rid != "baixa") continue;
      auto ssrc = s->ssrc.value_or(0), pli = s->pli_count.value_or(0);
      auto previous = video_pli.find(ssrc);
      if (previous == video_pli.end() || previous->second != pli)
        logf("screen feedback rid=%s ssrc=%u pli=%u",rid.c_str(),ssrc,pli);
      video_pli[ssrc] = pli;
    }
    // The first seconds of a screen decide its bandwidth estimate, so they are
    // told in full: whether a drop there came with lost packets (the SFU's
    // receiver reports) or without (the estimate reacting to delay).
    if (startup_ticks < 40) {
      std::map<std::string,const webrtc::RTCRemoteInboundRtpStreamStats*> remote;
      for (auto* r : report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>())
        if (r->local_id) remote[*r->local_id] = r;
      bool any = false;
      for (auto* s : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
        auto rid = s->rid.value_or("");
        if (rid != "alta" && rid != "media" && rid != "baixa") continue;
        any = true;
        auto r = remote.find(s->id());
        logf("screen startup rid=%s sent=%llu retx=%llu nack=%u target=%.0f limit=%s lost=%lld fraction=%.3f", rid.c_str(),
             (unsigned long long)s->packets_sent.value_or(0), (unsigned long long)s->retransmitted_packets_sent.value_or(0),
             s->nack_count.value_or(0), s->target_bitrate.value_or(-1), s->quality_limitation_reason.value_or("").c_str(),
             r == remote.end() ? -1LL : (long long)r->second->packets_lost.value_or(-1),
             r == remote.end() ? -1.0 : r->second->fraction_lost.value_or(-1));
      }
      if (any) ++startup_ticks;
    }
    std::map<std::string,std::pair<uint64_t,uint64_t>> samples;
    for(auto* s:report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) if(s->kind.value_or("")=="video")samples[s->mid.value_or("")]={s->frames_decoded.value_or(0),s->frames_dropped.value_or(0)};
    {std::lock_guard lock(video_mu);video=std::move(samples);}
  }
};

Session::Session(net::Client* client, Options options) : client_(client), options_(std::move(options)) {
  capture_wanted_=options_.mic;
  // The standalone W1 viewer has no WinUI size callbacks and starts full size.
  if(!options_.watch.empty()) layer_request_.desired=0;
  #ifndef HARMONY_ENGINE_TEST
  thread_ = std::thread([this] { Loop(); });
#endif
}

Session::~Session() {
  if (network_notification_) CancelMibChangeNotify2(network_notification_);
  if (audio_.mixer) audio_.mixer->OnUnknown(nullptr);
  {
    std::lock_guard<std::mutex> lock(qmu_);
    stop_ = true;
  }
  qcv_.notify_all();
  if (thread_.joinable()) thread_.join();
  CloseMedia();
  if (own_meter_ && audio_.mic) audio_.mic->RemoveSink(own_meter_.get());
  own_meter_.reset();
  flow_ = nullptr;
  mic_ = nullptr;
  screen_ = nullptr;
  screen_audio_ = nullptr;
  system_audio_ = nullptr;
  factory_ = nullptr;
  // The audio device must die on the worker that created it, before its thread.
  if (worker_thread_) worker_thread_->BlockingCall([this] { audio_ = {}; });
}

std::string Session::session_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return session_id_;
}

void Session::Post(std::function<void()> task) {
#ifdef HARMONY_ENGINE_TEST
 task(); return;
#endif
  {
    std::lock_guard<std::mutex> lock(qmu_);
    if (stop_) return;
    queue_.push_back(std::move(task));
  }
  qcv_.notify_all();
}

void Session::PostDelayed(int ms, std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(qmu_);
    delayed_.emplace(NowMs() + ms, std::move(task));
  }
  qcv_.notify_all();
}

// Windows says when a microphone or headset arrives, leaves or becomes the
// default, so the device list is read only then. Reading it through
// libwebrtc leaks every device it lists (its loop reuses a ComPtr with
// GetAddressOf, core_audio_utility_win.cc:508); polling it every second kept
// ~11 registry keys per second open for as long as one sat in a room (W10).
class Session::DeviceWatch final : public IMMNotificationClient {
 public:
  static DeviceWatch* Start() {
    auto watch = new DeviceWatch;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&watch->enumerator_))) ||
        FAILED(watch->enumerator_->RegisterEndpointNotificationCallback(watch))) {
      logf("voice device watch unavailable; polling the device list");
      watch->Stop(); return nullptr;
    }
    return watch;
  }
  void Stop() {
    if (enumerator_) { enumerator_->UnregisterEndpointNotificationCallback(this); enumerator_->Release(); enumerator_ = nullptr; }
    Release();
  }
  bool Take() { return changed_.exchange(false); }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Mark(); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Mark(); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Mark(); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Mark(); }
  // Volume and format changes arrive here many times; none changes the list.
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  HRESULT Mark() { changed_ = true; return S_OK; }
  std::atomic<ULONG> refs_{1};
  std::atomic<bool> changed_{false};
  IMMDeviceEnumerator* enumerator_ = nullptr;
};

void Session::Loop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  perf::NameThread(L"harmony-voice");
  // The simulated device of the engine tests changes without Windows knowing.
  if (!options_.simulated_device) device_watch_ = DeviceWatch::Start();
  std::unique_lock<std::mutex> lock(qmu_);
  while (!stop_) {
    int64_t now = NowMs();
    while (!delayed_.empty() && delayed_.begin()->first <= now) {
      queue_.push_back(std::move(delayed_.begin()->second));
      delayed_.erase(delayed_.begin());
    }
    if (queue_.empty()) {
      if (delayed_.empty())
        qcv_.wait(lock);
      else
        qcv_.wait_for(lock, std::chrono::milliseconds(delayed_.begin()->first - now));
      continue;
    }
    auto task = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    task();
    lock.lock();
  }
  if (device_watch_) device_watch_->Stop();
  device_watch_ = nullptr;
  CoUninitialize();
}

// HARMONY_BWE_LOG=1: libwebrtc's own words about probing and congestion,
// copied into our log. Off by default — every INFO line is formatted before
// the filter can drop it.
namespace {
struct BweLog : webrtc::LogSink {
  void OnLogMessage(const std::string& m) override {
    for (const char* k : {"robing", "robe ", "ushback", "ALR", "verus", "bwe", "BWE", "target_rate", "Bitrate"})
      if (m.find(k) != std::string::npos) {
        auto line = m; while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        logf("webrtc %s", line.c_str()); return;
      }
  }
};
void StartBweLog() {
  static bool started = false;
  if (started) return;
  char v[4]{}; if (GetEnvironmentVariableA("HARMONY_BWE_LOG", v, sizeof v) == 0 || v[0] != '1') return;
  started = true; static BweLog sink; webrtc::LogMessage::AddLogToStream(&sink, webrtc::LS_INFO);
}
}

bool Session::CreateFactory() {
  StartBweLog();
  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  worker_thread_ = webrtc::Thread::Create();
  signaling_thread_ = webrtc::Thread::Create();
  network_thread_->SetName("harmony-network", nullptr);
  worker_thread_->SetName("harmony-worker", nullptr);
  signaling_thread_->SetName("harmony-signaling", nullptr);
  network_thread_->Start();
  worker_thread_->Start();
  signaling_thread_->Start();
#ifdef HARMONY_RESOURCE_MEASUREMENT
  resource_meter::Mark("factory-threads-started");
#endif
  // Register libwebrtc's own threads with the meter, so the panel can say how
  // much of the processor each one took.
  logf("voice engine threads up");
  network_thread_->BlockingCall([] { perf::NameThread(L"harmony-network"); });
  worker_thread_->BlockingCall([] { perf::NameThread(L"harmony-worker"); });
  signaling_thread_->BlockingCall([] { perf::NameThread(L"harmony-signaling"); });
  logf("voice engine threads named");
#if HARMONY_GPU
  auto encoders = gpu::CreateEncoderFactory();
  auto decoders = gpu::CreateDecoderFactory();
#else
  auto encoders = webrtc::CreateBuiltinVideoEncoderFactory();
  auto decoders = webrtc::CreateBuiltinVideoDecoderFactory();
#ifdef HARMONY_ENGINE_TEST
  if(engine_test::h264Formats){encoders=std::make_unique<EngineFormats<webrtc::VideoEncoderFactory>>(std::move(encoders));decoders=std::make_unique<EngineFormats<webrtc::VideoDecoderFactory>>(std::move(decoders));}
#endif
#endif
  // Our own audio pipeline (audio.h): the Windows device and the engine's
  // echo canceller as before, but the microphone goes to its own track only,
  // so the screen's sound track carries the film and nothing else.
  audio_ = worker_thread_->BlockingCall([this] { return audio::CreatePipeline(options_.test_tone,{},options_.simulated_device && !client_->secure() && client_->allowed(),options_.test_require_cable && !client_->secure() && client_->allowed()); });
  if (!audio_.device) return false;
  UpdateAudience();
  audio_.mixer->Gains({},audio::ReceiveGain({},"",options_.deafened));
#ifndef HARMONY_ENGINE_TEST
  audio_.mixer->OnUnknown([this](uint32_t ssrc) {
    logf("voice receive unrouted ssrc=%u, rebuilding routes",ssrc);
    Post([this] { if(pc_) ApplyVolumes(); });
  });
#endif
  audio_.filter->echo=options_.echo; audio_.filter->noise=options_.noise; audio_.filter->agc=options_.agc;
  audio_.filter->micGain=options_.micGain;
  audio_.filter->enabled=options_.dfn; audio_.filter->strength=options_.strength;
  ApplySidetone();
  // Each H.264 simulcast stream has its own reference chain. Without this
  // factory-scoped trial, the pinned libwebrtc turns one SSRC's PLI into
  // three simultaneous IDRs, including the layer the viewer is leaving.
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(), worker_thread_.get(), signaling_thread_.get(), audio_.device,
      webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(), std::move(encoders),
      std::move(decoders), audio_.mixer, audio_.engineProcessing, nullptr,
      std::make_unique<webrtc::FieldTrials>("WebRTC-Video-PerSsrcKeyframes/Enabled/"));
#ifdef HARMONY_RESOURCE_MEASUREMENT
  resource_meter::Mark("factory-created");
#endif
  logf("voice engine %s", factory_ ? "created" : "FAILED");
  // A test harmony on this same PC answers from 127.0.0.1, an address
  // libwebrtc ignores by default (the browser needs
  // --allow-loopback-in-peer-connection for the same reason). Only then.
  const std::wstring& host = client_->host();
  if (factory_ && (host == L"127.0.0.1" || host == L"localhost" || host == L"::1")) {
    webrtc::PeerConnectionFactoryInterface::Options o;
    o.network_ignore_mask = 0;
    factory_->SetOptions(o);
    logf("voice: loopback allowed (local server)");
  }
  return factory_ != nullptr;
}

bool Session::CreateConnection(const json::Value& ice_servers) {
  slot_limit_ = false;  // the server's limit is per connection
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // The server always sends its STUN/TURN list; an empty list on purpose is
  // the 18/09 lesson (a single path, and a hidden host candidate kills it).
  for (const auto& s : ice_servers.Elements()) {
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
  logf("voice ice servers: %zu", config.servers.size());
  webrtc::PeerConnectionDependencies deps(this);
  deps.video_bitrate_allocator_factory = std::make_unique<media::ScreenBitrateAllocatorFactory>();
  auto result = factory_->CreatePeerConnectionOrError(config, std::move(deps));
  if (!result.ok()) {
    logf("voice connection failed: %s", result.error().message());
    return false;
  }
  pc_ = result.MoveValue();
  return true;
}

bool Session::Start(std::string* error) {
  std::promise<bool> done;
  auto result = done.get_future();
  Post([this, error, &done] {
    Update opening{Update::Step};opening.reason="Abrindo o microfone?";Emit(std::move(opening));
    bool ok = CreateFactory() && Join(error);
    if (ok) {
      own_meter_ = std::make_unique<Meter>();
      audio_.mic->AddSink(own_meter_.get());
#ifndef HARMONY_PUBLISH
      if (options_.test_tone && !options_.simulated_device) TestTone();
#endif
      flow_ = webrtc::make_ref_counted<Flow>();
      Tick();
      // OS callback only queues work; teardown cancels it before joining.
      HANDLE notification = nullptr;
      if (NotifyIpInterfaceChange(AF_UNSPEC, [](void* context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        static_cast<Session*>(context)->Recover("network");
      }, this, FALSE, &notification) == NO_ERROR) network_notification_ = notification;
    }
    done.set_value(ok);
  });
  return result.get();
}

bool Session::Join(std::string* error) {
#ifdef HARMONY_ENGINE_TEST
 ++engine_test::joins; return true;
#endif
  Update entering{Update::Step};entering.reason="Entrando na sala?";Emit(std::move(entering));
  room_id_.clear();
    // The room by name, among the voice channels this account belongs to.
    auto channels = client_->Get("/api/channels");
    if (channels.status != 200) {
      *error = "channels " + std::to_string(channels.status);
      return false;
    }
    auto parsed = channels.Json();
    logf("voice channels: %zu bytes, %zu channels", channels.body.size(), parsed["channels"].Size());
    for (const auto& c : parsed["channels"].Elements()) {
      if (c["kind"].Str() != "voice") continue;
      if (!options_.room_id.empty() ? c["id"].Str() == options_.room_id : (options_.room.empty() || c["name"].Str() == options_.room)) {
        room_id_ = c["id"].Str();
        break;
      }
    }
    if (room_id_.empty()) {
      *error = "no voice room named '" + options_.room + "'";
      return false;
    }
    json::Value body = json::Value::Object();
    json::Value publish = json::Value::Array();
    publish.Push("mic");
    body["publish"] = publish;
    auto joined = SessionPost("/api/voice/" + room_id_ + "/join", body);
    if (joined.status != 200) {
      *error = "join " + std::to_string(joined.status) + " " + joined.Json()["error"].Str();
      return false;
    }
    Update negotiating{Update::Step};negotiating.reason="Ligando o ?udio?";Emit(std::move(negotiating));
    json::Value offer = joined.Json();
    {
      std::lock_guard<std::mutex> lock(mu_);
      session_id_ = offer["session_id"].Str();
    }
    logf("voice joined room");
    ++session_generation_;
    if (!CreateConnection(offer["ice_servers"])) {
      *error = "connection";
      return false;
    }
    if (options_.mic) {
      mic_ = factory_->CreateAudioTrack("mic", audio_.mic.get());
    }
    if (mic_) mic_->set_enabled(!options_.muted && !options_.deafened);
    ApplyOffer(offer);
    SendState();
    SyncDevices();
    if (!options_.watch.empty()) {
      json::Value w = json::Value::Object();
      w["session_id"] = session_id();
      w["username"] = options_.watch;
      w["watching"] = true;
      auto r = SessionPost("/api/voice/watch", w);
      logf("voice watch %s -> %d", options_.watch.c_str(), r.status);
    }
    RestoreOutgoingMedia();
    return true;
}

void Session::ApplyOffer(const json::Value& offer) {
#ifdef HARMONY_ENGINE_TEST
 ++engine_test::offers; if (!engine_test::applyOffers) return;
#endif
  static perf::Stage& stage = perf::Get("signal.offer");
  auto timer = stage.Begin();
  if (!pc_) return;
  if (offer["session_id"].Str() != session_id()) return;
  // A replay on stable signalling must resend the accepted answer.
  if (applied_offer_ == offer["sdp"].Str() && pc_->signaling_state() == webrtc::PeerConnectionInterface::kStable) {
    auto body = json::Value::Object(); body["session_id"] = session_id(); body["sdp"] = applied_answer_;
    SessionPost("/api/voice/answer", body); return;
  }
  tracks_.clear(); send_mids_.clear();
  // Who is in which slot, before the description goes in.
  for (const auto& t : offer["tracks"].Elements())
    tracks_[t["mid"].Str()] = {t["username"].Str(), t["source"].Str(), t["kind"].Str()};
  UpdateAudience();
  for (const auto& [source, mid] : offer["send_mids"].Items()) send_mids_[source] = mid.Str();

  if (!SetRemote(pc_.get(), webrtc::SdpType::kOffer, offer["sdp"].Str())) return;

  // A slot made by a remote offer is born recvonly, and a recvonly slot with a
  // track inside answers "I send nothing" — silent, no error (media.ts:1561).
  // sendrecv and not sendonly: the server puts the others' sound in it too.
  for (const auto& [source, mid] : send_mids_) {
    auto outgoing=media::Outgoing<webrtc::MediaStreamTrackInterface>(source,mic_.get(),camera_.get(),screen_.get(),screen_audio_.get());
    if (!outgoing) continue;
    for (auto& tr : pc_->GetTransceivers()) {
      if (tr->mid() != mid) continue;
      tr->sender()->SetTrack(outgoing);
      if(source=="screen_audio") screen_audio_tr_=tr;
      tr->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
    }
  }
  std::string answer = Create(pc_.get(), false);
  if (answer.empty()) return;
  if (!SetLocal(pc_.get(), webrtc::SdpType::kAnswer, answer)) return;
  json::Value body = json::Value::Object();
  body["session_id"] = session_id();
  body["sdp"] = answer;
  auto r = SessionPost("/api/voice/answer", body);
  if (r.status != 204) logf("voice answer -> %d", r.status);
  if (r.status == 204) { applied_offer_ = offer["sdp"].Str(); applied_answer_ = answer; }
  TuneMic();
  TuneScreen();
  for(auto& tr:pc_->GetTransceivers()) if(tr->mid()==send_mids_["cam"] && camera_) {
    auto p=tr->sender()->GetParameters(); if(!p.encodings.empty()) {p.encodings[0].max_bitrate_bps=900000; p.encodings[0].max_framerate=30; p.degradation_preference=webrtc::DegradationPreference::BALANCED; tr->sender()->SetParameters(p);}
  }
  BindAudio();
  AttachWatched();
  AttachVideos();
  timer.Done();
}

void Session::TuneMic() {
  auto it = send_mids_.find("mic");
  if (it == send_mids_.end()) return;
  for (auto& tr : pc_->GetTransceivers()) {
    if (tr->mid() != it->second) continue;
    auto params = tr->sender()->GetParameters();
    if (params.encodings.empty()) return;
    // The voice ahead of the picture when the pipe fills: the screen of this
    // person shares the same connection and congestion control
    // (media.ts tune(), the 23% voice loss of 19/09/2026). Chrome's "high".
    if (params.encodings[0].bitrate_priority == 4.0) return;
    params.encodings[0].bitrate_priority = 4.0;
    auto err = tr->sender()->SetParameters(params);
    if (!err.ok()) logf("voice mic priority: %s", err.message());
  }
}

void Session::RestoreOutgoingMedia() {
  if (options_.share_window || options_.share_monitor) ShareScreen();
  if (camera_wanted_) SetCamera(true,camera_id_);
}
void Session::ShareScreen() {
#if HARMONY_GPU || defined(HARMONY_ENGINE_TEST)
  if(!pc_) return;
  // Past the limit, later offers only grew until the server refused them as
  // malformed (W10e: 400 from the 20th share on); say the same thing at once.
  if(slot_limit_) {
    options_.share_window=active_screen_options_.share_window;options_.share_monitor=active_screen_options_.share_monitor;
    ScreenState("reconnect_needed"); return;
  }
#ifdef HARMONY_ENGINE_TEST
  auto source = engine_test::screenSource;
#else
  auto source = options_.share_window ? gpu::CreateWindowSource(options_.share_window, options_.share_fps,options_.share_height) : gpu::CreateMonitorSource(options_.share_monitor,options_.share_fps,options_.share_height);
#endif
  logf("screen open step=source_created first_frame_deadline_ms=3000");
  pending_screen_source_=source;pending_screen_options_=options_;pending_screen_deadline_=NowMs()+3000;
  options_.share_window=active_screen_options_.share_window;options_.share_monitor=active_screen_options_.share_monitor;
  options_.share_height=active_screen_options_.share_height;options_.share_fps=active_screen_options_.share_fps;options_.share_audio=active_screen_options_.share_audio;
  ScreenState("capture_starting");
#endif
}
void Session::CommitScreen(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source) {
#if HARMONY_GPU || defined(HARMONY_ENGINE_TEST)
  if (!pc_||!source||source->state()!=webrtc::MediaSourceInterface::kLive) return;
  active_screen_options_=options_;
  ++screen_offer_generation_;
  screen_ = factory_->CreateVideoTrack(source, "screen");
  screen_capture_=source.get();
  logf("screen open step=track_created success=%d",bool(screen_));
  if(!screen_) {ScreenState("capture_unavailable");return;}
  transmission_.Choose(bool(screen_));
  // A game and a film, not code (CLAUDE.md, 19/09/2026): motion.
  screen_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);
  if(screen_tr_) {
    screen_tr_->sender()->SetTrack(screen_.get());
    if(screen_audio_tr_) screen_audio_tr_->sender()->SetTrack(nullptr);
    DetachEverywhere(screen_audio_.get());
    system_audio_=nullptr; screen_audio_=nullptr;
    if(options_.share_audio) {
      DWORD pid=0; if(options_.share_window) GetWindowThreadProcessId(options_.share_window,&pid);
#ifdef HARMONY_ENGINE_TEST
      system_audio_=engine_test::systemAudio;
#else
      system_audio_=loopback::SystemAudioSource::Create(pid, true);
#endif
      if(system_audio_) screen_audio_=factory_->CreateAudioTrack("screen_audio",system_audio_.get());
    }
    if(screen_audio_ && !screen_audio_tr_) {
      webrtc::RtpTransceiverInit init; init.direction=webrtc::RtpTransceiverDirection::kSendOnly; init.stream_ids={"screen"};
      auto added=pc_->AddTransceiver(screen_audio_,init);
      if(!added.ok()) {screen_audio_=nullptr; system_audio_=nullptr; ScreenState("screen_audio_unavailable"); return;}
      screen_audio_tr_=added.MoveValue(); screen_offered_=false;
      sync_probe::AttachRtpProbe(screen_audio_tr_->sender().get(),true);
    }
    if(screen_audio_tr_) screen_audio_tr_->sender()->SetTrack(screen_audio_.get());
    auto body=json::Value::Object(); body["session_id"]=session_id(); body["sources"]=json::Value::Array(); body["sources"].Push("screen"); if(screen_audio_) body["sources"].Push("screen_audio");
    if(screen_offered_) {
      if(!screen_audio_) {auto silent=json::Value::Object();silent["session_id"]=session_id();silent["sources"]=json::Value::Array();silent["sources"].Push("screen_audio");SessionPost("/api/voice/unpublish",silent);}
      SessionPost("/api/voice/publish",body);TuneScreen(); ScreenState("transmitting");
    }
    else OfferScreen(1);
    return;
  }

  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
  init.stream_ids = {"screen"};
  for (const auto& l : kLayers) {
    webrtc::RtpEncodingParameters e;
    e.rid = l.rid;
    e.scale_resolution_down_by = l.scale;
    e.max_bitrate_bps = l.max_bps;
    e.max_framerate = std::min<double>(l.fps, options_.share_fps);
    init.send_encodings.push_back(e);
  }
  auto added = pc_->AddTransceiver(screen_, init);
  if (!added.ok()) {
    logf("voice screen transceiver: %s", added.error().message());
    return;
  }
  auto tr = added.MoveValue();
  sync_probe::AttachRtpProbe(tr->sender().get(),false);
  // The owner's codec decision: H.264 first (19/09/2026), then VP9 and VP8.
  auto caps = factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
  auto rank = [](const webrtc::RtpCodecCapability& c) {
    if (c.name == "H264") return 0;
    if (c.name == "VP9") return 1;
    if (c.name == "VP8") return 2;
    return 3;
  };
  std::stable_sort(caps.codecs.begin(), caps.codecs.end(),
                   [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
  caps.codecs.erase(std::remove_if(caps.codecs.begin(), caps.codecs.end(),
                                   [](const auto& c) { return c.name == "AV1"; }),
                    caps.codecs.end());
  auto pref = tr->SetCodecPreferences(caps.codecs);
  if (!pref.ok()) logf("voice screen codec preferences: %s", pref.message());

  // The film's own sound in the SAME offer as the picture: two offers in a
  // row is what the server's negotiation lock exists to prevent (sfu.go
  // ClientOffer). Without process loopback (Windows 10) the screen goes alone.
  webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> audio_tr;
  if (options_.share_audio) {
    DWORD pid=0; if(options_.share_window) GetWindowThreadProcessId(options_.share_window,&pid);
#ifdef HARMONY_ENGINE_TEST
    system_audio_ = engine_test::systemAudio;
#else
    system_audio_ = loopback::SystemAudioSource::Create(pid, true);
#endif
    if (system_audio_) {
      screen_audio_ = factory_->CreateAudioTrack("screen_audio", system_audio_.get());
      webrtc::RtpTransceiverInit ainit;
      ainit.direction = webrtc::RtpTransceiverDirection::kSendOnly;
      ainit.stream_ids = {"screen"};
      auto a = pc_->AddTransceiver(screen_audio_, ainit);
      if (a.ok()) { audio_tr = a.MoveValue(); sync_probe::AttachRtpProbe(audio_tr->sender().get(),true); }
      else logf("voice screen audio transceiver: %s", a.error().message());
    }
  }

  screen_tr_ = tr;
  screen_audio_tr_ = audio_tr;
  OfferScreen(1);
#else
  logf("voice screen: built without the GPU module");
#endif
}

void Session::SetCaptureRate(bool encoding) {
  if(!screen_) return;
  // Match PanelRenderer's refresh-derived interval without adding a sink cap:
  // VideoBroadcaster aggregates max_framerate_fps by minimum, not maximum.
  HDC dc=GetDC(nullptr);int hz=GetDeviceCaps(dc,VREFRESH);ReleaseDC(nullptr,dc);
  webrtc::VideoTrackSourceConstraints constraints;
  constraints.max_fps=encoding ? options_.share_fps : std::min(options_.share_fps,std::clamp(hz,1,30));
  screen_->GetSource()->ProcessConstraints(constraints);
}
void Session::TuneScreen() {
  auto it = send_mids_.find("screen");
  if (it == send_mids_.end()) return;
  for (auto& tr : pc_->GetTransceivers()) {
    if (tr->mid() != it->second) continue;
    auto params = tr->sender()->GetParameters();
    for (size_t i = 0; i < params.encodings.size(); ++i) {
      auto& e = params.encodings[i];
      auto layer = std::find_if(std::begin(kLayers), std::end(kLayers), [&](const auto& l) { return e.rid == l.rid; });
      if (layer == std::end(kLayers)) { logf("screen unknown RID at index=%zu",i); continue; }
      e.max_bitrate_bps = layer == std::begin(kLayers) ? media::ScreenCeiling(options_.share_height,options_.share_fps) : layer->max_bps;
      e.scale_resolution_down_by = layer->scale;
      e.max_framerate = std::min<double>(layer->fps, options_.share_fps);
      e.active = layers_on_.count(layer->rid) > 0;
      logf("screen sender index=%zu rid=%s scale=%.1f fps=%.0f active=%d ssrc=%u", i,layer->rid,layer->scale,*e.max_framerate,e.active,e.ssrc.value_or(0));
    }
    // libwebrtc implements priority per sender, on the first encoding only.
    if (!params.encodings.empty()) params.encodings.front().bitrate_priority = media::kScreenBitratePriority;
    // Keep the frames, let the size fall (CLAUDE.md: maintain-framerate).
    params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
    const bool encoding=std::any_of(params.encodings.begin(),params.encodings.end(),[](const auto& e){return e.active;});
    // Restore capture BEFORE the encoder can request its first full-rate frame.
    if(encoding) SetCaptureRate(true);
    auto err = tr->sender()->SetParameters(params);
    if (!err.ok()) logf("voice screen parameters: %s", err.message());
    else if(!encoding) SetCaptureRate(false);
  }
}

// voice_watchers: which of my layers someone is watching. Layers nobody
// needs are switched off, after 5 s, because switching back costs a full
// picture (media.ts screenWatchers).
void Session::ApplyWatchers(const json::Value& data) {
  std::set<std::string> keep;
  if (data["screen"].Bool()) {
    const auto& layers = data["layers"];
    if (layers.IsArray() && layers.Size() > 0) {
      for (const auto& l : layers.Elements()) keep.insert(l.Str());
    } else {
      for (const auto& l : kLayers) keep.insert(l.rid);
    }
  }
  std::string list;
  for (const auto& k : keep) list += k + " ";
  logf("voice watchers: %s", list.empty() ? "nobody" : list.c_str());
  // The server's "nobody" for a screen that just stopped lands after the stop
  // reset the layers; acted on, its switch-off fired five seconds later on the
  // next share, which then sent nothing, so the server never saw it and never
  // asked for a layer (W10f: 6 of 15 shares). Without a screen there is
  // nothing to switch; each share starts with every layer on.
  if (!screen_) { ++shrink_generation_; return; }
  bool shrinking = std::any_of(layers_on_.begin(), layers_on_.end(), [&](const std::string& l) { return !keep.count(l); });
  std::set<std::string> grow = layers_on_;
  grow.insert(keep.begin(), keep.end());
  if (grow != layers_on_) SetLayersActive(grow);
  int64_t gen = ++shrink_generation_;
  if (shrinking) {
    PostDelayed(kShrinkWaitMs, [this, gen, keep] {
      if (gen == shrink_generation_) SetLayersActive(keep);
    });
  }
}

void Session::SetLayersActive(const std::set<std::string>& keep) {
  layers_on_ = keep;
  if (screen_offered_) TuneScreen();
}

void Session::AttachWatched() {
  if (options_.watch.empty() || !options_.on_watch) return;
  webrtc::VideoTrackInterface* found = nullptr;
  for (auto& tr : pc_->GetTransceivers()) {
    auto mid = tr->mid();
    if (!mid) continue;
    auto it = tracks_.find(*mid);
    if (it == tracks_.end() || it->second.username != options_.watch || it->second.source != options_.watch_source) continue;
    auto dir = tr->current_direction();
    if (!dir || (*dir != webrtc::RtpTransceiverDirection::kRecvOnly &&
                 *dir != webrtc::RtpTransceiverDirection::kSendRecv))
      continue;
    auto track = tr->receiver()->track();
    if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind)
      found = static_cast<webrtc::VideoTrackInterface*>(track.get());
  }
  if (found == watched_) return;
  watched_ = found;
  logf("voice watching %s: %s", options_.watch.c_str(), found ? "picture arrived" : "no picture");
  options_.on_watch(found);

}

// The screen is the one thing this device offers: Pion cannot ask for three
// layers, so the layers have to be named in our own offer (voice.go
// handleVoiceOffer). Three tries, like the web — each one a task of its own:
// the server refuses while its own offer is pending, and that offer (a
// voice_offer on the socket) waits in this same queue. Sleeping between tries
// inside one task kept it waiting until the tries ran out (Codex audit,
// 06/10/2026).
void Session::OfferScreen(int attempt) {
  auto tr = screen_tr_;
  auto audio_tr = screen_audio_tr_;
  if (!pc_ || !screen_ || !tr || screen_offered_) return;
  auto generation = screen_offer_generation_;
  auto retry = [this, attempt, generation](const char* why) {
    if (attempt >= 3) {
      logf("voice screen: the layered offer did not go through (%s)", why);
      StopScreenNow("screen_offer_failed");
      return;
    }
    PostDelayed(300 * attempt, [this, attempt, generation] { if (generation == screen_offer_generation_ && screen_) OfferScreen(attempt + 1); });
  };
  if (pc_->signaling_state() != webrtc::PeerConnectionInterface::kStable) return retry("not stable");
  std::string offer = Create(pc_.get(), true);
  if (offer.empty() || !SetLocal(pc_.get(), webrtc::SdpType::kOffer, offer)) return retry("local offer");
  json::Value body = json::Value::Object();
  body["session_id"] = session_id();
  json::Value sources = json::Value::Array();
  sources.Push("screen");
  if (audio_tr) sources.Push("screen_audio");
  body["sources"] = sources;
  body["sdp"] = offer;
  logf("screen open step=offer_sent attempt=%d response_phase_deadline_ms=5000",attempt);
  auto r = SessionPost("/api/voice/offer", body);
  logf("screen open step=response status=%d",r.status);
  if (r.status != 200) {
    auto error = r.Json()["error"].Str();
    logf("voice screen offer %d/3 -> %d %s", attempt, r.status, error.c_str());
    // Take the offer back so the server's next one can land.
    SetLocal(pc_.get(), webrtc::SdpType::kRollback, "");
    // The server's slot limit on this connection does not pass with a retry;
    // the web tells the person to rejoin, and so does this (main.cpp).
    if (error == "reconnect_needed") { slot_limit_ = true; return StopScreenNow("reconnect_needed"); }
    return retry("refused");
  }
  std::string mid = tr->mid().value_or("");
  send_mids_["screen"] = mid;
  if (audio_tr) send_mids_["screen_audio"] = audio_tr->mid().value_or("");
  // No x-google-start-bitrate here. Setting a start rate on a transport that is
  // already running makes libwebrtc's probe controller fire a probe sized to
  // what is allocated at that instant — the voice alone, 80 kbit/s — and a
  // successful probe overwrites the estimate: 6 Mbit/s "start" became ~110
  // kbit/s within half a second (r21, probe_bitrate_estimator "cluster 3").
  // The call's own opening probes (0.9 and 1.8 Mbit/s) have already measured
  // the link by the time a screen is shared, and adding the screen's layers
  // raises the allocation, which probes upward on its own.
  std::string answer = r.Json()["sdp"].Str();
  if (!SetRemote(pc_.get(), webrtc::SdpType::kAnswer, answer)) return retry("remote answer");
  screen_offered_ = true;
  TuneScreen(); ScreenState("transmitting");
  logf("voice screen offered in %zu layers (mid %s)", std::size(kLayers), mid.c_str());
}

void Session::ScreenState(std::string reason) {
  logf("screen state=%s",reason.c_str());
  AttachVideos();
  Update u{Update::Media}; u.sharingScreen=screen_!=nullptr; u.sharingCamera=camera_!=nullptr; u.reason=std::move(reason); Emit(std::move(u));
}
void Session::SetCamera(bool enabled,std::string id) {
  Post([this,enabled,id=std::move(id)] {
    camera_wanted_=enabled;
    if(enabled) camera_failed_=false;
    if(!pc_) return;
    if(enabled) {
      camera_id_=id;
#ifdef HARMONY_ENGINE_TEST
      auto source=engine_test::cameraSource;
#else
      auto source=camera::Open(id);
#endif
      camera_=factory_->CreateVideoTrack(source,"cam");
      camera_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);
      for(auto& tr:pc_->GetTransceivers()) if(tr->mid()==send_mids_["cam"]) tr->sender()->SetTrack(camera_.get());
    } else {
      // The screen's lesson (StopScreenNow): the server's Stop never reaches
      // this side as a rejected slot, so a slot only emptied kept its channel
      // and encoder until leaving — ~42 MiB per toggle, 1.4 GB after 30 (W10).
      // Detach first (a stopped sender refuses SetTrack), then end our side;
      // the next camera gets the fresh slot the server opens on publish.
      for(auto& tr:DetachEverywhere(camera_.get())) if(!tr->stopped()&&!tr->stopping()) { auto error=tr->StopStandard(); if(!error.ok()) logf("camera slot stop: %s",error.message()); }
      camera_=nullptr; send_mids_.erase("cam");
    }
    auto body=json::Value::Object(); body["session_id"]=session_id(); body["sources"]=json::Value::Array(); body["sources"].Push("cam");
    auto response=SessionPost(enabled?"/api/voice/publish":"/api/voice/unpublish",body);
    ScreenState(enabled?"camera_on":camera_failed_?"camera_unavailable":"camera_off"); logf("camera enabled=%d status=%d",enabled,response.status);
  });
}
void Session::SetScreen(HWND window,HMONITOR monitor,int height,int fps,bool sound) {
  Post([this,window,monitor,height,fps,sound] {
    if(!pc_ || ((!window)==(!monitor))) return;
    DWORD pid=0; if(window) GetWindowThreadProcessId(window,&pid);
    if(window && (!IsWindow(window)||pid==GetCurrentProcessId())) { ScreenState("capture_unavailable"); return; }
    options_.share_window=window; options_.share_monitor=monitor;
    options_.share_height=height==480||height==720?height:1080;
    options_.share_fps=fps==15||fps==30?fps:60; options_.share_audio=sound;
    wchar_t path[32768]{}; DWORD count=32768; std::string executable=window?"não encontrado":"monitor";
    if(window) {
      HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
      if(process) { if(QueryFullProcessImageNameW(process,0,path,&count)) executable=net::Narrow(std::filesystem::path(path).filename().wstring()); CloseHandle(process); }
    }
    // Never obtain a window title here; titles belong only to the chooser UI.
    logf("screen choose type=%s executable=%s height=%d fps=%d sound=%d",window?"window":"monitor",executable.c_str(),options_.share_height,options_.share_fps,sound);
    ShareScreen();
  });
}
void Session::StopScreenNow(std::string reason) {
  ++screen_offer_generation_;
  pending_screen_source_=nullptr;pending_screen_deadline_=0;
  active_screen_options_.share_window=nullptr;active_screen_options_.share_monitor=nullptr;
  if(!screen_) return;
  auto held=DetachEverywhere(screen_.get()); auto sound=DetachEverywhere(screen_audio_.get());
  held.insert(held.end(),sound.begin(),sound.end());
  if(screen_tr_) held.push_back(screen_tr_);
  if(screen_audio_tr_) held.push_back(screen_audio_tr_);
  // A slot the server stops is never rejected towards this device (Pion's
  // Stop writes no port 0), so its channel, send stream and hardware encoder
  // lived until leaving the room: 12 NVENC sessions by the 15th share (W10e).
  // Stopping our side ends the channel; the tracks go first because a stopped
  // sender refuses SetTrack and would keep the capture (rtp_sender.cc:722).
  for(auto& tr:held) if(!tr->stopped()&&!tr->stopping()) { auto error=tr->StopStandard(); if(!error.ok()) logf("screen slot stop: %s",error.message()); }
  screen_=nullptr; screen_audio_=nullptr; system_audio_=nullptr;
  // A switch-off still waiting belongs to this screen; armed, it would turn
  // off the next one before its first picture (media.ts dropLocal), and a new
  // share starts with every layer on, as it did the first time (W10e: 11 of
  // 15 shares went out with all layers inactive).
  ++shrink_generation_; layers_on_={"alta","media","baixa"};
  options_.share_window=nullptr; options_.share_monitor=nullptr; transmission_.Stop();
  auto body=json::Value::Object(); body["session_id"]=session_id(); body["sources"]=json::Value::Array(); body["sources"].Push("screen"); body["sources"].Push("screen_audio");
  SessionPost("/api/voice/unpublish",body);
  // Unpublishing makes the server stop these slots, and a stopped slot never
  // sends again (sfu.go closeSlot). Keeping them made every later share land
  // in a plain one-layer slot the server opened on publish; forgetting them
  // makes the next share offer its own three layers, as the web does.
  screen_tr_=nullptr; screen_audio_tr_=nullptr; screen_offered_=false; send_mids_.erase("screen"); send_mids_.erase("screen_audio");
  ScreenState(std::move(reason)); logf("screen stopped");
}
void Session::StopScreen() { Post([this]{StopScreenNow("stopped");}); }
// The server's offer can put the screen in slots other than the one this
// device added (ApplyOffer follows its send_mids), so clearing only
// screen_tr_ left every earlier capture, loopback and encoder owned by those
// senders until leaving the room: W10 counted 12 hardware encoders alive at
// the 30th toggle and the 16th share sent no picture.
std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>> Session::DetachEverywhere(webrtc::MediaStreamTrackInterface* track) {
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>> emptied;
  if(!pc_||!track) return emptied;
  for(auto& tr:pc_->GetTransceivers()) if(tr->sender()->track().get()==track) { tr->sender()->SetTrack(nullptr); emptied.push_back(tr); }
  return emptied;
}
void Session::Watch(std::string username,std::function<void(webrtc::VideoTrackInterface*)> sink,bool camera) {
  Post([this,username=std::move(username),sink=std::move(sink),camera] {
    if(!pc_) return;
    if(options_.on_watch) options_.on_watch(nullptr); watched_=nullptr;
    auto body=json::Value::Object(); body["session_id"]=session_id();
    if(!options_.watch.empty()&&options_.watch!=username&&!tile_layers_.count(options_.watch)) { body["username"]=options_.watch; body["watching"]=false; SessionPost("/api/voice/watch",body); }
    options_.watch=username; options_.watch_source=camera?"cam":"screen"; options_.on_watch=sink; layer_request_={};
    if(username.empty()) return;
    if(camera) {AttachWatched();return;}
    body["username"]=username; body["watching"]=true;
    auto response=SessionPost("/api/voice/watch",body); logf("screen watch status=%d",response.status); AttachWatched();
  });
}
void Session::WatchSize(double width,double dpi,bool visible) { Post([this,width,dpi,visible] { layer_request_.Resize(width,dpi,visible,NowMs()); }); }
void Session::Video(std::string who,std::string source,std::function<void(webrtc::VideoTrackInterface*)> attach) {
  Post([this,who=std::move(who),source=std::move(source),attach=std::move(attach)] {
    if(who.empty()) {
      for(auto& [key,slot]:videos_) {if(slot.attach)slot.attach(nullptr);if(slot.source=="screen"&&slot.who!=options_.username&&slot.who!=options_.watch) {auto body=json::Value::Object();body["session_id"]=session_id();body["username"]=slot.who;body["watching"]=false;SessionPost("/api/voice/watch",body);}}
      videos_.clear();tile_layers_.clear();return;
    }
    auto key=who+"/"+source;auto old=videos_.find(key);if(old!=videos_.end()&&old->second.attach)old->second.attach(nullptr);
    if(!attach) {
      videos_.erase(key);
      if(source=="screen") {tile_layers_.erase(who);if(who!=options_.watch&&who!=options_.username) {auto body=json::Value::Object();body["session_id"]=session_id();body["username"]=who;body["watching"]=false;SessionPost("/api/voice/watch",body);}}
      return;
    }
    videos_[key]={who,source,attach,{}};
    if(source=="screen"&&who!=options_.username) {tile_layers_[who]={};auto body=json::Value::Object();body["session_id"]=session_id();body["username"]=who;body["watching"]=true;SessionPost("/api/voice/watch",body);}
    AttachVideos();
  });
}
// A negative width says "nobody is looking, keep the size": the tile left the
// screen (a text channel is being read) but may be back in a moment.
void Session::VideoSize(std::string who,double width,double dpi,bool visible) {Post([this,who=std::move(who),width,dpi,visible]{
  auto tile=tile_layers_.find(who);
  if(width<0) {if(tile!=tile_layers_.end())tile->second.Look(false,NowMs());return;}
  tile_layers_[who].Resize(width,dpi,visible,NowMs());});}
void Session::AttachVideos() {
  if(!pc_)return;
  for(auto& [key,slot]:videos_) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> found;
    if(slot.who==options_.username) found=slot.source=="screen"?screen_:camera_;
    else for(auto& tr:pc_->GetTransceivers()) {
      auto it=tracks_.find(tr->mid().value_or(""));auto direction=tr->current_direction();
      if(it==tracks_.end()||it->second.username!=slot.who||it->second.source!=slot.source||!direction||(*direction!=webrtc::RtpTransceiverDirection::kRecvOnly&&*direction!=webrtc::RtpTransceiverDirection::kSendRecv))continue;
      auto track=tr->receiver()->track();if(track&&track->kind()==webrtc::MediaStreamTrackInterface::kVideoKind)found=static_cast<webrtc::VideoTrackInterface*>(track.get());
    }
    if(found!=slot.track) {if(slot.attach)slot.attach(found.get());slot.track=found;}
  }
}

void Session::OnSocketEvent(const std::string& type, const json::Value& data) {
  if (type == "voice_offer") {
    Post([this, data] { ApplyOffer(data); });
  } else if (type == "voice_watchers") {
    Post([this, data] { ApplyWatchers(data); });
  } else if (type == "voice_room") {
    size_t n = data["peers"].Size();
    Post([n] { logf("voice room: %zu inside", n); });
  }
}

void Session::OnSocketOpen() {
  // Events while the socket was closed are lost, a voice_offer included
  // (PROTOCOL-HARMONY §2). The recovery route is W3's; W1 only says so.
  Recover("gateway");
}

void Session::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) {
  logf("voice ice gathering %d", int(state));
}

void Session::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  json::Value body = json::Value::Object();
  body["candidate"] = candidate->ToString();
  body["sdp_mid"] = candidate->sdp_mid();
  body["sdp_mline_index"] = candidate->sdp_mline_index();
  Post([this, body]() mutable {
    body["session_id"] = session_id();
    SessionPost("/api/voice/candidate", body);
  });
}

void Session::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  logf("voice connection %d", int(state));
  Post([this, state] {
    connected_ = state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
    if (wanted_ && (state == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected ||
                   state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed)) Recover("ice");
  });
}

void Session::Leave() {
  std::promise<void> done;
  auto f = done.get_future();
  Post([this, &done] {
    wanted_ = false; ++recovery_generation_;
    if (!session_id().empty()) {
      json::Value body = json::Value::Object();
      body["session_id"] = session_id();
      SessionPost("/api/voice/leave", body);
      logf("voice left");
    }
    CloseMedia();
    { std::lock_guard lock(mu_); session_id_.clear(); }
    done.set_value();
  });
  f.wait();
}

void Session::Emit(Update update) {
  update.connectionIdentity=reinterpret_cast<uintptr_t>(pc_.get()); update.sessionIdentity=std::hash<std::string>{}(session_id());
  update.at = NowMs(); if(update.kind!=Update::Render) update.generation = session_generation_;
  if (options_.changed) options_.changed(std::move(update));
}
void Session::CloseMedia() {
  if (audio_.audience) *audio_.audience=false;
  for(auto& [key,slot]:videos_) {if(slot.attach)slot.attach(nullptr);slot.track=nullptr;}
  if(options_.on_watch) options_.on_watch(nullptr); watched_=nullptr;
  ++screen_offer_generation_;pending_screen_source_=nullptr;pending_screen_deadline_=0;
  screen_=nullptr; screen_audio_=nullptr; system_audio_=nullptr; screen_tr_=nullptr; screen_audio_tr_=nullptr; screen_offered_=false;
  camera_=nullptr;transmission_.Stop();
  for (auto& [mid, r] : reception_) r.track->RemoveSink(r.meter.get());
  reception_.clear();
  if (pc_) pc_->Close(); pc_ = nullptr;
  connected_ = false; applied_offer_.clear(); applied_answer_.clear(); tracks_.clear(); send_mids_.clear();
}
void Session::UpdateAudience() {
  // The SFU offer names remote participants' tracks even while they are muted.
  // Do not infer audience from PCM/levels: a silent listener still needs DFN.
  // Send slots are separate (send_mids_), so they never count as a listener.
  // A muted or deafened microphone has no listener either: the filter's
  // output would be thrown away a hundred times a second. Unmuting passes
  // through the same 200 ms of probes as somebody arriving in the room, with
  // libwebrtc's suppressor covering them.
  if (audio_.audience) *audio_.audience=!options_.muted && !options_.deafened && std::any_of(tracks_.begin(),tracks_.end(),[this](const auto& entry) {
    return !entry.second.username.empty() && entry.second.username!=options_.username;
  });
}
void Session::BindAudio() {
  if (!pc_) return;
  std::set<std::string> keep;
  for (auto& tr : pc_->GetTransceivers()) {
    auto mid = tr->mid().value_or(""); auto info = tracks_.find(mid);
    auto media = tr->receiver()->track();
    if (!media || media->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) continue;
    auto track = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(static_cast<webrtc::AudioTrackInterface*>(media.get()));
    // The send slot also owns an unbound receive source. The web connects
    // only tracks named by the offer; do not render an unnamed receiver.
    if (info == tracks_.end() || info->second.kind != "audio") {
      signaling_thread_->BlockingCall([&] { track->GetSource()->SetVolume(0); }); continue;
    }
    keep.insert(mid);
    auto previous = reception_.find(mid);
    auto volumeKey = info->second.source == "screen_audio" ? info->second.username + "/screen_audio" : info->second.username;
    if (previous != reception_.end() && previous->second.track == track && previous->second.username == info->second.username && previous->second.volume_key == volumeKey) continue;
    if (previous != reception_.end()) { previous->second.track->RemoveSink(previous->second.meter.get()); reception_.erase(previous); }
    Reception r; r.track = track; r.username = info->second.username; r.volume_key = volumeKey; r.meter = std::make_unique<Meter>();
    track->AddSink(r.meter.get()); reception_.emplace(mid, std::move(r));
  }
  for (auto it = reception_.begin(); it != reception_.end();) {
    if (!keep.count(it->first)) { it->second.track->RemoveSink(it->second.meter.get()); it = reception_.erase(it); }
    else ++it;
  }
  ApplyVolumes();
}
void Session::ApplyVolumes() {
  std::map<uint32_t,double> gains;
  for (auto& [mid, r] : reception_) {
    // Scale the decoded source at the existing mixer boundary. Both render
    // and the microphone reverse reference see the same post-gain mix.
    double volume = audio::ReceiveGain(options_.volumes,r.volume_key,options_.deafened);
    for(auto& transceiver:pc_->GetTransceivers()) if(transceiver->mid()==mid) {
      for(auto& encoding:transceiver->receiver()->GetParameters().encodings)
        if(encoding.ssrc) gains[*encoding.ssrc]=volume;
    }
    // Source volume is not our gain: the pinned build did not reliably mute
    // render through that observer. The mixer adapter is authoritative.
    signaling_thread_->BlockingCall([&] { r.track->GetSource()->SetVolume(1); });
    logf("voice receive channel gain %.2f", volume);
  }
  // Receivers the offer does not name (BindAudio) stay silent by route now
  // that an SSRC missing from the routes plays at the ordinary volume.
  if(pc_) for(auto& transceiver:pc_->GetTransceivers()) {
    auto media=transceiver->receiver()->track();
    if(!media||media->kind()!=webrtc::MediaStreamTrackInterface::kAudioKind||reception_.count(transceiver->mid().value_or(""))) continue;
    for(auto& encoding:transceiver->receiver()->GetParameters().encodings) if(encoding.ssrc) gains.emplace(*encoding.ssrc,0);
  }
  logf("voice receive routes %zu",gains.size());
  if(audio_.mixer) audio_.mixer->Gains(std::move(gains),audio::ReceiveGain({},"",options_.deafened));
}
void Session::SetVolume(std::string who, double gain) {
  Post([this, who = std::move(who), gain] { options_.volumes[who] = std::clamp(gain, 0.0, 2.0); ApplyVolumes(); });
}
net::Response Session::SessionPost(const std::string& path,const json::Value& body) {
#ifdef HARMONY_ENGINE_TEST
  return engine_test::post(path,body);
#else
  return client_->Post(path,body);
#endif
}
void Session::SendState() {
  if (session_id().empty()) return;
  auto body = json::Value::Object(); body["session_id"] = session_id();
  body["muted"] = options_.muted; body["deafened"] = options_.deafened;
  auto response = SessionPost("/api/voice/state", body);
  logf("voice state muted=%d deafened=%d status=%d", options_.muted, options_.deafened, response.status);
  if (!response.status || response.status >= 500) PostDelayed(500, [this] { if (wanted_) SendState(); });
}
void Session::SetMute(bool muted, bool deafened) {
  Post([this, muted, deafened] {
    options_.muted = muted; options_.deafened = deafened;
    if (mic_) mic_->set_enabled(!muted && !deafened);
    UpdateAudience();
    ApplySidetone(); ApplyVolumes(); SendState();
  });
}
void Session::ChooseDevice(bool capture, std::string id) {
#ifndef HARMONY_PUBLISH
  if(options_.simulated_device && !client_->secure() && client_->allowed() && id=="test-missing-endpoint") {
    // Force enumeration to remove A as well, rather than merely choosing an unknown ID.
    worker_thread_->BlockingCall([&] { if(audio_.removeTestDevice) audio_.removeTestDevice(); (capture ? inputs_ : outputs_).clear(); });
  }
#endif
  auto& devices = capture ? inputs_ : outputs_;
  auto found = std::find_if(devices.begin(), devices.end(), [&](auto& d) { return d.id == id; });
  if (!id.empty() && found == devices.end()) { id.clear(); logf("voice device disappeared; fallback to Windows default"); }
  auto& recovery=capture ? capture_recovery_ : playout_recovery_;
  recovery.baseline=(capture ? audio_.captureCalls : audio_.renderCalls)->load();
  // Where Windows lists it, which is where the ADM numbers it; one name from
  // the ADM confirms that before it is trusted, instead of walking all of them.
  int listed = -1;
  if (!id.empty() && !options_.simulated_device) {
    auto now = Endpoints(capture);
    auto at = std::find_if(now.begin(), now.end(), [&](auto& d) { return d.id == id; });
    if (at != now.end()) listed = int(at - now.begin());
  }
  bool success = worker_thread_->BlockingCall([&] {
    auto device = audio_.device; bool active = capture ? capture_wanted_ : playout_wanted_;
    capture ? device->StopRecording() : device->StopPlayout();
    auto choose = [&](bool useDefault) {
      if (useDefault) return capture ? device->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice) : device->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
      if (listed >= 0) {
        char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
        auto status = capture ? device->RecordingDeviceName(uint16_t(listed),name,guid) : device->PlayoutDeviceName(uint16_t(listed),name,guid);
        if (!status && id == guid) return capture ? device->SetRecordingDevice(uint16_t(listed)) : device->SetPlayoutDevice(uint16_t(listed));
        logf("voice device order differs from the ADM; searching");
      }
      uint16_t index = 0;
      auto count = capture ? device->RecordingDevices() : device->PlayoutDevices();
      for (; index < count; ++index) {
        char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
        auto status = capture ? device->RecordingDeviceName(index,name,guid) : device->PlayoutDeviceName(index,name,guid);
        if (!status && id == guid) break;
      }
      if (index >= count) return int32_t(-1);
      return capture ? device->SetRecordingDevice(index) : device->SetPlayoutDevice(index);
    };
    auto start = [&] {
      int status = capture ? device->InitRecording() : device->InitPlayout();
      if (!status && active) status = capture ? device->StartRecording() : device->StartPlayout();
      return status;
    };
    int result = choose(id.empty()); if (!result) result = start();
    if (result && !id.empty()) {
      id.clear(); capture ? device->StopRecording() : device->StopPlayout();
      result = choose(true); if (!result) result = start();
      logf("voice device open failed; fallback to Windows default");
    }
    return result == 0;
  });
  if (success) (capture ? options_.input : options_.output) = id;
  recovery.baseline=(capture ? audio_.captureCalls : audio_.renderCalls)->load();
  recovery.pending=true; recovery.opened=success; ++recovery.attempts;
  recovery.next=NowMs()+std::min(4000,250*(1 << std::min(recovery.attempts,4)));
  logf("voice device opening capture=%d default=%d opened=%d; awaiting callbacks; same PeerConnection", capture, id.empty(), success);
  Update update{Update::Devices}; update.capture = capture; update.who = capture ? options_.input : options_.output;
  update.reason=success ? "opening" : "failed"; update.devices = devices; Emit(std::move(update));
}
void Session::SetDevice(bool capture, std::string id) { Post([this, capture, id = std::move(id)] { (capture ? capture_recovery_ : playout_recovery_)={}; ChooseDevice(capture, id); }); }
void Session::SyncDevices() {
  if (!audio_.device) return;
  for (bool capture : {true, false}) {
    // The engine tests' simulated device exists only inside the ADM.
    auto list = !options_.simulated_device ? Endpoints(capture) : worker_thread_->BlockingCall([&] {
      std::vector<DeviceInfo> result; auto d = audio_.device;
      auto count = capture ? d->RecordingDevices() : d->PlayoutDevices();
      for (int16_t i = 0; i < count; ++i) {
        char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
        auto status = capture ? d->RecordingDeviceName(i, name, guid) : d->PlayoutDeviceName(i, name, guid);
        if (!status) result.push_back({guid, name});
      }
      return result;
    });
    auto& old = capture ? inputs_ : outputs_;
    bool changed = old.size() != list.size();
    for (size_t i = 0; !changed && i < old.size(); ++i) changed = old[i].id != list[i].id || old[i].name != list[i].name;
    old = std::move(list);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&enumerator)))) {
      IMMDevice* endpoint = nullptr; std::wstring current;
      if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender,eConsole,&endpoint))) {
        LPWSTR endpointID = nullptr;
        if (SUCCEEDED(endpoint->GetId(&endpointID))) { current = endpointID; CoTaskMemFree(endpointID); }
        endpoint->Release();
      }
      enumerator->Release(); auto& previous = capture ? default_input_ : default_output_;
      if (previous != current && (capture ? options_.input : options_.output).empty()) {
        changed = true; logf("voice Windows default endpoint changed capture=%d",capture);
      }
      previous = std::move(current);
    }
    if (changed) (capture ? capture_recovery_ : playout_recovery_)={};
    if (changed || device_check_ == 0) ChooseDevice(capture, capture ? options_.input : options_.output);
  }
  device_check_ = NowMs();
}
void Session::CheckDeviceRecovery(bool capture) {
  auto& r=capture ? capture_recovery_ : playout_recovery_;
  if (!r.pending) return;
  auto calls=(capture ? audio_.captureCalls : audio_.renderCalls)->load();
  if (r.opened && calls>r.baseline) {
    r={}; Update u{Update::Devices}; u.capture=capture; u.reason="recovered";
    u.who=capture ? options_.input : options_.output; u.devices=capture ? inputs_ : outputs_; Emit(std::move(u));
    logf("voice device recovered capture=%d success=1; callbacks growing",capture); return;
  }
  if (NowMs()<r.next) return;
  if (r.attempts>=5) {
    if(r.opened) {
      r.opened=false; Update u{Update::Devices}; u.capture=capture; u.reason="failed";
      u.who=capture ? options_.input : options_.output; u.devices=capture ? inputs_ : outputs_; Emit(std::move(u));
      logf("voice device recovery exhausted capture=%d; no fresh callbacks",capture);
    }
    return;
  }
  ChooseDevice(capture,capture ? options_.input : options_.output);
}
bool Session::NetworkOnline() {
#ifdef HARMONY_ENGINE_TEST
 return engine_test::online;
#endif
  PMIB_IF_TABLE2 table=nullptr; bool online=false;
  if (GetIfTable2(&table)!=NO_ERROR) return network_online_;
  for (ULONG i=0;i<table->NumEntries;++i) {
    auto& row=table->Table[i];
    if (row.Type!=IF_TYPE_SOFTWARE_LOOPBACK && row.OperStatus==IfOperStatusUp && row.MediaConnectState==MediaConnectStateConnected) online=true;
  }
  FreeMibTable(table); return online;
}
void Session::ObserveDecode(const std::string& mid,std::pair<uint64_t,uint64_t> counters,int64_t now) {
  auto info=tracks_.find(mid);if(info==tracks_.end()||info->second.source!="screen")return;
  auto tile=tile_layers_.find(info->second.username);
  auto* health=tile!=tile_layers_.end()?&tile->second.decode:info->second.username==options_.watch?&layer_request_.decode:nullptr;
  auto previous=decode_previous_.find(mid);
  const bool fresh=previous==decode_previous_.end()||counters.first<previous->second.first||counters.second<previous->second.second;
  if(fresh) {
    // Sample's 20-second hold uses the same monotonic clock as Tick. Zero
    // means boot time, not the start of this stream: startup/keyframe gaps
    // otherwise look like three consecutive overloads and cap 1080p to 30 fps.
    if(health) {health->moved=now;health->bad=health->clean=0;}
  } else if(health) {
    int decoded=int(counters.first-previous->second.first),dropped=int(counters.second-previous->second.second);
    if(health->Sample(decoded,dropped,now))logf("screen decode cap=%s decoded=%d dropped=%d",media::Rid(health->cap),decoded,dropped);
  }
  if(health&&info->second.username==options_.watch)layer_request_.decode=*health;
  decode_previous_[mid]=counters;
}
void Session::Tick() {
  if (!wanted_) return;
  auto now = NowMs();
  if(pending_screen_source_) {
    if(pending_screen_source_->state()==webrtc::MediaSourceInterface::kLive) {
      auto source=pending_screen_source_;pending_screen_source_=nullptr;
      options_.share_window=pending_screen_options_.share_window;options_.share_monitor=pending_screen_options_.share_monitor;
      options_.share_height=pending_screen_options_.share_height;options_.share_fps=pending_screen_options_.share_fps;options_.share_audio=pending_screen_options_.share_audio;
      CommitScreen(source);
    } else if(pending_screen_source_->state()==webrtc::MediaSourceInterface::kEnded||now>=pending_screen_deadline_) {logf("screen open failed step=first_frame reason=%s deadline_ms=3000",now>=pending_screen_deadline_?"timeout":"source_ended");pending_screen_source_=nullptr;ScreenState("capture_unavailable");}
  }
  if(flow_&&now-decode_check_>=1000) {
    std::map<std::string,std::pair<uint64_t,uint64_t>> samples;{std::lock_guard lock(flow_->video_mu);samples=flow_->video;}
    for(auto& [mid,counters]:samples) ObserveDecode(mid,counters,now);
    decode_check_=now;
  }
  if(camera_&&camera_->GetSource()->state()==webrtc::MediaSourceInterface::kEnded) { camera_failed_=true; SetCamera(false,camera_id_); }
  if(screen_) {
#if HARMONY_GPU && !defined(HARMONY_ENGINE_TEST)
    auto followed=gpu::CaptureWindow(screen_capture_);
    if(options_.share_window&&followed&&followed!=options_.share_window) {
      options_.share_window=followed;active_screen_options_.share_window=followed;
      transmission_.Choose(true);ScreenState("transmitting");
      if(options_.share_audio&&screen_audio_tr_) {
        screen_audio_tr_->sender()->SetTrack(nullptr);DetachEverywhere(screen_audio_.get());
        screen_audio_=nullptr;system_audio_=nullptr;
        DWORD pid=0;GetWindowThreadProcessId(followed,&pid);
        if(pid)system_audio_=loopback::SystemAudioSource::Create(pid,true);
        if(system_audio_)screen_audio_=factory_->CreateAudioTrack("screen_audio",system_audio_.get());
        screen_audio_tr_->sender()->SetTrack(screen_audio_.get());
        if(!screen_audio_)ScreenState("screen_audio_unavailable");
      }
    }
#endif
    RECT size{};if(options_.share_window)GetClientRect(options_.share_window,&size);
    const char* event=transmission_.Observe(!options_.share_window||IsWindow(options_.share_window),screen_->GetSource()->state()==webrtc::MediaSourceInterface::kEnded,options_.share_window&&IsIconic(options_.share_window),size.right,size.bottom);
    if(event) {
      if(!transmission_.active) StopScreenNow(event);
      else if(std::string(event)=="resized") logf("screen window client resized width=%d height=%d",size.right,size.bottom);
      else {ScreenState(event);logf("screen minimized=%d",transmission_.minimized);}
    }
  }
  int layer=layer_request_.Ready(now);
  auto layerSession=session_id();
  for(auto& [who,request]:tile_layers_) {
    if(layerSession.empty()||!request.CanAttempt(now))continue;
    // The separate video window watches the same person on its own account.
    if(int move=request.PauseMove(now);move&&who!=options_.watch&&who!=options_.username) {
      auto body=json::Value::Object();body["session_id"]=layerSession;body["username"]=who;body["watching"]=move<0;
      auto response=SessionPost("/api/voice/watch",body);
      if(response.status==204) {request.Paused(move>0,NowMs());logf("screen tile %s status=204",move>0?"paused, not looked at":"resumed");}
      else if(request.Complete(request.requested,false,NowMs()))logf("screen tile pause=%d status=%d",int(move>0),response.status);
      continue;
    }
    if(request.paused)continue;
    int desired=request.Ready(now);if(desired<0)continue;
    if(options_.watch==who&&layer>=0)desired=std::max(request.decode.cap,std::min(desired,layer));
    if(desired==request.requested)continue;
    auto body=json::Value::Object();body["session_id"]=layerSession;body["username"]=who;body["layer"]=media::Rid(desired);
    auto response=SessionPost("/api/voice/layer",body);
    // A granted size is rare (a change of what is looked at) and stays in the
    // diagnostic; of a run of failures only the first is written.
    if(request.Complete(desired,response.status==204,NowMs())||response.status==204)
      logf("screen tile layer requested=%s status=%d",media::Rid(desired),response.status);
  }
  if(!layerSession.empty()&&layer_request_.CanAttempt(now)&&pc_&&options_.watch_source=="screen"&&!options_.watch.empty()&&!tile_layers_.count(options_.watch)&&layer>=0&&layer!=layer_request_.requested) {
    auto body=json::Value::Object(); body["session_id"]=layerSession; body["username"]=options_.watch; body["layer"]=media::Rid(layer);
    auto response=SessionPost("/api/voice/layer",body);
    if(layer_request_.Complete(layer,response.status==204,NowMs())||response.status==204)
      logf("screen layer requested=%s status=%d",media::Rid(layer),response.status);
  }
  if (device_watch_ ? device_watch_->Take() : now - device_check_ >= 1000) SyncDevices();
  CheckDeviceRecovery(true); CheckDeviceRecovery(false);
  std::map<std::string, double> levels;
  for (auto& [mid, r] : reception_) levels[r.username] = std::max(levels[r.username], r.meter->Read(now));
  auto emitLevel=[&](std::string const& who,double amplitude,bool own) {
    auto on=speaking_levels_[who].Observe(amplitude,NowMs(),!own || (!options_.muted && !options_.deafened));
    if(!on) return;
    Update u{Update::Level};u.who=who;u.own=own;u.amplitude=amplitude;u.reason=*on?"speaking":"quiet";Emit(std::move(u));
  };
  for(auto& [who,level]:levels) emitLevel(who,level,false);
  if(own_meter_) emitLevel(options_.username,own_meter_->Read(now),true);
  // A track can disappear before its final quiet sample.
  for(auto& [who,gate]:speaking_levels_) if(!levels.count(who) && who!=options_.username) {
    if(auto on=gate.Observe(0,NowMs())) {Update u{Update::Level};u.who=who;u.reason=*on?"speaking":"quiet";Emit(std::move(u));}
  }
  if (audio_.renderLevel && now-render_check_>=250) {
    render_check_=now;
    Update u{Update::Render}; bool fresh=GetTickCount64()-audio_.renderAt->load()<100;
    u.amplitude=fresh?audio_.renderLevel->load():0; u.reason=fresh?"active":"idle";
    u.generation=unsigned(audio_.renderCalls->load());
    u.dfnActive=audio_.filter->active.load(); u.dfnFallbacks=audio_.filter->fallbacks.load(); u.noiseFilter=u.dfnActive?"DFN3":(audio_.filter->nsApplied.load()?"libwebrtc NS":"raw");
    u.noiseReason=audio::FilterSlot::ReasonText(audio_.filter->reason.load());
    u.captureCallbacks=audio_.captureCalls->load(); u.referenceMissing=audio_.referenceMissing->load(); u.referenceDropped=audio_.referenceDropped->load(); u.referenceAge=audio_.referenceAge->load();
    Emit(std::move(u));
  }
  if (audio_.captureCalls && now-diagnostic_check_>=1000) {
    logf("voice audio capture=%llu render=%llu reference_missing=%llu reference_dropped=%llu reference_age_ms=%llu",
      audio_.captureCalls->load(),audio_.renderCalls->load(),audio_.referenceMissing->load(),audio_.referenceDropped->load(),audio_.referenceAge->load());
    logf("voice noise=%s dfn_fallbacks=%llu",audio_.filter->active.load()?"DFN3":(audio_.filter->nsApplied.load()?"libwebrtc NS":"raw"),audio_.filter->fallbacks.load());
    diagnostic_check_=now;
  }
  if (pc_ && flow_) {
    auto in = flow_->incoming.load(), out = flow_->outgoing.load();
    bool growing = in > flow_->beforeIn || (reception_.empty() && out > flow_->beforeOut);
    flow_->beforeIn = in; flow_->beforeOut = out;
    if (connected_ && growing && (!recovery_at_ || restart_applied_)) {
      if (recovery_at_) logf("voice ICE recovered in %lld ms; RTP growing; generation=%u", now - outage_at_, session_generation_);
      recovery_at_ = 0; outage_at_=0; ++recovery_generation_; restart_applied_ = false; rejoin_attempts_ = 0;
      Emit(Update{Update::Flowing});
    }
    if (now - stats_check_ >= 350) { pc_->GetStats(flow_.get()); stats_check_ = now; }
  }
  PostDelayed(25, [this] { Tick(); });
}
void Session::SetMicGain(double gain) {
  if(!std::isfinite(gain)) return;
  Post([this,gain] { options_.micGain=std::clamp(gain,0.,2.); if(audio_.filter) audio_.filter->micGain=options_.micGain; });
}
void Session::SetSidetone(bool on) {
  Post([this,on] { options_.sidetone=on; ApplySidetone(); logf("voice sidetone=%d",int(on)); });
}
void Session::ApplySidetone() {
  if(audio_.sidetone) audio_.sidetone->store(options_.sidetone && !options_.muted && !options_.deafened);
}
void Session::SetMicFilters(bool echo,bool noise,bool agc) {
  Post([this,echo,noise,agc] {
    options_.echo=echo; options_.noise=noise; options_.agc=agc;
    if(audio_.filter) { audio_.filter->echo=echo; audio_.filter->noise=noise; audio_.filter->agc=agc; }
  });
}
void Session::SetNoise(bool enabled,int strength) {
  Post([this,enabled,strength] { options_.dfn=enabled; options_.strength=std::clamp(strength,10,100);
    if(audio_.filter) { audio_.filter->enabled=enabled; audio_.filter->strength=options_.strength; if(!enabled) audio_.filter->active=false; } });
}
#ifndef HARMONY_PUBLISH
void Session::TestTone() {
  if (!wanted_) return;
  if(test_pcm_.empty()) {
    wchar_t path[32768]{}; auto n=GetEnvironmentVariableW(L"HARMONY_TEST_PCM",path,32768);
    if(n && n<32768 && !client_->secure() && client_->allowed()) {
      std::ifstream in(std::filesystem::path(path),std::ios::binary|std::ios::ate); auto bytes=in.tellg();
      if(bytes>0 && bytes<48000*2*60 && int64_t(bytes)%2==0) { test_pcm_.resize(size_t(bytes)/2); in.seekg(0); in.read(reinterpret_cast<char*>(test_pcm_.data()),bytes); }
    }
  }
  std::vector<int16_t> block(480);
  for(auto& sample:block) { sample=test_pcm_.empty()?int16_t(6000*std::sin(6.283185307179586*440*double(test_sample_)/48000)):test_pcm_[test_sample_%test_pcm_.size()]; ++test_sample_; }
  audio_.filter->nsApplied=false; // This local synthetic source bypasses the device APM.
  audio_.filter->Process(block,48000,1);
  audio_.mic->Deliver(block.data(),48000,1,480);
  PostDelayed(10,[this] { TestTone(); });
}
#endif
void Session::Recover(std::string reason) {
  Post([this, reason = std::move(reason)] {
    if (!wanted_ || session_id().empty()) return;
    bool online=NetworkOnline();
    // A gateway resync must replay offers lost during a rejoin, even while
    // that rejoin is still waiting for its first media sample.
    bool renew=reason=="gateway" || reason=="resume" || (reason=="network" && online);
    network_online_=online;
    if (recovery_at_ && !renew && online) return;
    if (!outage_at_) outage_at_=NowMs();
    rejoin_attempts_=0;
    recovery_at_ = NowMs(); ++recovery_generation_; restart_applied_ = false;
    if (flow_) { flow_->beforeIn = flow_->incoming; flow_->beforeOut = flow_->outgoing; }
    Update u{Update::Recovering}; u.reason = reason; Emit(std::move(u));
    logf("voice ICE restart requested: %s", reason.c_str()); RestartAttempt(recovery_generation_);
  });
}
void Session::RestartAttempt(uint64_t generation) {
  if (!wanted_ || !recovery_at_ || generation != recovery_generation_) return;
  if (!NetworkOnline()) {
    network_online_=false;
    PostDelayed(500,[this,generation] { RestartAttempt(generation); }); return;
  }
  if (!network_online_) { Recover("network"); return; }
  if (restart_applied_ && NowMs() - recovery_at_ < 8000) {
    PostDelayed(350, [this, generation] { RestartAttempt(generation); }); return;
  }
  auto body = json::Value::Object(); body["session_id"] = session_id();
  auto response = SessionPost("/api/voice/restart", body);
  // Connectivity may have changed while WinHTTP was waiting for its answer.
  if (!NetworkOnline()) {
    network_online_=false;
    PostDelayed(500,[this,generation] { RestartAttempt(generation); }); return;
  }
  bool missing = response.status == 404 && response.Json()["error"].Str() == "not_found";
  if (missing || NowMs() - recovery_at_ >= 8000) {
    if (++rejoin_attempts_ > 3) {
      // Exhaust this online window, retain the room and retry with bounded backoff.
      ++recovery_generation_; rejoin_attempts_=0; recovery_at_=NowMs(); restart_applied_=false;
      auto next=recovery_generation_; PostDelayed(10000,[this,next] { RestartAttempt(next); }); return;
    }
    logf("voice session missing or restart deadline; rejoin same room attempt %d", rejoin_attempts_);
    CloseMedia();
    if (worker_thread_) {
      // Closing the last connection terminates the ADM's audio buffer. The
      // pipeline's callback registration belongs to that buffer, so keeping
      // the old factory/device can report connected with no capture/playout
      // callbacks. Rebuild this session's engine together with its connection.
      if (audio_.mixer) audio_.mixer->OnUnknown(nullptr);
      if (own_meter_) audio_.mic->RemoveSink(own_meter_.get());
      own_meter_.reset(); mic_ = nullptr; flow_ = nullptr;
      factory_ = nullptr;
      if (worker_thread_) worker_thread_->BlockingCall([this] { audio_ = {}; }); else audio_ = {};
      signaling_thread_.reset(); worker_thread_.reset(); network_thread_.reset();
      capture_recovery_ = {}; playout_recovery_ = {}; device_check_ = 0;
      if (!CreateFactory()) {
        logf("voice rejoin engine unavailable");
        recovery_at_ = NowMs();
        PostDelayed(1000, [this, generation] { RestartAttempt(generation); });
        return;
      }
      own_meter_ = std::make_unique<Meter>();
      audio_.mic->AddSink(own_meter_.get());
    }
    std::string error;
    if (Join(&error)) { flow_ = webrtc::make_ref_counted<Flow>(); restart_applied_ = true; }
    else logf("voice rejoin failed");
    // Keep the original outage time for reporting; the next deadline gets
    // its own attempt window without losing the UI's total outage duration.
    recovery_at_ = NowMs();
  } else if (response.status == 200 && !restart_applied_) {
    auto offer = response.Json(); ApplyOffer(offer);
    restart_applied_ = offer["ice_restart"].Bool();
    if (restart_applied_ && flow_) { flow_->beforeIn = flow_->incoming; flow_->beforeOut = flow_->outgoing; }
    logf("voice restart offer answered; fresh ICE=%d", restart_applied_);
  }
  PostDelayed(restart_applied_ ? 350 : 300, [this, generation] {
    if (generation != recovery_generation_ || !wanted_) return;
    if (restart_applied_ && recovery_at_ && NowMs() - recovery_at_ < 8000) {
      PostDelayed(350, [this, generation] { RestartAttempt(generation); });
    } else RestartAttempt(generation);
  });
}

}  // namespace voice
