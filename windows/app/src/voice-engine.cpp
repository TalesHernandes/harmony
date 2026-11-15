#include "voice-engine.h"
#include "platform.h"
#include "../../native/src/voice.h"
#include "../../native/src/call-media.h"
#include "direct-call.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include "video-renderer.h"
#include "../../native/src/resource-usage.h"

namespace app {
std::vector<VoiceDevice> EnumerateVoiceDevices(bool capture) {
#ifndef HARMONY_PUBLISH
  if(Environment(L"HARMONY_TEST_ADM")=="1") return {{"device-a","Dispositivo simulado A"}};
#endif
  using Microsoft::WRL::ComPtr;
  auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  std::vector<VoiceDevice> devices;
  {
    ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDeviceCollection> collection;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator))) &&
       SUCCEEDED(enumerator->EnumAudioEndpoints(capture?eCapture:eRender,DEVICE_STATE_ACTIVE,&collection))) {
      UINT count=0; collection->GetCount(&count);
      for(UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> device; ComPtr<IPropertyStore> properties; LPWSTR id=nullptr;
        if(FAILED(collection->Item(i,&device)) || FAILED(device->GetId(&id))) continue;
        PROPVARIANT name{};
        if(SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties)) && SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&name)) && name.vt==VT_LPWSTR)
          devices.push_back({net::Narrow(id),net::Narrow(name.pwszVal)});
        PropVariantClear(&name); CoTaskMemFree(id);
      }
    }
  }
  if(SUCCEEDED(initialized)) CoUninitialize(); return devices;
}
namespace {
class Engine final : public VoiceEngine {
 public:
  Engine(net::Client* client, const VoiceState& state, std::function<void(VoiceUpdate)> changed) : session_(client, Options(client, state, std::move(changed))) {}
  bool Start(std::string* error) override {bool ok=session_.Start(error);if(ok)resourceLoad_.Loaded();return ok;}
  void Leave() override { session_.Leave(); }
  void Event(const std::string& type, const json::Value& data) override { session_.OnSocketEvent(type, data); }
  void Recover(const std::string& reason) override { session_.Recover(reason); }
  void MicGain(double gain) override { session_.SetMicGain(gain); }
  void MicFilters(bool echo,bool noise,bool agc) override { session_.SetMicFilters(echo,noise,agc); }
  void Sidetone(bool on) override { session_.SetSidetone(on); }
  void Noise(bool enabled,int strength) override { session_.SetNoise(enabled,strength); }
  void Mute(bool muted, bool deafened) override { session_.SetMute(muted, deafened); }
  void Volume(const std::string& who, double gain) override { session_.SetVolume(who, gain); }
  void Device(bool capture, const std::string& id) override { session_.SetDevice(capture, id); }
  void Screen(HWND window,HMONITOR monitor,int height,int fps,bool sound) override { session_.SetScreen(window,monitor,height,fps,sound); }
  void StopScreen() override { session_.StopScreen(); }
  void Camera(bool enabled,const std::string& id) override { session_.SetCamera(enabled,id); }
  void Watch(const std::string& who,HWND target,bool camera) override {
    if(!target) {session_.Watch("",{});return;}
    auto renderer=std::make_shared<VideoRenderer>(target);
    session_.Watch(who,[renderer](auto track){renderer->Attach(track);},camera);
  }
  void WatchSize(double width,double dpi,bool visible) override { session_.WatchSize(width,dpi,visible); }
  void Video(std::string who,std::string source,std::function<void(webrtc::VideoTrackInterface*)> attach) override {session_.Video(std::move(who),std::move(source),std::move(attach));}
  void VideoSize(std::string who,double width,double dpi,bool visible) override {session_.VideoSize(std::move(who),width,dpi,visible);}
 private:
  static voice::Options Options(net::Client* client, const VoiceState& state, std::function<void(VoiceUpdate)> changed) {
    voice::Options options; options.room_id = state.room; options.muted = state.muted; options.deafened = state.deafened;
    options.micGain=state.micGain; options.echo=state.echo; options.noise=state.noise; options.agc=state.agc; options.dfn=state.dfn; options.strength=state.strength; options.sidetone=state.voiceTest;
    options.volumes = state.volumes; options.input = state.input; options.output = state.output;
#ifndef HARMONY_PUBLISH
    options.test_tone = !client->secure() && client->allowed() && Environment(L"HARMONY_TEST_TONE") == "1";
    options.simulated_device=!client->secure() && client->allowed() && Environment(L"HARMONY_TEST_ADM")=="1";
    options.test_require_cable=!client->secure()&&client->allowed()&&Environment(L"HARMONY_TEST_REQUIRE_CABLE")=="1";
#endif
    auto me = client->Get("/api/me").Json(); options.username = me["user"]["username"].Str();
    options.changed = [changed = std::move(changed)](voice::Update u) {
      VoiceUpdate update{static_cast<VoiceUpdate::Kind>(u.kind)};
      update.who = std::move(u.who); update.reason = std::move(u.reason); update.amplitude = u.amplitude;
      update.connectionIdentity=u.connectionIdentity; update.sessionIdentity=u.sessionIdentity;
      update.dfnActive=u.dfnActive; update.dfnFallbacks=u.dfnFallbacks; update.noiseFilter=u.noiseFilter; update.noiseReason=u.noiseReason;
      update.captureCallbacks=u.captureCallbacks; update.referenceMissing=u.referenceMissing; update.referenceDropped=u.referenceDropped; update.referenceAge=u.referenceAge;
      update.at = u.at; update.own = u.own; update.capture = u.capture; update.generation = u.generation;
      update.sharingScreen=u.sharingScreen; update.sharingCamera=u.sharingCamera;
      for (auto& d : u.devices) update.devices.push_back({std::move(d.id), std::move(d.name)});
      changed(std::move(update));
    };
    return options;
  }
  resources::Load resourceLoad_{resources::Voice};
  voice::Session session_;
};
}
std::shared_ptr<VoiceEngine> CreateVoiceEngine(net::Client* client, const VoiceState& state, std::function<void(VoiceUpdate)> changed) {
  return std::make_shared<Engine>(client, state, std::move(changed));
}
namespace {
class Monitor final : public VoiceTest {
 public:
  explicit Monitor(audio::MonitorSettings settings) : monitor_(std::move(settings)) {}
  bool Running() const override { return monitor_.Running(); }
  void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double gain) override { monitor_.Filters(dfn, strength, echo, noise, agc, gain); }
 private:
  audio::Monitor monitor_;
};
}
std::unique_ptr<VoiceTest> CreateVoiceTest(net::Client* client, const VoiceState& state) {
  audio::MonitorSettings settings;
  settings.input = state.input; settings.output = state.output;
  settings.dfn = state.dfn; settings.strength = state.strength; settings.echo = state.echo; settings.noise = state.noise; settings.agc = state.agc;
  settings.micGain = state.micGain;
#ifndef HARMONY_PUBLISH
  bool local = !client->secure() && client->allowed();
  settings.simulatedDevice = local && Environment(L"HARMONY_TEST_ADM") == "1";
  settings.requireCable = local && Environment(L"HARMONY_TEST_REQUIRE_CABLE") == "1";
#endif
  return std::make_unique<Monitor>(std::move(settings));
}
namespace {
// The model's media (direct-call.h) over the W6 connection (call-media.h).
class CallMedia final : public DirectMedia {
 public:
  explicit CallMedia(callmedia::Options options) : connection_(std::move(options)) {}
  bool Open(bool polite, const json::Value& ice, std::string* error) override {bool ok=connection_.Open(polite,ice,error);if(ok)resourceLoad_.Loaded();return ok;}
  void Remote(const std::string& kind, const json::Value& payload) override { connection_.Remote(kind, payload); }
  void Mute(bool muted) override { connection_.Mute(muted); }
  void Deafen(bool deafened) override { connection_.Deafen(deafened); }
  void Volumes(double voice, double screen) override { connection_.Volumes(voice, screen); }
  void Close() override { connection_.Close(); }
  void Camera(bool on, const std::string& id) override { connection_.Camera(on, id); }
  void Screen(void* window, void* monitor, int height, int fps, bool sound) override {
    connection_.Screen(static_cast<HWND>(window), static_cast<HMONITOR>(monitor), height, fps, sound);
  }
  void StopScreen() override { connection_.StopScreen(); }
  void Video(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach) override { connection_.Video(slot, std::move(attach)); }
  void Device(bool capture, const std::string& id) override { connection_.Device(capture, id); }
  void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double gain) override { connection_.Filters(dfn, strength, echo, noise, agc, gain); }
  void Sidetone(bool on) override { connection_.Sidetone(on); }
 private:
  resources::Load resourceLoad_{resources::Voice};
  callmedia::Connection connection_;
};
}
std::unique_ptr<DirectMedia> CreateDirectMedia(net::Client* client, const VoiceState& state,
    std::function<void(std::string, json::Value)> send, std::function<void(bool)> status) {
  callmedia::Options options;
  options.input = state.input; options.output = state.output;
  options.dfn = state.dfn; options.echo = state.echo; options.noise = state.noise; options.agc = state.agc;
  options.micGain = state.micGain; options.strength = state.strength; options.sidetone = state.voiceTest;
  const std::wstring& host = client->host();
  options.loopback = host == L"127.0.0.1" || host == L"localhost" || host == L"::1";
#ifndef HARMONY_PUBLISH
  bool local = !client->secure() && client->allowed();
  options.test_tone = local && Environment(L"HARMONY_TEST_TONE") == "1";
  options.simulated_device = local && Environment(L"HARMONY_TEST_ADM") == "1";
#endif
  options.send = std::move(send); options.status = std::move(status);
  return std::make_unique<CallMedia>(std::move(options));
}
}

namespace app {
// The direct call's two levels for its green rings (call-media.cpp).
void DirectLevels(double* mine, double* theirs) { callmedia::Levels(mine, theirs); }
}
