// The media of a direct 1:1 call (PROTOCOL-HARMONY §7): one peer-to-peer
// connection, with the same audio as a voice room (audio.h: the chosen
// devices, the echo canceller, DFN3) and perfect negotiation as the web does
// it (web/harmony/src/direct.ts, PROTOCOL.md §5–6). The call itself — ringing,
// the outbox, who answered — is the app's model (windows/app/direct-call.h);
// this only carries the voice and says what it wants sent.
//
// W1's direct.cpp proved the negotiation; this is that, grown up: the voice
// room's audio pipeline instead of a bare device, and an ICE restart by the
// caller when the path is lost, as the web does.
#pragma once
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <thread>

#include "api/peer_connection_interface.h"
#include "audio.h"
#include "json.h"
#include "network-stats.h"

namespace callmedia {

// How loud this side's microphone and the other side's voice are right now,
// linear RMS (the room's meter, voice.cpp Session::Meter), for the green
// ring; zero with no call. One direct call at a time, so one pair.
void Levels(double* mine, double* theirs);

struct Options {
  std::string input, output;  // device GUIDs; empty = the Windows default
  bool dfn = true, echo = true, noise = true, agc = true;
  double micGain = 1;
  int strength = 50;
  bool sidetone = false;  // "Ouvir minha voz" (audio.h Pipeline::sidetone)
  bool loopback = false;  // a harmony on this PC: allow 127.0.0.1 candidates
  bool test_tone = false, simulated_device = false;
  // Both are called on the media's own thread: hand them over, do not block.
  std::function<void(std::string kind, json::Value payload)> send;
  std::function<void(bool connected)> status;
};

class Connection : public webrtc::PeerConnectionObserver {
 public:
  explicit Connection(Options options);
  ~Connection() override;

  // Builds the engine and the connection. The caller (polite=false) creates
  // the four sendrecv slots — mic, cam, screen, screen sound — and so makes
  // the first offer. Blocks until built.
  bool Open(bool polite, const json::Value& ice_servers, std::string* error);
  // offer, answer or ice from the other side, exactly as it arrived.
  void Remote(const std::string& kind, const json::Value& payload);
  void Mute(bool muted);
  // Not hearing the other side: their voice and their screen's sound at zero.
  void Deafen(bool deafened);
  // Their voice and their screen's sound as loud as the room plays them for
  // that person (0..2, voice-volumes.json); deafened still wins.
  void Volumes(double voice, double screen);
  // The camera (Ajustes' choice) in slot 1 and a window or a monitor in slot 2,
  // with its sound in slot 3: replaceTrack into slots that were negotiated
  // when the call began, so nothing renegotiates (direct.ts send/attach).
  void Camera(bool on, std::string id);
  void Screen(HWND window, HMONITOR monitor, int height, int fps, bool sound);
  void StopScreen();
  // What the other person sends in slot 1 (camera) or 2 (screen), or what
  // this side sends, as its own preview: 3 (camera) or 4 (screen). attach is
  // called with the track, or nullptr when it goes, on the call's thread.
  void Video(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach);
  // Ajustes changed in the middle of a call: now, not on the next one.
  void Device(bool capture, std::string id);
  void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double micGain);
  // Hearing one's own voice in the call; silent while muted.
  void Sidetone(bool on);
  // 10 ms of own voice played so far (the engine test).
  uint64_t SidetonePlayed() const { return audio_.sidetonePlayed ? audio_.sidetonePlayed->load() : 0; }
  // Ends the media; the object may then be destroyed. Idempotent.
  void Close();

 private:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnNegotiationNeededEvent(uint32_t event_id) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;

  void Post(std::function<void()> task);
  void Loop();
  bool Build(bool polite, const json::Value& ice, std::string* error);
  void ChooseDevices();
  void Signal(const std::string& kind, const json::Value& payload);
  json::Value LocalJson();
  void Emit(const std::string& kind, json::Value payload);
  void Tick();
  void AddCandidate(const json::Value& payload);
  void Send(size_t slot, webrtc::MediaStreamTrackInterface* track);
  void Price(size_t slot);
  void AttachVideos();
  void ChooseDevice(bool capture, const std::string& id);

  Options options_;
  bool polite_ = false;
  bool making_offer_ = false, ignore_offer_ = false, closed_ = false;
  std::atomic<bool> muted_{false};
  bool connected_ = false;
  std::mutex muteMu_;
  webrtc::scoped_refptr<audio::MicSource> muteSource_;
  void ApplySidetone() { if (audio_.sidetone) audio_.sidetone->store(options_.sidetone && !muted_); }
  int restarts_ = 0, ticks_ = 0;
  // Candidates that arrive before the first remote description: the web posts
  // them outside the queue that sends its answer, so one can overtake it, and
  // libwebrtc refuses a candidate with no remote description to hang it on.
  bool has_remote_ = false;
  std::vector<json::Value> early_candidates_;
  std::thread tone_;
  std::atomic<bool> tone_stop_{false};

  std::thread thread_;
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;

  std::unique_ptr<webrtc::Thread> network_, worker_, signaling_;
  audio::Pipeline audio_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  webrtc::scoped_refptr<network::Poller> networkStats_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> mic_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> camera_, screen_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> screen_audio_;
  int screen_height_ = 1080, screen_fps_ = 60;
  std::function<void(webrtc::VideoTrackInterface*)> video_attach_[5];
  webrtc::VideoTrackInterface* video_shown_[5] = {};
  bool deafened_ = false;
  double voice_volume_ = 1, screen_volume_ = 1;
  void ApplyDeafen();
  struct Meter;
  std::unique_ptr<Meter> own_meter_, peer_meter_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> peer_voice_;
  void Meters();
  // Pictures that arrive from the other side, counted for the log whether or
  // not a tile shows them (the round, and a "I see nothing" from the owner).
  struct Frames : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<int> count{0};
    void OnFrame(const webrtc::VideoFrame&) override { ++count; }
  };
  Frames frames_[3];
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> counted_[3];
  void Uncount();
  void Prefer();
  void Ended(const char* what);
  void CommitScreen();
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> pending_screen_source_;
  HWND screen_window_=nullptr;
  // screen_ retains this real source behind libwebrtc's GetSource() proxy.
  webrtc::VideoTrackSourceInterface* screen_capture_=nullptr;
  bool screen_sound_=false;
  HWND pending_screen_window_=nullptr;
  bool pending_screen_sound_=false;
  uint64_t pending_screen_deadline_=0;
};

}  // namespace callmedia
