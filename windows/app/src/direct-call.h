#pragma once
#include <optional>
#include "../../native/src/net.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace webrtc { class VideoTrackInterface; }
namespace app {
struct CallPerson { std::string username, displayName; };
enum class CallPhase { Idle, Calling, Ringing, Connecting, Live };
struct CallView {
  CallPhase phase = CallPhase::Idle;
  std::string id; CallPerson peer;
  bool polite = false, muted = false;
  bool deafened = false;   // not hearing the other side; the microphone goes off with it
  bool cam = false, screen = false;   // what this side is sending (W6, 2nd part)
  bool peerMuted = false, peerCam = false, peerScreen = false;
  bool mediaConnected = false;
  int64_t ringingSince = 0, liveSince = 0;
  std::string notice;
  uint64_t sequence = 0;
};
class DirectMedia {
 public:
  virtual ~DirectMedia() = default;
  // Only the impolite caller creates the four slots. Never log iceServers.
  virtual bool Open(bool polite, const json::Value& iceServers, std::string* error) = 0;
  virtual void Remote(const std::string& kind, const json::Value& payload) = 0;
  virtual void Mute(bool muted) = 0;
  virtual void Deafen(bool) {}
  virtual void Volumes(double, double) {}
  virtual void Close() = 0;
  // Camera and screen go into slots negotiated at the start (replaceTrack).
  // window and monitor are HWND and HMONITOR, kept opaque for the tests.
  virtual void Camera(bool, const std::string&) {}
  virtual void Screen(void*, void*, int, int, bool) {}
  virtual void StopScreen() {}
  // The other side's camera (slot 1) or screen (slot 2); attach runs on the
  // media's thread with the track or nullptr.
  virtual void Video(int, std::function<void(webrtc::VideoTrackInterface*)>) {}
  virtual void Device(bool, const std::string&) {}
  virtual void Filters(bool, int, bool, bool, bool, double) {}
  virtual void Sidetone(bool) {}
};
using DirectMediaFactory = std::function<std::unique_ptr<DirectMedia>(
    std::function<void(std::string, json::Value)> send, std::function<void(bool)> status)>;
// All methods except View/MuteImmediately run on ONE Service worker thread. The media adapter
// must enqueue send/status callbacks onto that worker before invoking them (or
// invoking MediaSignal/MediaStatus). Never invoke model methods on a media thread.
// View is safe on any thread. HTTP, media and changed callbacks run without mu_.
// Destroy on the worker, after stopping/draining queued adapter callbacks.
class DirectCall {
 public:
  using Transport = std::function<net::Response(const std::string&, const std::string&, const json::Value*)>;
  using Clock = std::function<int64_t()>;
  DirectCall(Transport, DirectMediaFactory, std::function<void()> changed = {}, Clock = {});
  ~DirectCall();
  CallView View() const;
  void Ring(const CallPerson& who);
  void Accept(); void Decline(); void Hangup(); void Mute(bool muted); void Deafen(bool deafened);
  // The UI may cut audio while the service worker is blocked in HTTP.
  uint64_t MuteImmediately(bool muted);
  void CommitMute(uint64_t epoch);
  bool Event(const std::string& type, const json::Value& data);
  void Open(const std::string& id);
  void MediaSignal(std::string kind, json::Value payload);
  void MediaStatus(bool connected);
  void Tick(); void Disconnect();
  std::optional<int64_t> NextTickDelay() const;
  // After the gateway reconnects: recover an accept or an end it missed.
  void Reconcile();
  void Camera(bool on, const std::string& id);
  void Screen(const std::string& id, void* window, void* monitor, int height, int fps, bool sound);
  void StopScreen();
  void Video(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach);
  void Device(bool capture, const std::string& id);
  void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double micGain);
  void Sidetone(bool on);
  // The person's voice and screen-sound volumes, read when the media opens and
  // again by VolumesChanged (the room's choice for them, shared).
  std::function<std::pair<double, double>(const std::string& peer)> volumes;
  void VolumesChanged();
 private:
  void Set(CallView view);
  void Finish(std::string notice = {});
  void End(const std::string& reason);
  void PostEnd(const std::string& id, const std::string& reason);
  bool StartMedia(const json::Value& ice);
  void Signal(const std::string& kind, const json::Value& payload);
  void PostState(const CallView& v);
  void CaptureEnded(const json::Value& payload);
  std::function<void(webrtc::VideoTrackInterface*)> videos_[5];
  bool mutedBeforeDeaf_ = false;
  Transport transport_; DirectMediaFactory factory_; std::function<void()> changed_; Clock clock_;
  mutable std::mutex mu_; CallView view_;
  std::unique_ptr<DirectMedia> media_;
  std::mutex muteMu_; // publication only; never held during Open/Close/HTTP
  DirectMedia* muteMedia_ = nullptr;
  bool requestedMute_ = false, muteActive_ = false;
  uint64_t muteEpoch_ = 1;
  std::string cameraId_;
  bool answered_ = false, everConnected_ = false;
  uint64_t generation_ = 0;
  std::vector<std::pair<std::string, json::Value>> outbox_;
};
int DirectSelfTest(const std::filesystem::path& output, const std::string& base);
}
