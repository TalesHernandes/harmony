// The sound of the PC, minus harmony itself, as the screen's second audio
// track (PLANO-WINDOWS §2.3). Windows' process loopback (build 20348+, i.e.
// Windows 11) captures every process EXCEPT this one's tree, so the voices of
// the room never go back into the film's sound.
//
// It is a libwebrtc audio source of its own: its audio goes straight to the
// encoder, outside the echo canceller and the noise suppressor, which would
// chew music and explosions as if they were background noise.
#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "api/media_stream_interface.h"
#include "api/notifier.h"

namespace loopback {

// Carry the first sample's capture time through 10-ms splitting. A delayed
// delivery may contain many blocks; they must not all acquire delivery time.
class CaptureBlocks {
 public:
  template<class Deliver>
  void Push(const int16_t* data, size_t frames, std::optional<int64_t> capture_ns,
            bool discontinuity, Deliver deliver) {
    if (discontinuity) used_ = 0;
    size_t consumed = 0;
    while (consumed < frames) {
      if (!used_) first_ns_ = capture_ns ? std::optional<int64_t>(*capture_ns + int64_t(consumed) * 1000000000LL / 48000) : std::nullopt;
      if (!capture_ns) first_ns_.reset();
      size_t take = std::min(size_t(480) - used_, frames - consumed);
      if (data) std::copy_n(data + consumed * 2, take * 2, samples_.data() + used_ * 2);
      else std::fill_n(samples_.data() + used_ * 2, take * 2, int16_t(0));
      consumed += take; used_ += take;
      if (used_ == 480) {
        deliver(samples_.data(), first_ns_ ? std::optional<int64_t>(*first_ns_ / 1000000) : std::nullopt);
        used_ = 0;
      }
    }
  }
 private:
  std::array<int16_t, 960> samples_{};
  size_t used_ = 0;
  std::optional<int64_t> first_ns_;
};

class SystemAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
 public:
  // nullptr when Windows does not offer process loopback (Windows 10).
  static webrtc::scoped_refptr<SystemAudioSource> Create(uint32_t target_process = 0, bool screen_probe = false);
  ~SystemAudioSource() override;

  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;

 protected:
  SystemAudioSource() = default;

 private:
  int64_t probe_stream_ = 0;
  bool screen_probe_ = false;
  bool Start(uint32_t target_process);
  void Run();

  std::mutex mu_;
  std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  void* client_ = nullptr;   // IAudioClient*
  void* capture_ = nullptr;  // IAudioCaptureClient*
  void* event_ = nullptr;    // HANDLE
};

}  // namespace loopback
