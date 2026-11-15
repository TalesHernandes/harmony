// The direct 1:1 call (PROTOCOL-HARMONY §7): P2P media, the server only
// forwards signals. Perfect negotiation (PROTOCOL.md §5–6): the caller is
// impolite, the one who answers is polite; on simultaneous offers the polite
// side rolls its own back and the impolite side ignores the other's. Only the
// caller creates transceivers — all four, sendrecv, in the fixed order
// mic, cam, screen, screen sound (web/harmony/src/direct.ts).
//
// W1 uses it for one proof: both sides renegotiating at the same instant
// (restartIce on both, scheduled to the same wall-clock millisecond) and the
// call surviving it.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/peer_connection_interface.h"
#include "json.h"
#include "net.h"

namespace direct {

class Call : public webrtc::PeerConnectionObserver {
 public:
  explicit Call(net::Client* client);
  ~Call() override;

  // Rings this person (impolite) or waits for the first call and accepts it
  // (polite).
  bool Ring(const std::string& username, std::string* error);
  void AcceptNext();
  void Hangup();
  // Renegotiates on purpose (restartIce): the glare proof.
  void RestartIce();
  void OnSocketEvent(const std::string& type, const json::Value& data);

  // Counters for the proof, read at the end.
  int offers_ignored() const { return offers_ignored_; }
  int rollbacks() const { return rollbacks_; }
  int restarts_connected() const { return restarts_connected_; }
  bool connected() const { return connected_; }
  // "stable local=<ufrag> remote=<ufrag> ice=<state>": a restart that went
  // through changes both ufrags while the state stays stable and connected.
  std::string Report();

 private:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnNegotiationNeededEvent(uint32_t event_id) override;
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override;

  void Post(std::function<void()> task);
  void Loop();
  bool Build(bool polite, const json::Value& ice);
  void Send(const std::string& kind, const json::Value& payload);
  // The local description as JSON, read on libwebrtc's signalling thread:
  // the description objects are only safe there (peer_connection_interface.h).
  json::Value LocalJson();
  // Ends the media: the server's "ended" and our own hangup both close it.
  void CloseConnection();
  void Signal(const json::Value& data);
  void FlushOutbox();

  net::Client* client_;
  std::string id_;
  bool polite_ = false;
  bool live_ = false;  // answered: signals may go straight to the other side
  bool accept_next_ = false;
  std::vector<std::pair<std::string, json::Value>> outbox_;
  bool making_offer_ = false;
  bool ignore_offer_ = false;
  // Written on the call's thread, read by main while it waits.
  std::atomic<int> offers_ignored_{0}, rollbacks_{0}, restarts_connected_{0};
  std::atomic<bool> connected_{false};
  bool restarting_ = false;

  std::thread thread_;
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;

  std::unique_ptr<webrtc::Thread> network_, worker_, signaling_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  // The Windows audio device, made and let go on the worker thread: left to
  // the factory it was released elsewhere at exit and crashed in its
  // destructor about one run in three (06/10/2026).
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> mic_;
};

}  // namespace direct
