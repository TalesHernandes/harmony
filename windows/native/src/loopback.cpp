#include "loopback.h"
#include "sync-detectors.h"
#include "sync-probe.h"
#include <tlhelp32.h>
#include <map>

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <algorithm>

#include "api/make_ref_counted.h"
#include "log.h"
#include "perf.h"
#include "media-policy.h"
#include "rtc_base/time_utils.h"

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace loopback {
namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrames10ms = kRate / 100;

// ActivateAudioInterfaceAsync answers on another thread; this waits for it.
class Activation : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
 public:
  Activation() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~Activation() override { CloseHandle(done_); }
  STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
    HRESULT hr = E_FAIL;
    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(op->GetActivateResult(&hr, &unknown)) && SUCCEEDED(hr)) unknown.As(&client);
    result = hr;
    SetEvent(done_);
    return S_OK;
  }
  bool Wait() {
    auto status=WaitForSingleObject(done_,5000);
    wait_result=status==WAIT_OBJECT_0?result:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    logf("screen open step=loopback_activation wait=%lu HRESULT=0x%08lx deadline_ms=5000",status,wait_result);
    return status==WAIT_OBJECT_0 && SUCCEEDED(wait_result) && client;
  }
  ComPtr<IAudioClient> client;
  HRESULT result = E_FAIL,wait_result=E_FAIL;

 private:
  HANDLE done_;
};

}  // namespace

webrtc::scoped_refptr<SystemAudioSource> SystemAudioSource::Create(uint32_t target_process, bool screen_probe) {
  auto source = webrtc::make_ref_counted<SystemAudioSource>();
  source->screen_probe_ = screen_probe;
  if (!source->Start(target_process)) return nullptr;
  return source;
}

namespace {
struct ComScope {
  HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  ~ComScope() {if(SUCCEEDED(hr))CoUninitialize();}
};
bool IncludesHarmony(uint32_t target) {
  if(!target)return false;
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return true;
  PROCESSENTRY32W entry{sizeof(entry)};std::map<DWORD,DWORD> parents;
  if(Process32FirstW(snapshot,&entry))do {parents[entry.th32ProcessID]=entry.th32ParentProcessID;}while(Process32NextW(snapshot,&entry));
  CloseHandle(snapshot);DWORD pid=GetCurrentProcessId();
  for(size_t i=0;i<=parents.size()&&pid;++i) {if(pid==target)return true;auto it=parents.find(pid);if(it==parents.end())break;pid=it->second;}
  return false;
}
}
bool SystemAudioSource::Start(uint32_t target_process) {
  ComScope apartment;
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  if(IncludesHarmony(target_process)) {logf("system audio: selected tree includes harmony; choose a monitor"); return false;}
  auto target=media::SoundTarget(target_process,GetCurrentProcessId());
  params.ProcessLoopbackParams.TargetProcessId = target.pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = target.include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT pv{};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof(params);
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
  auto handler = Microsoft::WRL::Make<Activation>();
  ComPtr<IActivateAudioInterfaceAsyncOperation> op;
  logf("screen open step=loopback_requested deadline_ms=5000");
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                           handler.Get(), &op);
  if (FAILED(hr) || !handler->Wait()) {
    logf("system audio: process loopback refused (0x%08lx) — Windows 11 needed", FAILED(hr) ? hr : handler->wait_result);
    return false;
  }
  logf("screen open step=loopback_activated");
  // Process loopback has no mix format to ask for: it converts to what we
  // name, which is what the encoder wants anyway.
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = kChannels;
  fmt.nSamplesPerSec = kRate;
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
  fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
  IAudioClient* client = handler->client.Detach();
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                          200000 /* 20 ms; the 30-minute run used this. 0/0 (round 27) is unmeasured */, 0, &fmt, nullptr);
  if (FAILED(hr)) {
    logf("system audio: initialize failed 0x%08lx", hr);
    client->Release();
    return false;
  }
  // These APIs are not implemented consistently by process loopback. Preserve
  // HRESULT and raw values; capacity is not a measurement of queued audio.
  if (sync_probe::Enabled()) {
    static std::atomic<int64_t> next_stream{0};
    probe_stream_ = ++next_stream;
    UINT32 capacity = 0;
    REFERENCE_TIME latency = 0, period = 0, minimum = 0;
    const HRESULT capacity_hr = client->GetBufferSize(&capacity);
    const HRESULT latency_hr = client->GetStreamLatency(&latency);
    const HRESULT period_hr = client->GetDevicePeriod(&period, &minimum);
    auto e = sync_probe::Event("loopback_config");
    e["stream_id"] = probe_stream_;
    e["source"] = screen_probe_ ? "screen_audio" : "aec_reference";
    e["mode"] = target.include ? "include_target_tree" : "exclude_harmony_tree";
    e["requested_buffer_100ns"] = 0;
    e["sample_rate"] = kRate;
    e["buffer_size_hr"] = int64_t(capacity_hr);
    e["buffer_frames"] = SUCCEEDED(capacity_hr) ? json::Value(int64_t(capacity)) : json::Value();
    e["latency_hr"] = int64_t(latency_hr);
    e["latency_100ns"] = SUCCEEDED(latency_hr) ? json::Value(int64_t(latency)) : json::Value();
    e["period_hr"] = int64_t(period_hr);
    e["period_100ns"] = SUCCEEDED(period_hr) ? json::Value(int64_t(period)) : json::Value();
    e["minimum_period_100ns"] = SUCCEEDED(period_hr) ? json::Value(int64_t(minimum)) : json::Value();
    sync_probe::Record(std::move(e));
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  client->SetEventHandle(event);
  IAudioCaptureClient* capture = nullptr;
  hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
  if (FAILED(hr) || FAILED(client->Start())) {
    logf("system audio: start failed 0x%08lx", hr);
    if (capture) capture->Release();
    client->Release();
    CloseHandle(event);
    return false;
  }
  client_ = client;
  capture_ = capture;
  event_ = event;
  thread_ = std::thread([this] { Run(); });
  logf("system audio: mode=%s (48 kHz stereo; outside AEC/DFN)",target.include?"include target tree":"exclude harmony tree");
  return true;
}

SystemAudioSource::~SystemAudioSource() {
  stop_ = true;
  if (event_) SetEvent(static_cast<HANDLE>(event_));
  if (thread_.joinable()) thread_.join();
  if (client_) static_cast<IAudioClient*>(client_)->Stop();
  if (capture_) static_cast<IAudioCaptureClient*>(capture_)->Release();
  if (client_) static_cast<IAudioClient*>(client_)->Release();
  if (event_) CloseHandle(static_cast<HANDLE>(event_));
}

void SystemAudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) sinks_.push_back(sink);
}

void SystemAudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void SystemAudioSource::Run() {
  perf::NameThread(L"harmony-system-audio");
  ComScope apartment;
  static perf::Stage& stage = perf::Get("audio.system");
  auto* capture = static_cast<IAudioCaptureClient*>(capture_);
  LARGE_INTEGER qpc, frequency;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&qpc);
  const int64_t clock_offset = webrtc::TimeNanos() -
      (qpc.QuadPart / frequency.QuadPart * 1000000000LL +
       qpc.QuadPart % frequency.QuadPart * 1000000000LL / frequency.QuadPart);
  CaptureBlocks blocks;
  const bool probe_enabled=screen_probe_ && sync_probe::Enabled();
  sync_probe::BeepDetector beep_detector;
  std::vector<int16_t> packet_samples;
  int64_t packet_sequence = 0, drain_sequence = 0;
  double previous_observed_ms = 0;
  int64_t missing_timestamps_ = 0;
  while (!stop_) {
    WaitForSingleObject(static_cast<HANDLE>(event_), 100);
    if (stop_) break;
    ++drain_sequence;
    int64_t packet_in_drain = 0;
    for (;;) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 captured_qpc = 0, device_position = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, &device_position, &captured_qpc)) || frames == 0) break;
      const double observed_ms = sync_probe::Enabled() ? sync_probe::QpcMs() : 0;
      ++packet_sequence;
      ++packet_in_drain;
      std::optional<int64_t> captured_ns;
      if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && captured_qpc) {
        captured_ns = int64_t(captured_qpc) * 100 + clock_offset;
        static perf::Stage& stageAudioSystemQueueAge = perf::Get("audio.system.queue_age");
        stageAudioSystemQueueAge.Record(std::max<int64_t>(0, webrtc::TimeNanos() - *captured_ns));
      } else {
        // Missing device time stays unknown; never claim delivery was capture.
        static perf::Stage& stageAudioSystemTimestampMissing = perf::Get("audio.system.timestamp_missing");
        stageAudioSystemTimestampMissing.SetValue(++missing_timestamps_);
      }
      packet_samples.resize(size_t(frames) * kChannels);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        std::fill(packet_samples.begin(), packet_samples.end(), int16_t(0));
      else
        memcpy(packet_samples.data(), data, packet_samples.size() * sizeof(int16_t));
      capture->ReleaseBuffer(frames);
      // Record after releasing WASAPI. Sample the first 32 packets, then 1/100,
      // plus every discontinuity/timestamp error. Never retain PCM in the log.
      if (sync_probe::Enabled()) {
        if (packet_sequence <= 32 || packet_sequence % 100 == 0 ||
            (flags & (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))) {
          auto e = sync_probe::Event("loopback_packet");
          e["stream_id"] = probe_stream_;
          e["source"] = screen_probe_ ? "screen_audio" : "aec_reference";
          e["sequence"] = packet_sequence;
          e["drain"] = drain_sequence;
          e["packet_in_drain"] = packet_in_drain;
          e["frames"] = int64_t(frames);
          e["sample_rate"] = kRate;
          e["flags"] = int64_t(flags);
          e["device_position_frames"] = double(device_position);
          e["captured_qpc_100ns"] = double(captured_qpc);
          e["observed_qpc_ms"] = observed_ms;
          e["timestamp_valid"] = captured_ns.has_value();
          e["qpc_age_ms"] = captured_ns ? json::Value(observed_ms - double(captured_qpc) / 10000.0) : json::Value();
          e["previous_delivery_gap_ms"] = packet_sequence > 1 ? json::Value(observed_ms - previous_observed_ms) : json::Value();
          sync_probe::Record(std::move(e));
        }
        previous_observed_ms = observed_ms;
      }
      if(probe_enabled && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
        beep_detector.Gap();sync_probe::Record(sync_probe::Event("audio_probe_gap"));
      }
      blocks.Push(packet_samples.data(),
                  frames, captured_ns, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
                  [&](const int16_t* samples, std::optional<int64_t> captured_ms) {
        if(probe_enabled) {
          const auto onset=beep_detector.Push(samples,kFrames10ms,kChannels,kRate);
          if(onset) {
            auto e=sync_probe::Event("beep");e["media_now_ms"]=webrtc::TimeMicros()/1000.0;
            e["captured_ms"]=captured_ms?json::Value(*captured_ms):json::Value();
            e["onset_frame"]=int64_t(*onset);e["onset_ms"]=double(*onset)*1000/kRate;
            e["source"]="screen_audio";e["sample_rate"]=kRate;sync_probe::Record(std::move(e));
          }
        }
        auto timer = stage.Begin();
        {
          std::lock_guard<std::mutex> lock(mu_);
          for (auto* sink : sinks_) sink->OnData(samples, 16, kRate, kChannels, kFrames10ms, captured_ms);
        }
        timer.Done();
      });
    }
  }
}

}  // namespace loopback
