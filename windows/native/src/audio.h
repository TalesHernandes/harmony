// The microphone, delivered only to the microphone's track.
//
// libwebrtc hands what its own audio device records to EVERY audio stream
// that is sending (AudioTransportImpl::audio_senders_). With a second audio
// track — the PC's sound for a shared screen — that stream got the voice too,
// and two threads feeding it at once tripped its race check and killed the
// program (measured 06/10/2026). LiveKit solves the same thing the same way:
//
//   * MicDevice wraps the Windows audio device and forwards everything to it,
//     except that the recorded audio comes to us instead of to libwebrtc;
//   * microphone APM receives the mixed room AFTER per-person gains plus
//     W1 process loopback (other applications), as one reverse reference.
//     A separate engine APM avoids analyzing the render reference twice.
//
// Playback is untouched. The noise suppressor of W3 (DFN3) goes in here.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <filesystem>
#include "noise-filter.h"
#include "receive-mixer.h"

#include "api/audio/audio_device.h"
#include "api/audio/audio_processing.h"
#include "api/media_stream_interface.h"
#include "api/notifier.h"
#include "api/stats/rtc_stats_report.h"

namespace audio {

class MicSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
 public:
  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
  void Deliver(const int16_t* data, int rate, size_t channels, size_t frames);
  void Mute(bool muted) { muted_ = muted; }

 private:
  std::atomic<bool> muted_{false};
  std::mutex mu_;
  std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
};

// Creates the wrapped device and the processing object; both go into
// CreatePeerConnectionFactory. Must run on the engine's worker thread, where
// libwebrtc would have created its own device.
struct Pipeline {
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> device;
  webrtc::scoped_refptr<webrtc::AudioProcessing> processing;
  webrtc::scoped_refptr<webrtc::AudioProcessing> engineProcessing;
  webrtc::scoped_refptr<ReceiveMixer> mixer;
  webrtc::scoped_refptr<MicSource> mic;
  std::shared_ptr<FilterSlot> filter;
  // Session writes; the capture callback reads once per 10 ms quantum.
  std::shared_ptr<std::atomic<bool>> audience;
  std::shared_ptr<std::atomic<double>> renderLevel;
  // The loudest 10 ms played since a reader last took it (exchange with 0):
  // a once-a-second reading of renderLevel lands on a gap between sounds.
  std::shared_ptr<std::atomic<double>> renderPeak;
  std::shared_ptr<std::atomic<uint64_t>> renderAt, renderCalls, captureCalls, referenceMissing, referenceDropped, referenceAge;
  // True while the voice test plays the microphone back (Monitor below).
  std::shared_ptr<std::atomic<bool>> monitor;
  // "Ouvir minha voz" in a room or call: the processed microphone mixed into
  // the playback after the echo canceller's reference was taken. The owner of
  // the pipeline stores it off while muted. sidetonePlayed counts 10 ms played.
  std::shared_ptr<std::atomic<bool>> sidetone;
  std::shared_ptr<std::atomic<uint64_t>> sidetonePlayed;
  std::function<void()> removeTestDevice;
};
Pipeline CreatePipeline(bool testTone = false, std::unique_ptr<NoiseFilter> backend = {}, bool simulatedDevice = false,bool requireCable = false);

// "Ouvir minha voz" (Ajustes → Voz; the owner, 29/10/2026: "adicionar opcao
// de retorno de voz para saber como esta saindo a voz"). Outside a call, the
// call's own microphone chain — device, echo canceller, DFN, leveller, the
// gain slider — plays into the chosen output a few tens of milliseconds late,
// as the others would hear it before the network. The playback is never the
// echo canceller's reference (audio.cpp NeedMorePlayData). Headphones only:
// on speakers the voice comes back into the microphone.
struct MonitorSettings {
  std::string input, output;  // device GUIDs; empty = the Windows default
  bool dfn = true, echo = true, noise = true, agc = true;
  int strength = 50;
  double micGain = 1;
  bool simulatedDevice = false, requireCable = false;  // local tests only
  std::unique_ptr<NoiseFilter> backend;                // tests; empty = DFN
};
class Monitor {
 public:
  // Opens both devices on a thread of its own; returns once they are open
  // or have failed (Running()).
  explicit Monitor(MonitorSettings settings);
  ~Monitor();
  bool Running() const;
  void Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double gain);
  // 10 ms quanta played from the microphone (not silence for lack of it).
  uint64_t Played() const;
  // Times the queue overflowed (a few ms dropped) or ran dry (a click).
  uint64_t Gaps() const;
  // The loudest of them since the last call, linear 0..32767 (tests).
  double TakePeak();
  struct State;
 private:
  std::shared_ptr<State> state_;
};

// "Gravar o áudio da próxima chamada" (Ajustes → Diagnóstico; the owner,
// 25/10/2026: a friend called his voice bad, and "gravar a chamada do nativo
// pra entender melhor o que tá acontecendo"). Armed, the next call that has
// someone listening records `seconds` of captured audio into its own folder
// under `root`, then disarms itself:
//   eco.aecdump  — libwebrtc's AEC dump of the microphone processor (the
//                  Android probe's format): the microphone before and after
//                  it, and the reference it cancels against;
//   enviado.wav  — what the call really sends, after DFN and the gain slider;
//   numeros.txt  — one line a second: levels, clipping, what DFN did (blocks
//                  it filtered, fallbacks, slowest call, the reason), echo
//                  statistics, and the network as both sides report it.
// Only the two newest folders are kept. It holds everyone's voice, so it
// never leaves the computer.
struct RecordingStatus {
  enum State { Off, Armed, Recording, Saved, Failed } state = Off;
  int seconds = 0;
  std::filesystem::path folder;
};
void ArmRecording(std::filesystem::path root, int seconds = 60);
void DisarmRecording();
RecordingStatus ReadRecording();
// Rooms and direct calls hand every stats report here; a no-op unless recording.
void RecordStats(const webrtc::RTCStatsReport& report);

}  // namespace audio
