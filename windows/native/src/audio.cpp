#include "modules/audio_device/include/audio_device_factory.h"
#include "audio.h"
#include "dfn-filter.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <condition_variable>
#include <map>
#include <cstring>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/stats/rtcstats_objects.h"
#include "rtc_base/event.h"
#include "modules/audio_processing/aec_dump/aec_dump_factory.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "log.h"
#include "perf.h"
#include "loopback.h"
#include "common_audio/resampler/include/push_resampler.h"
#include <array>
#include <deque>
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <mutex>
#include "echo-reference.h"
#ifndef HARMONY_PUBLISH
#include "controlled-adm.h"
#endif
#ifdef HARMONY_ENGINE_TEST
#include "engine-test-control.h"
#endif

namespace audio {

void MicSource::AddSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) sinks_.push_back(sink);
}

void MicSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void MicSource::Deliver(const int16_t* data, int rate, size_t channels, size_t frames) {
  std::lock_guard<std::mutex> lock(mu_);
  if (muted_) return;
  for (auto* sink : sinks_) sink->OnData(data, 16, rate, channels, frames);
}

namespace {

// Reference only: other applications never become microphone publications.
class ExternalReference : public webrtc::AudioTrackSinkInterface {
 public:
  static uint64_t Now() {
#ifdef HARMONY_ENGINE_TEST
    return engine_test::now;
#else
    return GetTickCount64();
#endif
  }
  void OnData(const void* data, int bits, int rate, size_t channels, size_t frames) override {
    if (bits != 16 || rate != 48000 || channels != 2 || frames != 480) return;
    std::lock_guard lock(mu_);
    if (queue_.size() == 8) { queue_.pop_front(); ++dropped; }
    Block block; std::copy_n(static_cast<const int16_t*>(data),960,block.pcm.begin());
    block.at = Now(); queue_.push_back(block);
  }
  std::array<int16_t,960> Read() {
    std::lock_guard lock(mu_);
    // Align to this render quantum: retain order, but never feed a reference
    // older than the bounded 80 ms horizon. Each render consumes exactly 10 ms.
    auto now = Now();
    while (!queue_.empty() && now-queue_.front().at >= 80) { queue_.pop_front(); ++dropped; }
    if (queue_.empty()) { ++missing; return {}; }
    auto block = queue_.front(); queue_.pop_front(); age = now-block.at;
    return block.pcm;
  }
 private:
  struct Block { std::array<int16_t,960> pcm{}; uint64_t at=0; };
  std::mutex mu_; std::deque<Block> queue_;
 public:
  std::atomic<uint64_t> missing{0}, dropped{0}, age{0};
};

// The recording of ArmRecording. The disk is touched only on its own queue:
// the capture callback claims a recording, copies samples and posts them.
webrtc::TaskQueueBase* RecordingQueue() {
  // Process lifetime on purpose: an AecDump's tasks run on this queue and must
  // outlive it, and the pipeline holding the dump can end at any moment.
  static webrtc::TaskQueueBase* queue = webrtc::CreateDefaultTaskQueueFactory()
      ->CreateTaskQueue("harmony-recording", webrtc::TaskQueueFactory::Priority::LOW).release();
  return queue;
}
struct Wave {
  FILE* file = nullptr; uint32_t bytes = 0; bool failed = false;
  void Header() {
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, file); }; auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, file); };
    fwrite("RIFF", 1, 4, file); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, file); u32(16); u16(1); u16(1);
    u32(48000); u32(48000 * 2); u16(2); u16(16); fwrite("data", 1, 4, file); u32(bytes);
  }
  // The header follows every write, so a programme closed mid-recording still
  // leaves a playable file up to its last tenth of a second.
  void Write(const std::vector<int16_t>& pcm) {
    if (!file) return;
    size_t written = fwrite(pcm.data(), 2, pcm.size(), file);
    failed |= written != pcm.size(); bytes += uint32_t(written * 2);
    fseek(file, 0, SEEK_SET); Header(); fseek(file, 0, SEEK_END);
  }
  void Close() { if (!file) return; failed |= ferror(file) != 0; failed |= fclose(file) != 0; file = nullptr; }
};
struct Recording {
  std::filesystem::path root, folder;
  std::wstring name;
  std::string header;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
  int limit = 0;
  std::atomic<bool> ready{false}, stop{false}, finishing{false};
  std::atomic<int> quanta{0};
  // Capture thread only.
  std::vector<int16_t> pending;
  double inSquares = 0, outSquares = 0; int inPeak = 0, outPeak = 0, inClipped = 0, outClipped = 0, block = 0;
  // DFN over the second: blocks it filtered, calls that fell back and why,
  // and the slowest call. The level columns alone could not tell a call
  // where DFN never once ran from one where it ran (03/11/2026).
  int dfnBlocks = 0, dfnFailures = 0, dfnLate = 0; uint32_t dfnSlowest = 0;
  std::array<int, FilterSlot::Alone + 1> dfnWhy{};
  // Recording queue only.
  Wave sent; FILE* numbers = nullptr; bool attached = false;
  // Stats threads, under the recorder's lock. Per stream: people come and go,
  // and a sum over a changing set is not a difference of anything.
  struct Inbound { int64_t lost = 0; uint64_t samples = 0, concealed = 0, emitted = 0; double delay = 0; };
  uint64_t statsAt = 0;
  std::map<std::string, uint64_t> outPrevious;
  std::map<std::string, Inbound> inPrevious;
  std::weak_ptr<Recording> self;
  void Line(std::string line) {
    RecordingQueue()->PostTask([rec = self.lock(), line = std::move(line)] { if (rec->numbers) fputs(line.c_str(), rec->numbers); });
  }
  double Seconds() const { return quanta / 100.0; }
};
struct Recorder {
  std::mutex mu;
  bool armed = false; int seconds = 60; std::filesystem::path root;
  // From the claim until its files are closed: one recording at a time, end
  // to end, so a new one never meets the previous one's dump on the same APM.
  std::shared_ptr<Recording> current;
  RecordingStatus status;
  std::atomic<bool> armedFast{false}, currentFast{false};
};
// Never destroyed: queue tasks and the detaching thread may still run while
// the programme exits.
Recorder& TheRecorder() { static auto* recorder = new Recorder; return *recorder; }
double Dbfs(double rms) { return rms > 0 ? 20 * std::log10(rms / 32768.0) : -120; }
std::string Fixed(std::optional<double> v, const char* format = "%.1f") {
  if (!v) return "-"; char text[32]; snprintf(text, sizeof text, format, *v); return text;
}
// Keeps the newest two recordings; every folder holds everyone's voice.
void PruneRecordings(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> folders; std::error_code error;
  for (auto& entry : std::filesystem::directory_iterator(root, error))
    if (entry.is_directory() && entry.path().filename().wstring().starts_with(L"chamada-")) folders.push_back(entry.path());
  std::sort(folders.begin(), folders.end());
  for (size_t i = 0; i + 2 < folders.size(); ++i) std::filesystem::remove_all(folders[i], error);
}
// Recording queue: closes the files and lets the next recording start.
void CloseRecording(const std::shared_ptr<Recording>& rec, bool opened) {
  rec->sent.Close();
  bool failed = !opened || rec->sent.failed;
  if (rec->numbers) { failed |= ferror(rec->numbers) != 0; failed |= fclose(rec->numbers) != 0; rec->numbers = nullptr; }
  if (opened) PruneRecordings(rec->root);
  auto& r = TheRecorder();
  std::lock_guard lock(r.mu);
  if (r.current == rec) { r.current = nullptr; r.currentFast = false; }
  r.status = {failed ? RecordingStatus::Failed : RecordingStatus::Saved, int(rec->Seconds()), rec->folder};
  logf("audio recording %s seconds=%d", failed ? "failed" : "saved", int(rec->Seconds()));
}
// Recording queue: the folder, the three files, and the dump on the APM.
void OpenRecording(const std::shared_ptr<Recording>& rec) {
  std::error_code error; std::filesystem::create_directories(rec->root, error);
  // A folder of its own even when two start within the same second.
  for (int n = 1; n < 100 && rec->folder.empty(); ++n) {
    auto folder = rec->root / (rec->name + (n > 1 ? L"-" + std::to_wstring(n) : L""));
    if (std::filesystem::create_directory(folder, error)) rec->folder = folder;
    else if (error) break;
  }
  FILE* dumpFile = rec->folder.empty() ? nullptr : _wfopen((rec->folder / L"eco.aecdump").c_str(), L"wb");
  // 40 MB, as the Android probe: a minute of 48 kHz takes about 25 MB.
  auto dump = dumpFile ? webrtc::AecDumpFactory::Create(dumpFile, 40 << 20, RecordingQueue()) : nullptr;
  if (dump) {
    rec->sent.file = _wfopen((rec->folder / L"enviado.wav").c_str(), L"wb");
    rec->numbers = _wfopen((rec->folder / L"numeros.txt").c_str(), L"wb");
  }
  if (!dump || !rec->sent.file || !rec->numbers) { logf("audio recording could not open its files"); CloseRecording(rec, false); return; }
  rec->sent.Header();
  fputs(rec->header.c_str(), rec->numbers);
  // No dump was attached before (one recording at a time), so attaching
  // replaces nothing and waits for nothing on this queue.
  rec->apm->AttachAecDump(std::move(dump));
  rec->attached = true; rec->ready = true;
}
// Capture thread: claims the armed recording for this APM; the files open
// on the queue, and quanta are recorded once they are.
std::shared_ptr<Recording> ClaimRecording(webrtc::AudioProcessing* apm, uint32_t deviceRate, size_t deviceChannels, const FilterSlot& filter) {
  auto& r = TheRecorder();
  std::lock_guard lock(r.mu);
  if (!r.armed || r.current) return {};
  r.armed = false; r.armedFast = false;
  SYSTEMTIME now{}; GetLocalTime(&now);
  wchar_t name[64]; swprintf_s(name, L"chamada-%04d-%02d-%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  auto rec = std::make_shared<Recording>(); rec->self = rec;
  rec->root = r.root; rec->name = name; rec->limit = r.seconds * 100; rec->apm = webrtc::scoped_refptr<webrtc::AudioProcessing>(apm);
  char head[600];
  snprintf(head, sizeof head,
           "harmony nativo · gravação da chamada\n"
           "microfone %u Hz %zu canais · eco=%d ruído=%d nivelar=%d DFN=%d força=%d ganho=%.0f%%\n\n"
           "segundo | entrada dBFS (média pico estouros) | enviado dBFS (média pico estouros) | filtro | "
           "DFN: blocos_filtrados/100 falhas atrasos_tolerados maior_ms motivo | "
           "eco: ERL ERLE atraso_ms divergência eco_residual | atraso informado ms\n"
           "segundo rede | ida_volta_ms rota | envio kbit/s alvo | o outro recebe: perda%% jitter_ms | "
           "você recebe: perdidos jitter_ms buracos%% espera_ms\n\n",
           deviceRate, deviceChannels, filter.echo.load(), filter.noise.load(), filter.agc.load(), filter.enabled.load(),
           filter.strength.load(), filter.micGain.load() * 100);
  rec->header = head;
  r.current = rec; r.currentFast = true;
  r.status = {RecordingStatus::Recording, 0, {}};
  RecordingQueue()->PostTask([rec] { OpenRecording(rec); });
  logf("audio recording started seconds=%d", r.seconds);
  return rec;
}
void FinishRecording(std::shared_ptr<Recording> rec) {
  if (!rec || rec->finishing.exchange(true)) return;
  {
    auto& r = TheRecorder();
    std::lock_guard lock(r.mu);
    if (r.current == rec) r.currentFast = false;  // no more network lines
  }
  if (!rec->pending.empty()) RecordingQueue()->PostTask([rec, pcm = std::move(rec->pending)] { rec->sent.Write(pcm); });
  // Off the capture thread: the dump's destructor waits for its queued
  // writes. Queued after OpenRecording, so `attached` is settled by then.
  std::thread([rec] {
    webrtc::Event opened;
    RecordingQueue()->PostTask([&opened] { opened.Set(); });
    opened.Wait(webrtc::Event::kForever);
    if (rec->attached) rec->apm->DetachAecDump();
    RecordingQueue()->PostTask([rec] { if (rec->attached) CloseRecording(rec, true); });
  }).detach();
}
// What DFN did with one quantum: `tried` when it was handed the block (as the
// published filter or as a recovery probe), `failed` when it gave it back.
struct DfnQuantum {
  bool tried = false, failed = false, published = false, late = false; FilterSlot::Reason reason = FilterSlot::Warming; uint32_t micros = 0;
  // Right after FilterSlot::Process. Switched off in Ajustes is not a failure.
  void Took(const FilterSlot& slot, bool ok) {
    reason = slot.reason.load(); micros = slot.lastMicros.load();
    tried = slot.enabled.load(); failed = tried && !ok; late = ok && slot.lastLate.load();
  }
};
// Capture thread, every quantum once the files are open. `raw` is the
// device's own samples (clipping happens there), `out` what the track sends.
void RecordQuantum(Recording& rec, const int16_t* raw, size_t rawCount, const std::vector<int16_t>& out,
                   const char* effective, uint32_t delayMs, const DfnQuantum& dfn) {
  for (size_t i = 0; i < rawCount; ++i) {
    int v = raw[i], a = std::abs(v); rec.inPeak = std::max(rec.inPeak, a);
    if (v >= 32767 || v <= -32768) ++rec.inClipped;
    rec.inSquares += double(v) * v / double(rawCount) * 480;
  }
  for (auto v : out) {
    int a = std::abs(int(v)); rec.outPeak = std::max(rec.outPeak, a);
    if (v >= 32767 || v <= -32768) ++rec.outClipped;
    rec.outSquares += double(v) * v;
  }
  if (dfn.published) ++rec.dfnBlocks;
  if (dfn.tried) rec.dfnSlowest = std::max(rec.dfnSlowest, dfn.micros);
  if (dfn.failed) { ++rec.dfnFailures; ++rec.dfnWhy[dfn.reason]; }
  if (dfn.late) ++rec.dfnLate;
  rec.pending.insert(rec.pending.end(), out.begin(), out.end());
  if (rec.pending.size() >= 4800) {
    RecordingQueue()->PostTask([self = rec.self.lock(), pcm = std::move(rec.pending)] { self->sent.Write(pcm); });
    rec.pending.clear();
  }
  ++rec.quanta;
  if (++rec.block < 100) return;
  auto stats = rec.apm->GetStatistics(true);
  auto opt = [](std::optional<int32_t> v) { return v ? std::optional<double>(*v) : std::nullopt; };
  // The reason most failures gave this second; with none, the filter's own
  // state at the end of it ("ativo", "verificando 20 blocos estáveis", ...).
  FilterSlot::Reason why = dfn.reason;
  if (rec.dfnFailures) why = FilterSlot::Reason(std::max_element(rec.dfnWhy.begin(), rec.dfnWhy.end()) - rec.dfnWhy.begin());
  char line[512];
  snprintf(line, sizeof line, "%3.0f | %6.1f %6.1f %4d | %6.1f %6.1f %4d | %s | %3d %3d %3d %5.1f %s | %s %s %s %s %s | %u\n",
           rec.Seconds(), Dbfs(std::sqrt(rec.inSquares / (100 * 480))), Dbfs(rec.inPeak), rec.inClipped,
           Dbfs(std::sqrt(rec.outSquares / (100 * 480))), Dbfs(rec.outPeak), rec.outClipped, effective,
           rec.dfnBlocks, rec.dfnFailures, rec.dfnLate, rec.dfnSlowest / 1000.0, FilterSlot::ReasonText(why),
           Fixed(stats.echo_return_loss).c_str(), Fixed(stats.echo_return_loss_enhancement).c_str(),
           Fixed(opt(stats.delay_ms), "%.0f").c_str(), Fixed(stats.divergent_filter_fraction, "%.2f").c_str(),
           Fixed(stats.residual_echo_likelihood, "%.2f").c_str(), delayMs);
  rec.Line(line);
  rec.block = 0; rec.inSquares = rec.outSquares = 0; rec.inPeak = rec.outPeak = rec.inClipped = rec.outClipped = 0;
  rec.dfnBlocks = rec.dfnFailures = rec.dfnLate = 0; rec.dfnSlowest = 0; rec.dfnWhy.fill(0);
}

// The microphone's processed 10 ms blocks on their way back to the ears, for
// "Ouvir minha voz" alone (MonitorPlayback) and in a call (Transport's
// sidetone). Playing starts once one block is there and at most 20 ms wait:
// the owner heard the first version's 20-60 ms on top of DFN's own 30 ms,
// and asked for less again on 29/10/2026.
class VoiceQueue {
 public:
  std::atomic<uint64_t> played{0}, trimmed{0}, starved{0};
  std::atomic<double> peak{0};
  void Push(const int16_t* pcm) {
    std::lock_guard lock(mu_);
    queue_.insert(queue_.end(), pcm, pcm + 480);
    if (queue_.size() > kMost) { queue_.erase(queue_.begin(), queue_.begin() + (queue_.size() - kMost)); ++trimmed; }
  }
  void Clear() { std::lock_guard lock(mu_); queue_.clear(); primed_ = started_ = false; }
  // One output pull's worth of voice at the device's rate, mono, in scaled;
  // false when there is none (nothing yet, or it ran dry).
  bool Pop(size_t frames, uint32_t rate, std::vector<int16_t>& scaled) {
    std::array<int16_t, 480> block{};
    {
      std::lock_guard lock(mu_);
      // The microphone opens before the output: what piled up meanwhile is
      // old voice, and the overflow it caused was never heard.
      if (!started_) { started_ = true; if (queue_.size() > kStart) queue_.erase(queue_.begin(), queue_.end() - kStart); trimmed = 0; }
      if (queue_.size() >= kStart) primed_ = true;
      if (!primed_ || queue_.size() < block.size()) { if (primed_) ++starved; primed_ = false; return false; }  // wait for a whole block again
      std::copy_n(queue_.begin(), block.size(), block.begin());
      queue_.erase(queue_.begin(), queue_.begin() + block.size());
    }
    scaled.assign(frames, 0);
    if (rate == 48000 && frames == 480) std::copy(block.begin(), block.end(), scaled.begin());
    else resampler_.Resample(webrtc::InterleavedView<const int16_t>(block.data(), 480, 1),
                             webrtc::InterleavedView<int16_t>(scaled.data(), frames, 1));
    double loudest = 0; for (auto s : block) loudest = std::max(loudest, std::abs(double(s)));
    double seen = peak.load(); while (loudest > seen && !peak.compare_exchange_weak(seen, loudest)) {}
    ++played;
    return true;
  }

 private:
  static constexpr size_t kStart = 480, kMost = 960;
  std::mutex mu_;
  std::deque<int16_t> queue_;
  bool primed_ = false, started_ = false;
  webrtc::PushResampler<int16_t> resampler_;
};

// Stands between the Windows device and libwebrtc's transport: playback goes
// through untouched (the engine mixes the room and feeds the echo canceller
// the reference); recording is processed here and goes to the mic track only.
class Transport : public webrtc::AudioTransport {
 public:
  Transport(webrtc::AudioProcessing* apm, MicSource* mic, std::shared_ptr<FilterSlot> filter,
            std::shared_ptr<std::atomic<double>> renderLevel, std::shared_ptr<std::atomic<uint64_t>> renderAt,
            std::shared_ptr<std::atomic<uint64_t>> renderCalls, bool testTone, bool externalReference = true,
            std::shared_ptr<std::atomic<bool>> audience = {})
      : apm_(apm), mic_(mic), filter_(std::move(filter)), render_level_(std::move(renderLevel)), render_at_(std::move(renderAt)), render_calls_(std::move(renderCalls)), test_tone_(testTone) {
    audience_=std::move(audience);
    if (!testTone && externalReference) {
      external_ = loopback::SystemAudioSource::Create();
      if (external_) external_->AddSink(&reference_);
      logf("audio external AEC reference %s", external_ ? "active; reference only" : "unavailable; room render only");
    }
  }
  ~Transport() override { FinishRecording(std::exchange(recording_, nullptr)); if (external_) external_->RemoveSink(&reference_); external_ = nullptr; }
  void SetEngine(webrtc::AudioTransport* engine) {
    std::lock_guard<std::mutex> lock(mu_);
    engine_ = engine;
  }

#ifdef HARMONY_ENGINE_TEST
  void ReferenceForTest(const int16_t* data) { reference_.OnData(data,16,48000,2,480); }
#endif
  int32_t RecordedDataIsAvailable(const void* samples, size_t frames, size_t bytes_per_sample, size_t channels,
                                  uint32_t rate, uint32_t delay_ms, int32_t, uint32_t level, bool,
                                  uint32_t& new_level) override {
    static thread_local bool named = false;
    if (!named) {
      perf::NameThread(L"harmony-mic");
      named = true;
    }
    static perf::Stage& stage = perf::Get("audio.mic");
    auto timer = stage.Begin();
    new_level = level;
    ++*capture_calls_;
    if (test_tone_) return 0;
    if (!samples || !channels || rate < 8000 || rate > 192000 ||
        frames != size_t(rate / 100) || bytes_per_sample != channels * sizeof(int16_t)) {
      filter_->active=false; filter_->reason=FilterSlot::Format; dfn_ready_=false; good_blocks_=0;
      ReportNoise("raw", FilterSlot::Format); return 0;
    }
    // ADM PCM is signed 16-bit regardless of the endpoint's WASAPI mix format.
    // Downmix before resampling, so every capture rate/channel layout reaches
    // APM and DFN as exactly one 10 ms mono quantum at 48 kHz.
    const auto* in = static_cast<const int16_t*>(samples);
    mono_.resize(frames);
    for (size_t f=0; f<frames; ++f) {
      int64_t sum=0;
      for (size_t c=0; c<channels; ++c) sum+=in[f*channels+c];
      mono_[f]=int16_t(sum/int64_t(channels));
    }
    normalized_.resize(480); out_.resize(480);
    capture_resampler_.Resample(webrtc::InterleavedView<const int16_t>(mono_.data(),frames,1),
                               webrtc::InterleavedView<int16_t>(normalized_.data(),480,1));
    webrtc::StreamConfig config(48000, 1);
    // Only the capture callback owns configuration and the recovery streak.
    // ApplyConfig is never concurrent with ProcessStream on another thread.
    const bool audience = !audience_ || audience_->load();
    if (!filter_->enabled || !audience) { dfn_ready_=false; good_blocks_=0; }
    SetNoiseSuppression(filter_->noise && !dfn_ready_);
    apm_->set_stream_delay_ms(int(delay_ms));
    // In-place processing also covers APM's no-processing fast path: it
    // must publish this quantum, never the previous DFN output buffer.
    out_=normalized_;
    bool processed=apm_->ProcessStream(out_.data(), config, config, out_.data())==0;
    filter_->nsApplied=processed && ns_enabled_;
    if (!processed) { out_=normalized_; filter_->active=false; filter_->reason=FilterSlot::Apm; dfn_ready_=false; good_blocks_=0; }
    bool clean=false;
    DfnQuantum dfn;
    if (!audience) {
      // Keep APM and microphone publication (including the own speaking meter)
      // alive, but do not even probe DFN when nobody can hear the result.
      filter_->active=false;
      filter_->reason=filter_->enabled ? FilterSlot::Alone : FilterSlot::Disabled;
    } else if (dfn_ready_) {
      clean=filter_->Process(out_, 48000, 1, -1, true, LateIsIsolated());
      dfn.Took(*filter_, clean); dfn.published=clean;
      CountLate(dfn.tried);
      if (!clean) {
        dfn_ready_=false; good_blocks_=0;
        // This quantum can be unsuppressed; NS protects the very next one.
        // Do not run AEC/AGC twice on the same block to disguise that gap.
        SetNoiseSuppression(filter_->noise);
      }
    } else {
      probe_.assign(out_.begin(), out_.end());
      bool good=filter_->Process(probe_, 48000, 1, -1, false, LateIsIsolated());
      dfn.Took(*filter_, good);
      CountLate(dfn.tried);
      // A tolerated late probe keeps the streak but does not add to it.
      good_blocks_=good ? good_blocks_+(dfn.late ? 0 : 1) : 0;
      // Discard probes: no published block ever receives both NS and DFN.
      dfn_ready_=good_blocks_>=kDfnRecoveryBlocks;
    }
    const char* effective=clean ? "DFN3" : (filter_->nsApplied.load() ? "libwebrtc NS" : "raw");
    if (!processed) { filter_->active=false; filter_->reason=FilterSlot::Apm; }
    ReportNoise(effective, filter_->reason.load());
    for(auto& sample:out_) sample=int16_t(std::clamp(double(sample)*filter_->micGain.load(),-32768.,32767.));
    mic_->Deliver(out_.data(), 48000, 1, 480);
    const bool sidetone=sidetone_ && sidetone_->load();
    if (sidetone) side_.Push(out_.data()); else if (side_on_) side_.Clear();
    side_on_=sidetone;
    if (!recording_ && audience && TheRecorder().armedFast.load(std::memory_order_relaxed))
      recording_=ClaimRecording(apm_, rate, channels, *filter_);
    if (recording_) {
      if (recording_->ready) {
        if (!dfn.tried) dfn.reason=filter_->reason.load();
        RecordQuantum(*recording_, in, frames*channels, out_, effective, delay_ms, dfn);
      }
      if (recording_->quanta>=recording_->limit || recording_->stop) FinishRecording(std::exchange(recording_, nullptr));
    }
    timer.Done();
    return 0;
  }

  int32_t NeedMorePlayData(size_t frames, size_t bytes_per_sample, size_t channels, uint32_t rate, void* samples,
                           size_t& frames_out, int64_t* elapsed_ms, int64_t* ntp_ms) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!engine_) {
      memset(samples, 0, frames * bytes_per_sample);
      frames_out = frames;
      return 0;
    }
    auto result = engine_->NeedMorePlayData(frames, bytes_per_sample, channels, rate, samples, frames_out, elapsed_ms, ntp_ms);
    auto pcm = static_cast<int16_t*>(samples);
    auto produced=RenderedFrames(frames,channels,frames_out);
    // The voice test plays the speaker's own voice back: as the canceller's
    // reference it would be cancelled out of the microphone and the test
    // would lie. Other programmes' sound still is the reference.
    if (monitor_ && monitor_->load()) { silence_.assign(produced*channels,0); AnalyzeReference(silence_.data(), produced, channels, rate); return result; }
    MeasureRender(pcm,produced,channels);
    AnalyzeReference(pcm, produced, channels, rate);
    // The call's sidetone goes in after the reference was taken, for the same
    // reason as the voice test's playback, and after the level the room's
    // meters read: it is the speaker's own voice, not the room.
    if (sidetone_ && sidetone_->load() && bytes_per_sample==channels*sizeof(int16_t) && side_.Pop(produced, rate, side_scaled_)) {
      if (sidetone_played_) ++*sidetone_played_;
      for (size_t f=0; f<produced; ++f) for (size_t c=0; c<channels; ++c) {
        auto& s=pcm[f*channels+c]; s=int16_t(std::clamp(int(s)+side_scaled_[f],-32768,32767));
      }
    }
#ifdef HARMONY_ENGINE_TEST
    if(engine_test::rendered) engine_test::rendered(pcm,produced*channels);
#endif
    return result;
  }

  void PullRenderData(int bits, int rate, size_t channels, size_t frames, void* data, int64_t* elapsed_ms,
                      int64_t* ntp_ms) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (engine_) {
      engine_->PullRenderData(bits, rate, channels, frames, data, elapsed_ms, ntp_ms);
      if (bits == 16) { auto pcm=static_cast<int16_t*>(data); MeasureRender(pcm,frames,channels); AnalyzeReference(pcm,frames,channels,rate); }
    }
  }

 private:
  void ReportNoise(const char* effective, FilterSlot::Reason reason) {
    if (effective_!=effective || reason_!=reason) {
      logf("audio noise=%s reason=%s",effective,FilterSlot::ReasonText(reason));
      effective_=effective; reason_=reason;
    }
  }
  void SetNoiseSuppression(bool enabled) {
    if (config_applied_ && enabled==ns_enabled_ && echo_enabled_==filter_->echo && agc_enabled_==filter_->agc) return;
    auto config=apm_->GetConfig();
    config.echo_canceller.enabled=filter_->echo;
    config.gain_controller2.enabled=filter_->agc;
    config.noise_suppression.enabled=enabled;
    config.noise_suppression.level=webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    apm_->ApplyConfig(config);
    config_applied_=true; ns_enabled_=enabled; echo_enabled_=filter_->echo; agc_enabled_=filter_->agc;
  }
  void MeasureRender(const int16_t* samples,size_t frames,size_t channels) {
    double sum=0; for(size_t i=0;i<frames*channels;++i) sum+=std::abs(int(samples[i]))/256.0;
    double level=frames&&channels ? sum/(frames*channels):0;
    *render_level_=level; *render_at_=GetTickCount64(); ++*render_calls_;
    if(render_peak_) { double seen=render_peak_->load(); while(level>seen && !render_peak_->compare_exchange_weak(seen,level)) {} }
  }
  void AnalyzeReference(const int16_t* room, size_t frames, size_t channels, int rate) {
    if (frames != size_t(rate / 100) || (channels != 1 && channels != 2)) return;
    auto external = reference_.Read(); std::array<int16_t,960> converted{};
    *reference_missing_=reference_.missing.load(); *reference_dropped_=reference_.dropped.load(); *reference_age_=reference_.age.load();
    for(size_t i=0;i<480;++i) for(size_t c=0;c<channels;++c)
      converted[i*channels+c] = channels==2 ? external[i*2+c] : int16_t((int(external[i*2])+external[i*2+1])/2);
    external_scaled_.resize(frames*channels); reverse_.resize(frames*channels);
    reference_resampler_.Resample(webrtc::InterleavedView<const int16_t>(converted.data(),480,channels),
                                 webrtc::InterleavedView<int16_t>(external_scaled_.data(),frames,channels));
    MixEchoReference(room,external_scaled_.data(),reverse_.data(),reverse_.size());
#ifdef HARMONY_ENGINE_TEST
    if(engine_test::reverse) engine_test::reverse(reverse_);
#endif
    webrtc::StreamConfig config(rate,channels);
    apm_->ProcessReverseStream(reverse_.data(),config,config,reverse_.data());
  }
  ExternalReference reference_;
  std::shared_ptr<Recording> recording_;
  webrtc::scoped_refptr<loopback::SystemAudioSource> external_;
  webrtc::PushResampler<int16_t> reference_resampler_;
  std::vector<int16_t> external_scaled_, reverse_, silence_;
  webrtc::AudioProcessing* apm_;
  MicSource* mic_;
  std::mutex mu_;
  webrtc::AudioTransport* engine_ = nullptr;
  std::vector<int16_t> out_, probe_;
  std::shared_ptr<FilterSlot> filter_;
  // 20 consecutive 10 ms probes: measured recovery and burst tests in w3-dfn.md.
  static constexpr unsigned kDfnRecoveryBlocks=20;
  unsigned good_blocks_=0;
  // DFN over 10 ms once is tolerated: the block goes out filtered, late,
  // and DFN stays. On an i3-3110M DFN never stayed on through a whole call
  // when the first late block brought NS back (03/11/2026). A second late
  // block within 20 blocks (200 ms) is a slow machine, not a hiccup, and
  // falls back as before. NaN, exceptions and bad shapes never have output
  // worth keeping and fall back at once.
  unsigned since_late_=kDfnRecoveryBlocks;
  bool LateIsIsolated() const { return since_late_>=kDfnRecoveryBlocks; }
  void CountLate(bool tried) {
    if (!tried) return;
    if (filter_->reason.load()==FilterSlot::Deadline) since_late_=0;
    else if (since_late_<kDfnRecoveryBlocks) ++since_late_;
  }
  bool dfn_ready_=false;
  std::shared_ptr<std::atomic<bool>> audience_;
  bool config_applied_=false;
  bool ns_enabled_=true, echo_enabled_=true, agc_enabled_=true;
  FilterSlot::Reason reason_=FilterSlot::Warming;
  webrtc::PushResampler<int16_t> capture_resampler_;
  std::vector<int16_t> mono_, normalized_;
  std::string effective_;
  std::shared_ptr<std::atomic<double>> render_level_;
  std::shared_ptr<std::atomic<uint64_t>> render_at_,render_calls_;
  bool test_tone_ = false;
 public:
  std::shared_ptr<std::atomic<uint64_t>> capture_calls_,reference_missing_,reference_dropped_,reference_age_;
  std::shared_ptr<std::atomic<double>> render_peak_;
  std::shared_ptr<std::atomic<bool>> monitor_, sidetone_;
  std::shared_ptr<std::atomic<uint64_t>> sidetone_played_;
 private:
  VoiceQueue side_;
  std::vector<int16_t> side_scaled_;
  bool side_on_=false;
};

// Windows lowers every other sound (80% by default) while an output stream
// marked AudioCategory_Communications plays, and libwebrtc's Core Audio module
// marks every stream that way (core_audio_utility_win.cc:907 in m155). The
// documented way out, IAudioSessionControl2::SetDuckingPreference, changed
// nothing on Windows 11 (W12 level probe, 25/10/2026: a 440 Hz tone fell from
// -23 to -41 dBFS with or without it), and a microphone in that category
// lowers nothing. So the output streams are marked Other as the module opens
// them, the way a browser's are: its call to IAudioClient2::SetClientProperties
// goes through a function table that every audio client in the process
// shares, and the programme puts itself in that slot once. The microphone
// keeps its category, since some drivers process a communications microphone
// differently; a microphone reopened by the module itself after a device
// change counts as output and loses it, which lowers nothing either.
// Friends watch a game or a film with the call on.
thread_local int opening_microphone = 0;
bool OpeningMicrophone() { return opening_microphone > 0; }
AUDIO_STREAM_CATEGORY StreamCategory(AUDIO_STREAM_CATEGORY wanted, bool microphone) {
  return wanted == AudioCategory_Communications && !microphone ? AudioCategory_Other : wanted;
}
using SetPropertiesFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient2*, const AudioClientProperties*);
std::atomic<SetPropertiesFn> set_properties{nullptr};
HRESULT STDMETHODCALLTYPE SetPropertiesKeepingOthers(IAudioClient2* client, const AudioClientProperties* p) {
  const SetPropertiesFn original = set_properties.load();
  if (!p || p->cbSize < offsetof(AudioClientProperties, eCategory) + sizeof(p->eCategory)) return original(client, p);
  const AUDIO_STREAM_CATEGORY category = StreamCategory(p->eCategory, OpeningMicrophone());
  if (category == p->eCategory) return original(client, p);
  // A copy as long as the caller's: newer Windows SDKs add fields at the end.
  std::vector<BYTE> copy(reinterpret_cast<const BYTE*>(p), reinterpret_cast<const BYTE*>(p) + p->cbSize);
  reinterpret_cast<AudioClientProperties*>(copy.data())->eCategory = category;
  const HRESULT result = original(client, reinterpret_cast<const AudioClientProperties*>(copy.data()));
  logf("audio output stream marked Other so Windows keeps other sounds up: 0x%08lx", static_cast<unsigned long>(result));
  return result;
}
// Needs COM; called on the audio thread before the module is created.
void KeepOthersAudible() {
  static std::once_flag once;
  std::call_once(once, [] {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> devices; ComPtr<IMMDevice> output; ComPtr<IAudioClient2> client; ComPtr<IAudioClient3> client3;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
    if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &output);
    if (SUCCEEDED(hr)) hr = output->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    int replaced = 0;
    if (SUCCEEDED(hr)) {
      // IUnknown (3), IAudioClient (12), IsOffloadCapable, then SetClientProperties.
      constexpr int kSetClientProperties = 16;
      std::vector<void**> tables{*reinterpret_cast<void***>(client.Get())};
      if (SUCCEEDED(client.As(&client3)) && *reinterpret_cast<void***>(client3.Get()) != tables[0]) tables.push_back(*reinterpret_cast<void***>(client3.Get()));
      for (void** table : tables) {
        void*& slot = table[kSetClientProperties];
        if (slot == reinterpret_cast<void*>(&SetPropertiesKeepingOthers)) continue;
        if (set_properties.load() && slot != reinterpret_cast<void*>(set_properties.load())) continue; // another implementation: leave it
        DWORD was = 0;
        if (!VirtualProtect(&slot, sizeof(slot), PAGE_READWRITE, &was)) { hr = HRESULT_FROM_WIN32(GetLastError()); continue; }
        set_properties = reinterpret_cast<SetPropertiesFn>(slot);
        slot = reinterpret_cast<void*>(&SetPropertiesKeepingOthers);
        VirtualProtect(&slot, sizeof(slot), was, &was);
        ++replaced;
      }
    }
    logf("audio: output streams will not lower other sounds (%d table(s), 0x%08lx)", replaced, static_cast<unsigned long>(hr));
  });
}

// Everything forwards to the Windows device, except RegisterAudioCallback.
class Device : public webrtc::AudioDeviceModule {
 public:
  // `skip` hides the module's leading pseudo-devices: the Core Audio module
  // lists "Default - X" and "Communication - X" first, both repeating a real
  // device, and the rest of the programme expects only real devices.
  Device(webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner, std::unique_ptr<Transport> transport,bool requireCable=false,uint16_t skip=0)
      : inner_(std::move(inner)), transport_(std::move(transport)),required_cable_(requireCable),skip_(skip) {
    // The Core Audio module has no audio buffer until Init(): registering
    // the callback before it crashed the programme (22/10/2026).
    if (inner_->Initialized()) Register();
  }
  ~Device() override { if (registered_) inner_->RegisterAudioCallback(nullptr); }

  int32_t RegisterAudioCallback(webrtc::AudioTransport* cb) override {
    transport_->SetEngine(cb);
    return 0;
  }
  int32_t ActiveAudioLayer(AudioLayer* l) const override { return inner_->ActiveAudioLayer(l); }
  int32_t Init() override { auto result = inner_->Init(); if (!result) Register(); return result; }
  int32_t Terminate() override { return inner_->Terminate(); }
  bool Initialized() const override { return inner_->Initialized(); }
  int16_t PlayoutDevices() override { return int16_t(std::max(0,inner_->PlayoutDevices()-skip_)); }
  int16_t RecordingDevices() override { return int16_t(std::max(0,inner_->RecordingDevices()-skip_)); }
  int32_t PlayoutDeviceName(uint16_t i, char n[webrtc::kAdmMaxDeviceNameSize],
                            char g[webrtc::kAdmMaxGuidSize]) override {
    return inner_->PlayoutDeviceName(uint16_t(i+skip_), n, g);
  }
  int32_t RecordingDeviceName(uint16_t i, char n[webrtc::kAdmMaxDeviceNameSize],
                              char g[webrtc::kAdmMaxGuidSize]) override {
    return inner_->RecordingDeviceName(uint16_t(i+skip_), n, g);
  }
  int32_t SetPlayoutDevice(uint16_t i) override {
    uint16_t j=uint16_t(i+skip_);
    if(required_cable_&&!IsCable(j))return -1;
    cable_ready_=false;auto result=inner_->SetPlayoutDevice(j);if(!result)cable_index_=j;return result;
  }
  // libwebrtc opens the communications device on Windows (media/engine/adm_helpers.cc),
  // and Windows lowers every other sound by 80% while a communications stream is
  // open (mmsys.cpl, Communications tab). Friends watch a game with the call on:
  // open the ordinary default device instead, as an explicit choice already does.
  static WindowsDeviceType Ordinary(WindowsDeviceType d) { return d==kDefaultCommunicationDevice?kDefaultDevice:d; }
  int32_t SetPlayoutDevice(WindowsDeviceType d) override {return required_cable_?SelectCable():inner_->SetPlayoutDevice(Ordinary(d));}
  int32_t SetRecordingDevice(uint16_t i) override { return inner_->SetRecordingDevice(uint16_t(i+skip_)); }
  int32_t SetRecordingDevice(WindowsDeviceType d) override { return inner_->SetRecordingDevice(Ordinary(d)); }
  int32_t PlayoutIsAvailable(bool* a) override { return inner_->PlayoutIsAvailable(a); }
  int32_t InitPlayout() override {if(required_cable_&&SelectCable())return -1;auto result=inner_->InitPlayout();cable_ready_=result==0;return result;}
  bool PlayoutIsInitialized() const override { return inner_->PlayoutIsInitialized(); }
  int32_t RecordingIsAvailable(bool* a) override { return inner_->RecordingIsAvailable(a); }
  int32_t InitRecording() override { ++opening_microphone; auto result = inner_->InitRecording(); --opening_microphone; return result; }
  bool RecordingIsInitialized() const override { return inner_->RecordingIsInitialized(); }
  int32_t StartPlayout() override {if(required_cable_&&(!cable_ready_||cable_index_<0||!IsCable(uint16_t(cable_index_))))return -1;return inner_->StartPlayout();}
  int32_t StopPlayout() override {cable_ready_=false;return inner_->StopPlayout();}
  bool Playing() const override { return inner_->Playing(); }
  int32_t StartRecording() override { return inner_->StartRecording(); }
  int32_t StopRecording() override { return inner_->StopRecording(); }
  bool Recording() const override { return inner_->Recording(); }
  int32_t InitSpeaker() override { return inner_->InitSpeaker(); }
  bool SpeakerIsInitialized() const override { return inner_->SpeakerIsInitialized(); }
  int32_t InitMicrophone() override { return inner_->InitMicrophone(); }
  bool MicrophoneIsInitialized() const override { return inner_->MicrophoneIsInitialized(); }
  int32_t SpeakerVolumeIsAvailable(bool* a) override { return inner_->SpeakerVolumeIsAvailable(a); }
  int32_t SetSpeakerVolume(uint32_t v) override { return inner_->SetSpeakerVolume(v); }
  int32_t SpeakerVolume(uint32_t* v) const override { return inner_->SpeakerVolume(v); }
  int32_t MaxSpeakerVolume(uint32_t* v) const override { return inner_->MaxSpeakerVolume(v); }
  int32_t MinSpeakerVolume(uint32_t* v) const override { return inner_->MinSpeakerVolume(v); }
  int32_t MicrophoneVolumeIsAvailable(bool* a) override { return inner_->MicrophoneVolumeIsAvailable(a); }
  int32_t SetMicrophoneVolume(uint32_t v) override { return inner_->SetMicrophoneVolume(v); }
  int32_t MicrophoneVolume(uint32_t* v) const override { return inner_->MicrophoneVolume(v); }
  int32_t MaxMicrophoneVolume(uint32_t* v) const override { return inner_->MaxMicrophoneVolume(v); }
  int32_t MinMicrophoneVolume(uint32_t* v) const override { return inner_->MinMicrophoneVolume(v); }
  int32_t SpeakerMuteIsAvailable(bool* a) override { return inner_->SpeakerMuteIsAvailable(a); }
  int32_t SetSpeakerMute(bool e) override { return inner_->SetSpeakerMute(e); }
  int32_t SpeakerMute(bool* e) const override { return inner_->SpeakerMute(e); }
  int32_t MicrophoneMuteIsAvailable(bool* a) override { return inner_->MicrophoneMuteIsAvailable(a); }
  int32_t SetMicrophoneMute(bool e) override { return inner_->SetMicrophoneMute(e); }
  int32_t MicrophoneMute(bool* e) const override { return inner_->MicrophoneMute(e); }
  int32_t StereoPlayoutIsAvailable(bool* a) const override { return inner_->StereoPlayoutIsAvailable(a); }
  int32_t SetStereoPlayout(bool e) override { return inner_->SetStereoPlayout(e); }
  int32_t StereoPlayout(bool* e) const override { return inner_->StereoPlayout(e); }
  int32_t StereoRecordingIsAvailable(bool* a) const override { return inner_->StereoRecordingIsAvailable(a); }
  int32_t SetStereoRecording(bool e) override { return inner_->SetStereoRecording(e); }
  int32_t StereoRecording(bool* e) const override { return inner_->StereoRecording(e); }
  int32_t PlayoutDelay(uint16_t* d) const override { return inner_->PlayoutDelay(d); }
  // Windows' built-in echo canceller is the Voice Capture DMO, which delivers
  // the microphone at 16 kHz mono: the friend heard a muffled, quieter voice
  // than the web client's 48 kHz one (W8b, 22/10/2026). The capture callback
  // already runs its own APM and DFN, so report none of the built-in effects.
  bool BuiltInAECIsAvailable() const override { return false; }
  bool BuiltInAGCIsAvailable() const override { return false; }
  bool BuiltInNSIsAvailable() const override { return false; }
  int32_t EnableBuiltInAEC(bool) override { return -1; }
  int32_t EnableBuiltInAGC(bool) override { return -1; }
  int32_t EnableBuiltInNS(bool) override { return -1; }
  int32_t GetPlayoutUnderrunCount() const override { return inner_->GetPlayoutUnderrunCount(); }
  std::optional<Stats> GetStats() const override { return inner_->GetStats(); }

 private:
  bool IsCable(uint16_t index) {
    char name[webrtc::kAdmMaxDeviceNameSize]{},guid[webrtc::kAdmMaxGuidSize]{};
    return !inner_->PlayoutDeviceName(index,name,guid)&&std::string(name).find("CABLE Input")!=std::string::npos;
  }
  int32_t SelectCable() {
    for(int16_t i=0;i<PlayoutDevices();++i)if(IsCable(uint16_t(i+skip_)))return SetPlayoutDevice(uint16_t(i));
    cable_ready_=false;logf("CABLE Input unavailable; test playout refused");return -1;
  }
  bool required_cable_=false,cable_ready_=false,registered_=false;int cable_index_=-1;uint16_t skip_=0;
  void Register() { if (!registered_) { inner_->RegisterAudioCallback(transport_.get()); registered_=true; } }
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner_;
  std::unique_ptr<Transport> transport_;
};

}  // namespace

Pipeline CreatePipeline(bool testTone, std::unique_ptr<NoiseFilter> backend, bool simulatedDevice,bool requireCable) {
  Pipeline p;
  auto env = webrtc::CreateEnvironment();
  webrtc::AudioProcessing::Config config;
  config.echo_canceller.enabled = true;
  config.noise_suppression.enabled = true;
  config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  config.high_pass_filter.enabled = true;
  config.gain_controller2.enabled = true;
  // The gain itself, not only AGC2's limiter: without it a quiet voice reached
  // DFN quiet, and DFN took 39-68% of speech windows down more than 6 dB (W13,
  // Codex, SAPI speech at -35/-25 dBFS) — the owner's voice "mudando de
  // volume". On, no window lost 6 dB at -45/-35. The web gets the same from
  // Chrome's autoGainControl before its DFN. The AGC switch still turns it off.
  config.gain_controller2.adaptive_digital.enabled = true;
  p.processing = webrtc::BuiltinAudioProcessingBuilder(config).Build(env);
  p.engineProcessing = webrtc::BuiltinAudioProcessingBuilder().Build(env);
  p.mixer=webrtc::make_ref_counted<ReceiveMixer>();
  // The Core Audio module ("ADM2"), not the legacy platform one: the legacy
  // module opens a microphone only in a few PCM formats and refused the
  // owner's Fifine, which Windows runs at 192 kHz (22/10/2026); its only way
  // around that was the 16 kHz Voice Capture DMO. This one opens every device
  // in its own mix format, and the capture callback resamples to 48 kHz.
  // It needs COM in the multithreaded apartment on the thread that creates
  // and then drives it (libwebrtc's worker); without it the factory hangs.
  auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) logf("audio: COM on the audio thread failed HRESULT=0x%08lx", com);
  KeepOthersAudible();
  auto inner = webrtc::CreateWindowsCoreAudioAudioDeviceModule(env, true);
  uint16_t skip = 2;
#ifndef HARMONY_PUBLISH
  if(simulatedDevice) {
    auto simulated=webrtc::make_ref_counted<FakeADM>(); simulated->EnablePump(); inner=simulated; skip=0;
    p.removeTestDevice=[simulated] { simulated->present=false; };
  }
#else
  testTone=false; simulatedDevice=false; requireCable=false;
#endif
  if (!inner || !p.processing || !p.engineProcessing) {
    logf("audio: device or processing unavailable");
    return {};
  }
#ifdef HARMONY_RESOURCE_MEASUREMENT
  resource_meter::Mark("pipeline-before-dfn");
#endif
  p.mic = webrtc::make_ref_counted<MicSource>();
  if (!backend) { try { backend=std::make_unique<DfnFilter>(); } catch (...) { logf("audio DFN initialization failed; libwebrtc NS fallback"); } }
#ifdef HARMONY_RESOURCE_MEASUREMENT
  resource_meter::Mark("pipeline-after-dfn");
#endif
  bool configured=bool(backend);
  p.filter = std::make_shared<FilterSlot>(std::move(backend));
  // Direct calls also use this pipeline and always have a peer. Room sessions
  // opt into audience tracking before starting their device.
  p.audience = std::make_shared<std::atomic<bool>>(true);
  p.renderLevel = std::make_shared<std::atomic<double>>(0); p.renderPeak = std::make_shared<std::atomic<double>>(0);
  p.renderAt=std::make_shared<std::atomic<uint64_t>>(0); p.renderCalls=std::make_shared<std::atomic<uint64_t>>(0);
  logf("audio DFN %s; libwebrtc NS fallback", configured ? "backend configured" : "unavailable");
  p.captureCalls=std::make_shared<std::atomic<uint64_t>>(0);
  p.referenceMissing=std::make_shared<std::atomic<uint64_t>>(0); p.referenceDropped=std::make_shared<std::atomic<uint64_t>>(0); p.referenceAge=std::make_shared<std::atomic<uint64_t>>(0);
  auto transport=std::make_unique<Transport>(p.processing.get(), p.mic.get(), p.filter, p.renderLevel, p.renderAt, p.renderCalls, testTone && !simulatedDevice, !simulatedDevice, p.audience);
  transport->capture_calls_=p.captureCalls; transport->reference_missing_=p.referenceMissing; transport->reference_dropped_=p.referenceDropped; transport->reference_age_=p.referenceAge; transport->render_peak_=p.renderPeak;
  p.monitor=std::make_shared<std::atomic<bool>>(false); transport->monitor_=p.monitor;
  p.sidetone=std::make_shared<std::atomic<bool>>(false); transport->sidetone_=p.sidetone;
  p.sidetonePlayed=std::make_shared<std::atomic<uint64_t>>(0); transport->sidetone_played_=p.sidetonePlayed;
  p.device = webrtc::make_ref_counted<Device>(inner, std::move(transport),requireCable,skip);
  return p;
}

// The voice test's player: the microphone track's 10 ms blocks in, the
// output device's pulls out (VoiceQueue).
class MonitorPlayback final : public webrtc::AudioTrackSinkInterface, public webrtc::AudioTransport {
 public:
  VoiceQueue queue;
  void OnData(const void* data, int bits, int rate, size_t channels, size_t frames) override {
    if (bits != 16 || rate != 48000 || channels != 1 || frames != 480) return;
    queue.Push(static_cast<const int16_t*>(data));
  }
  int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t, uint32_t, uint32_t, int32_t, uint32_t, bool, uint32_t&) override { return 0; }
  int32_t NeedMorePlayData(size_t frames, size_t, size_t channels, uint32_t rate, void* samples, size_t& out,
                           int64_t* elapsed, int64_t* ntp) override {
    if (elapsed) *elapsed = -1;
    if (ntp) *ntp = -1;
    if (!queue.Pop(frames, rate, scaled_)) scaled_.assign(frames, 0);
    auto pcm = static_cast<int16_t*>(samples);
    for (size_t f = 0; f < frames; ++f) for (size_t c = 0; c < channels; ++c) pcm[f * channels + c] = scaled_[f];
    out = frames * channels;
    return 0;
  }
  void PullRenderData(int bits, int, size_t channels, size_t frames, void* data, int64_t*, int64_t*) override {
    memset(data, 0, frames * channels * size_t(bits / 8));
  }

 private:
  std::vector<int16_t> scaled_;
};

struct Monitor::State {
  MonitorSettings settings;
  std::mutex mu;
  std::condition_variable changed;
  bool ready = false, running = false, stop = false;
  std::shared_ptr<FilterSlot> filter;
  MonitorPlayback playback;
  std::thread thread;
  void Apply() {
    if (!filter) return;
    filter->enabled = settings.dfn; filter->strength = settings.strength;
    filter->echo = settings.echo; filter->noise = settings.noise; filter->agc = settings.agc;
    filter->micGain = settings.micGain;
  }
  // The pipeline lives and dies on this thread: the Core Audio module wants
  // COM in the multithreaded apartment where it was made (CreatePipeline).
  void Run() {
    perf::NameThread(L"harmony-voice-test");
    auto p = CreatePipeline(false, std::move(settings.backend), settings.simulatedDevice, settings.requireCable);
    auto device = p.device;
    bool ok = false;
    if (device) {
      p.monitor->store(true);
      { std::lock_guard lock(mu); filter = p.filter; Apply(); }
      device->RegisterAudioCallback(&playback);
      ok = device->Init() == 0;
      auto choose = [&](bool capture, const std::string& id) {
        int32_t result = -1;
        auto count = capture ? device->RecordingDevices() : device->PlayoutDevices();
        for (uint16_t i = 0; !id.empty() && i < count; ++i) {
          char name[webrtc::kAdmMaxDeviceNameSize]{}, guid[webrtc::kAdmMaxGuidSize]{};
          auto status = capture ? device->RecordingDeviceName(i, name, guid) : device->PlayoutDeviceName(i, name, guid);
          if (!status && id == guid) { result = capture ? device->SetRecordingDevice(i) : device->SetPlayoutDevice(i); break; }
        }
        if (result) result = capture ? device->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice)
                                     : device->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
        return result == 0;
      };
      if (ok) {
        p.mic->AddSink(&playback);
        ok = choose(true, settings.input) && choose(false, settings.output) &&
             device->InitRecording() == 0 && device->StartRecording() == 0 &&
             device->InitPlayout() == 0 && device->StartPlayout() == 0;
      }
    }
    logf("voice test %s", ok ? "playing the microphone back" : "could not open the devices");
    {
      std::unique_lock lock(mu);
      ready = true; running = ok; changed.notify_all();
      changed.wait(lock, [this] { return stop; });
      running = false;
    }
    if (device) {
      device->StopRecording(); device->StopPlayout();
      p.mic->RemoveSink(&playback);
      device->RegisterAudioCallback(nullptr);
      device->Terminate();
    }
    std::lock_guard lock(mu); filter.reset();
    logf("voice test stopped");
  }
};

Monitor::Monitor(MonitorSettings settings) : state_(std::make_shared<State>()) {
  state_->settings = std::move(settings);
  state_->thread = std::thread([state = state_] { state->Run(); });
  std::unique_lock lock(state_->mu);
  state_->changed.wait(lock, [this] { return state_->ready; });
}
Monitor::~Monitor() {
  { std::lock_guard lock(state_->mu); state_->stop = true; }
  state_->changed.notify_all();
  state_->thread.join();
}
bool Monitor::Running() const { std::lock_guard lock(state_->mu); return state_->running; }
void Monitor::Filters(bool dfn, int strength, bool echo, bool noise, bool agc, double gain) {
  std::lock_guard lock(state_->mu);
  auto& s = state_->settings;
  s.dfn = dfn; s.strength = std::clamp(strength, 10, 100); s.echo = echo; s.noise = noise; s.agc = agc; s.micGain = std::clamp(gain, 0., 2.);
  state_->Apply();
}
uint64_t Monitor::Played() const { return state_->playback.queue.played.load(); }
uint64_t Monitor::Gaps() const { return state_->playback.queue.trimmed.load() + state_->playback.queue.starved.load(); }
double Monitor::TakePeak() { return state_->playback.queue.peak.exchange(0); }

void ArmRecording(std::filesystem::path root, int seconds) {
  auto& r = TheRecorder();
  std::lock_guard lock(r.mu);
  r.root = std::move(root); r.seconds = std::clamp(seconds, 1, 300); r.armed = true; r.armedFast = true;
  if (!r.current) r.status = {RecordingStatus::Armed, 0, {}};
  logf("audio recording armed seconds=%d", r.seconds);
}

void DisarmRecording() {
  auto& r = TheRecorder();
  std::lock_guard lock(r.mu);
  r.armed = false; r.armedFast = false;
  // A recording under way ends early and is kept: what it has is still useful.
  if (r.current) r.current->stop = true;
  else if (r.status.state == RecordingStatus::Armed) r.status = {};
}

RecordingStatus ReadRecording() {
  auto& r = TheRecorder();
  std::lock_guard lock(r.mu);
  auto status = r.status;
  if (r.current) status.seconds = int(r.current->quanta / 100);
  return status;
}

void RecordStats(const webrtc::RTCStatsReport& report) {
  auto& r = TheRecorder();
  if (!r.currentFast.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(r.mu);
  auto rec = r.current;
  auto now = GetTickCount64();
  if (!rec || !rec->ready || rec->finishing || (rec->statsAt && now - rec->statsAt < 950)) return;
  double seconds = rec->statsAt ? double(now - rec->statsAt) / 1000 : 0;
  rec->statsAt = now;
  std::optional<double> rtt, target, sendKbps, remoteLoss, remoteJitter, jitter, concealed, waiting;
  std::string route = "-";
  std::string selected;
  for (auto* t : report.GetStatsOfType<webrtc::RTCTransportStats>()) if (t->selected_candidate_pair_id) selected = *t->selected_candidate_pair_id;
  for (auto* pair : report.GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
    if (selected.empty() ? !pair->nominated.value_or(false) : pair->id() != selected) continue;
    if (pair->current_round_trip_time) rtt = *pair->current_round_trip_time * 1000;
    auto local = pair->local_candidate_id ? report.Get(*pair->local_candidate_id) : nullptr;
    auto remote = pair->remote_candidate_id ? report.Get(*pair->remote_candidate_id) : nullptr;
    if (local && remote)
      route = local->cast_to<webrtc::RTCLocalIceCandidateStats>().candidate_type.value_or("") == "relay" ||
              remote->cast_to<webrtc::RTCRemoteIceCandidateStats>().candidate_type.value_or("") == "relay" ? "TURN" : "direta";
    break;
  }
  // The microphone only: the screen's sound is a second audio stream.
  uint64_t sentBytes = 0; bool sentKnown = false;
  std::map<std::string, uint64_t> outNext;
  for (auto* s : report.GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
    if (s->kind.value_or("") != "audio" || !s->media_source_id) continue;
    auto source = report.Get(*s->media_source_id);
    if (!source || strcmp(source->type(), webrtc::RTCAudioSourceStats::kType) != 0 ||
        source->cast_to<webrtc::RTCAudioSourceStats>().track_identifier.value_or("") != "mic") continue;
    uint64_t bytes = s->bytes_sent.value_or(0); outNext[s->id()] = bytes;
    auto before = rec->outPrevious.find(s->id());
    if (before != rec->outPrevious.end() && bytes >= before->second) { sentBytes += bytes - before->second; sentKnown = true; }
    if (s->target_bitrate) target = *s->target_bitrate / 1000;
    for (auto* remote : report.GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>()) {
      if (remote->local_id.value_or("") != s->id()) continue;
      if (remote->fraction_lost) remoteLoss = *remote->fraction_lost * 100;
      if (remote->jitter) remoteJitter = *remote->jitter * 1000;
    }
  }
  if (sentKnown && seconds > 0) sendKbps = sentBytes * 8 / seconds / 1000;
  rec->outPrevious = std::move(outNext);
  // Every voice you hear, each against its own previous report.
  int64_t lost = 0; uint64_t samples = 0, hidden = 0, emitted = 0; double delay = 0; bool lostKnown = false;
  std::map<std::string, Recording::Inbound> inNext;
  for (auto* s : report.GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
    if (s->kind.value_or("") != "audio") continue;
    Recording::Inbound now{s->packets_lost.value_or(0), s->total_samples_received.value_or(0), s->concealed_samples.value_or(0),
                           s->jitter_buffer_emitted_count.value_or(0), s->jitter_buffer_delay.value_or(0)};
    inNext[s->id()] = now;
    if (s->jitter) jitter = std::max(jitter.value_or(0), *s->jitter * 1000);
    auto before = rec->inPrevious.find(s->id());
    if (before == rec->inPrevious.end()) continue;
    auto& was = before->second;
    if (now.lost >= was.lost) { lost += now.lost - was.lost; lostKnown = true; }
    if (now.samples >= was.samples && now.concealed >= was.concealed) { samples += now.samples - was.samples; hidden += now.concealed - was.concealed; }
    if (now.emitted >= was.emitted && now.delay >= was.delay) { emitted += now.emitted - was.emitted; delay += now.delay - was.delay; }
  }
  rec->inPrevious = std::move(inNext);
  std::string lostText = lostKnown ? std::to_string(lost) : "-";
  if (samples) concealed = 100.0 * double(hidden) / double(samples);
  if (emitted) waiting = delay / double(emitted) * 1000;
  char line[320];
  snprintf(line, sizeof line, "%3.0f rede | %s %s | %s %s | %s %s | %s %s %s %s\n", rec->Seconds(),
           Fixed(rtt, "%.0f").c_str(), route.c_str(), Fixed(sendKbps).c_str(), Fixed(target).c_str(),
           Fixed(remoteLoss).c_str(), Fixed(remoteJitter, "%.0f").c_str(), lostText.c_str(),
           Fixed(jitter, "%.0f").c_str(), Fixed(concealed).c_str(), Fixed(waiting, "%.0f").c_str());
  rec->Line(line);
}

}  // namespace audio
