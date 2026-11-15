#pragma once
#include "voice-state.h"
#include "../../native/src/net.h"
#include <functional>
#include <memory>
namespace webrtc {class VideoTrackInterface;}

namespace app {
std::vector<VoiceDevice> EnumerateVoiceDevices(bool capture);
// The UI/account worker owns this facade; libwebrtc stays in W1.
struct VoiceUpdate {
  enum Kind { Level, Devices, Recovering, Flowing, Failed, Render, Media, Step } kind;
  std::string who, reason;
  double amplitude = 0;
  int64_t at = 0;
  bool capture = false, own = false;
  bool sharingScreen=false,sharingCamera=false;
  unsigned generation = 0;
  uint64_t connectionIdentity=0, sessionIdentity=0;
  bool dfnActive=false; uint64_t dfnFallbacks=0; std::string noiseFilter="libwebrtc NS", noiseReason="aguardando captura";
  uint64_t captureCallbacks=0, referenceMissing=0, referenceDropped=0, referenceAge=0;
  std::vector<VoiceDevice> devices;
};
class VoiceEngine {
 public:
  virtual ~VoiceEngine() = default;
  virtual bool Start(std::string* error) = 0;
  virtual void Leave() = 0;
  virtual void Event(const std::string&, const json::Value&) = 0;
  virtual void Recover(const std::string&) = 0;
  virtual void Mute(bool, bool) = 0;
  virtual void Noise(bool, int) {}
  virtual void MicGain(double) {}
  virtual void MicFilters(bool, bool, bool) {}
  // "Ouvir minha voz" inside the room (audio.h Pipeline::sidetone).
  virtual void Sidetone(bool) {}
  virtual void Volume(const std::string&, double) = 0;
  virtual void Device(bool, const std::string&) = 0;
  virtual void Screen(HWND,HMONITOR,int,int,bool) {}
  virtual void StopScreen() {}
  virtual void Camera(bool,const std::string&) {}
  virtual void Watch(const std::string&,HWND,bool) {}
  virtual void WatchSize(double,double,bool) {}
  virtual void Video(std::string,std::string,std::function<void(webrtc::VideoTrackInterface*)>) {}
  virtual void VideoSize(std::string,double,double,bool) {}
};
std::shared_ptr<VoiceEngine> CreateVoiceEngine(net::Client*, const VoiceState&, std::function<void(VoiceUpdate)>);
// "Ouvir minha voz" (audio.h Monitor): the microphone through the call's
// filters, back into the chosen output, outside rooms and calls; inside
// them the same switch is VoiceEngine/DirectMedia Sidetone.
class VoiceTest {
 public:
  virtual ~VoiceTest() = default;
  virtual bool Running() const = 0;
  virtual void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double gain) = 0;
};
std::unique_ptr<VoiceTest> CreateVoiceTest(net::Client*, const VoiceState&);
// The direct call's media (W6): the voice room's devices and filters, on a
// peer-to-peer connection of its own. send and status come from the media's
// thread; the caller hands them to its worker.
class DirectMedia;
void DirectLevels(double* mine, double* theirs);
std::unique_ptr<DirectMedia> CreateDirectMedia(net::Client*, const VoiceState&,
    std::function<void(std::string, json::Value)> send, std::function<void(bool)> status);
}
