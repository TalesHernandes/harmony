// A voice room session against the harmony SFU (PROTOCOL-HARMONY §4), the
// native twin of web/harmony/src/media.ts for what W1 needs: join, talk with
// echo cancellation, share one window as a three-layer screen, watch one
// person's screen, leave.
//
// Every signalling step runs on ONE queue thread ("harmony-voice"), in order:
// the server's offers, the screen's own offer and the answers must never
// interleave (media.ts:1503 â€” "uma oferta de cada vez, em fila").
#pragma once
#include "speaking-level.h"
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "api/peer_connection_interface.h"
#include "audio.h"
#include "json.h"
#include "loopback.h"
#include "net.h"
#include "media-policy.h"

namespace voice {
struct DeviceInfo { std::string id, name; };
struct Update {
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
  std::vector<DeviceInfo> devices;
};

struct Options {
  std::string room;          // voice channel name; empty = the first one
  std::string room_id, username, input, output;
  bool muted = false, deafened = false;
  bool dfn = true, echo=true, noise=true, agc=true; double micGain=1; int strength = 50;
  bool sidetone = false;  // "Ouvir minha voz" (audio.h Pipeline::sidetone)
  std::map<std::string, double> volumes;
  std::function<void(Update)> changed;
  bool mic = true;
  bool test_tone = false;
  bool simulated_device = false;
  bool test_require_cable = false;
  HWND share_window = nullptr;  // share this window as the screen
  HMONITOR share_monitor = nullptr;
  int share_height = 1080;
  int share_fps = 60;
  bool share_audio = true;   // the PC's sound, minus harmony, with the screen
  std::string watch;         // username whose screen to watch
  std::string watch_source="screen";
  // Called on the voice thread when the watched screen's track arrives (or
  // nullptr when it goes). The sink must be quick; it runs on decode threads.
  std::function<void(webrtc::VideoTrackInterface*)> on_watch;
};

class Session : public webrtc::PeerConnectionObserver {
 public:
  Session(net::Client* client, Options options);
  ~Session() override;

  // Creates the engine, finds the room and joins. Blocks until joined or failed.
  bool Start(std::string* error);
  void Leave();
  // Events from the live socket.
  void OnSocketEvent(const std::string& type, const json::Value& data);
  void OnSocketOpen();
  void Recover(std::string reason);
  void SetMute(bool muted, bool deafened);
  void SetNoise(bool enabled, int strength);
  void SetMicGain(double gain);
  void SetMicFilters(bool echo, bool noise, bool agc);
  // Hearing one's own voice in the room; silent while muted or deafened.
  void SetSidetone(bool on);
  uint64_t SidetonePlayed() const { return audio_.sidetonePlayed ? audio_.sidetonePlayed->load() : 0; }
  void SetVolume(std::string who, double gain);
  void SetDevice(bool capture, std::string id);
  void SetScreen(HWND window,HMONITOR monitor,int height,int fps,bool sound);
  void StopScreen();
  void Watch(std::string username,std::function<void(webrtc::VideoTrackInterface*)> sink,bool camera=false);
  void WatchSize(double width,double dpi,bool visible);
  void SetCamera(bool enabled,std::string id);
  void Video(std::string who,std::string source,std::function<void(webrtc::VideoTrackInterface*)> attach);
  void VideoSize(std::string who,double width,double dpi,bool visible);

  // The panel reads these, once per second, through stats.
  webrtc::PeerConnectionInterface* pc() { return pc_.get(); }
  std::string session_id() const;

 private:
  // PeerConnectionObserver, on libwebrtc's signalling thread: hand over to ours.
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;

  void Post(std::function<void()> task);
  void PostDelayed(int ms, std::function<void()> task);
  void Loop();

  bool CreateFactory();
  bool CreateConnection(const json::Value& ice_servers);
  bool Join(std::string* error);
  void Tick();
  void TestTone();
  uint64_t test_sample_ = 0;
  std::vector<int16_t> test_pcm_;
  void RestartAttempt(uint64_t generation);
  void Emit(Update update);
  void BindAudio();
  void ApplyVolumes();
  void ApplySidetone();
  void SyncDevices();
  void ChooseDevice(bool capture, std::string id);
  net::Response SessionPost(const std::string&,const json::Value&);
  void SendState();
  void CloseMedia();
  class Meter;
  struct Reception {
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;
    std::unique_ptr<Meter> meter;
    std::string username;
    // Whose volume applies: the person's voice, or "who/screen_audio" for the
    // sound of their screen, which the web sets apart ("som da live").
    std::string volume_key;
  };
  std::map<std::string, Reception> reception_;
  std::unique_ptr<Meter> own_meter_;
  std::string applied_offer_, applied_answer_;
  std::vector<DeviceInfo> inputs_, outputs_;
  std::wstring default_input_, default_output_;
  int64_t device_check_ = 0, recovery_at_ = 0;
  uint64_t recovery_generation_ = 0;
  unsigned session_generation_ = 0;
  bool restart_applied_ = false, connected_ = false, wanted_ = true;
  int rejoin_attempts_ = 0;
  int64_t outage_at_=0;
  bool network_online_=true;
  bool capture_wanted_=false, playout_wanted_=true;
  struct DeviceRecovery { bool pending=false, opened=false; int attempts=0; int64_t next=0; uint64_t baseline=0; };
  DeviceRecovery capture_recovery_, playout_recovery_;
  void CheckDeviceRecovery(bool capture);
  bool NetworkOnline();
  friend class EngineRegression;
  void* network_notification_ = nullptr;
  class DeviceWatch;
  DeviceWatch* device_watch_ = nullptr;  // created and released on the session thread
  struct Flow;
  webrtc::scoped_refptr<Flow> flow_;
  int64_t stats_check_ = 0, diagnostic_check_=0, render_check_=0;
  std::map<std::string,SpeakingLevel> speaking_levels_;
  void ApplyOffer(const json::Value& offer);
  void UpdateAudience();
  void ShareScreen();
  void RestoreOutgoingMedia();
  void CommitScreen(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source);
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> pending_screen_source_;
  // Owned by screen_'s source proxy; queried only while screen_ is live.
  webrtc::VideoTrackSourceInterface* screen_capture_=nullptr;
  Options pending_screen_options_, active_screen_options_;
  int64_t pending_screen_deadline_=0;
  void ScreenState(std::string reason);
  void StopScreenNow(std::string reason);
  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>> DetachEverywhere(webrtc::MediaStreamTrackInterface* track);
  // The server refused another slot on this connection (reconnect_needed);
  // nothing short of a new connection lifts it.
  bool slot_limit_ = false;
  void OfferScreen(int attempt);
  void ApplyWatchers(const json::Value& data);
  void SetLayersActive(const std::set<std::string>& keep);
  void TuneMic();
  void TuneScreen();
  void SetCaptureRate(bool encoding);
  void AttachWatched();
  void AttachVideos();
  struct VideoSlot { std::string who,source; std::function<void(webrtc::VideoTrackInterface*)> attach; webrtc::scoped_refptr<webrtc::VideoTrackInterface> track; };
  std::map<std::string,VideoSlot> videos_;
  std::map<std::string,media::LayerRequest> tile_layers_;
  int64_t decode_check_=0;
  std::map<std::string,std::pair<uint64_t,uint64_t>> decode_previous_;
  void ObserveDecode(const std::string& mid,std::pair<uint64_t,uint64_t> counters,int64_t now);

  net::Client* client_;
  Options options_;
  std::string room_id_;

  mutable std::mutex mu_;
  std::string session_id_;

  // Queue.
  std::thread thread_;
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<std::function<void()>> queue_;
  std::multimap<int64_t, std::function<void()>> delayed_;
  bool stop_ = false;

  // Engine (owned by the voice thread after Start).
  std::unique_ptr<webrtc::Thread> network_thread_, worker_thread_, signaling_thread_;
  audio::Pipeline audio_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> mic_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> screen_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> camera_;
  std::string camera_id_;
  bool camera_wanted_ = false, camera_failed_ = false;
  webrtc::scoped_refptr<loopback::SystemAudioSource> system_audio_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> screen_audio_;
  std::map<std::string, std::string> send_mids_;  // source -> mid
  struct Track {
    std::string username, source, kind;
  };
  std::map<std::string, Track> tracks_;  // mid -> who
  webrtc::VideoTrackInterface* watched_ = nullptr;
  bool screen_offered_ = false;
  uint64_t screen_offer_generation_ = 0;
  media::Transmission transmission_;
  media::LayerRequest layer_request_;
  std::set<std::string> layers_on_{"alta", "media", "baixa"};
  int64_t shrink_generation_ = 0;
  webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> screen_tr_, screen_audio_tr_;
};

}  // namespace voice
