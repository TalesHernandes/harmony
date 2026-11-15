#include "internal.h"
#include "nvenc.h"
#include "mf.h"
#include "cadence.h"
#include "../src/log.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

namespace gpu {
namespace {
std::mutex name_mutex;
std::string active_name = "Not initialized";
std::atomic<bool> force_fallback{false};
std::atomic<int> fail_after{0};
const char* StageName(int rank) {
  return rank == 0 ? "encode.alta" : rank == 1 ? "encode.media" : "encode.baixa";
}
// Windows emits Annex B H.264. Keep its parameter sets for repeated IDRs; fail
// explicitly if a transform ever changes to length-prefixed output.
bool NormalizeMF(EncodedPacket& packet, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
  auto& b = packet.bytes;
  struct Nal { size_t start, payload, end; };
  std::vector<Nal> nals;
  for (size_t i = 0; i + 3 < b.size();) {
    size_t prefix = 0;
    if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 1) prefix = 3;
    else if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 0 && b[i+3] == 1) prefix = 4;
    if (!prefix) { ++i; continue; }
    if (!nals.empty()) nals.back().end = i;
    nals.push_back({i, i + prefix, b.size()});
    i += prefix + 1;
  }
  if (nals.empty()) throw std::runtime_error("MF H264 output is not Annex B");
  bool have_sps = false, have_pps = false;
  for (auto n : nals) {
    if (n.payload >= b.size()) continue;
    int type = b[n.payload] & 31;
    if (type == 5) packet.key = true;
    if (type == 7) { sps.assign(b.begin()+n.start, b.begin()+n.end); have_sps = true; }
    if (type == 8) { pps.assign(b.begin()+n.start, b.begin()+n.end); have_pps = true; }
  }
  if (packet.key && (!have_sps || !have_pps)) {
    if (sps.empty() || pps.empty()) throw std::runtime_error("MF IDR has no SPS/PPS");
    std::vector<uint8_t> headers;
    if (!have_sps) headers.insert(headers.end(), sps.begin(), sps.end());
    if (!have_pps) headers.insert(headers.end(), pps.begin(), pps.end());
    headers.insert(headers.end(), b.begin(), b.end()); b.swap(headers);
  }
  return packet.key;
}
struct Layer {
  int width = 0, height = 0, fps = 0, rank = 0;
  perf::Stage *stage = nullptr, *cadenceSkip = nullptr, *inactiveSkip = nullptr, *keyframes = nullptr;
  uint32_t bitrate = 0;
  int64_t next_us = 0;
  bool key = true;
  uint64_t bytes = 0;
  int64_t rate_start_ns = 0;
  std::unique_ptr<NVEncoder> nv;
  std::unique_ptr<MFEncoder> mf;
  std::vector<uint8_t> sps, pps;
};
class Encoder : public webrtc::VideoEncoder {
  DeviceLease device_lease_;
 public:
  explicit Encoder(bool high) : high_(high) {}
  ~Encoder() override { Release(); }
  int InitEncode(const webrtc::VideoCodec* codec, const Settings&) override {
    std::lock_guard lock(mutex_);
    layers_.clear(); inputs_ = 0; fallback_ = false; fatal_ = false;
    if (!codec || codec->codecType != webrtc::kVideoCodecH264) return -1;
    int count = std::max(1, static_cast<int>(codec->numberOfSimulcastStreams));
    if (count > 3) return -1;
    for (int i = 0; i < count; ++i) {
      Layer layer;
      layer.width = count == 1 ? codec->width : codec->simulcastStream[i].width;
      layer.height = count == 1 ? codec->height : codec->simulcastStream[i].height;
      layer.fps = static_cast<int>(count == 1 ? codec->maxFramerate : codec->simulcastStream[i].maxFramerate);
      layer.bitrate = (count == 1 ? codec->startBitrate : codec->simulcastStream[i].targetBitrate) * 1000;
      if (count > 1 && !codec->simulcastStream[i].active) layer.bitrate = 0;
      if (layer.width <= 0 || layer.height <= 0 || layer.width % 2 || layer.height % 2 || layer.fps <= 0) return -1;
      layer.rate_start_ns = perf::Stage::Now();
      layers_.push_back(std::move(layer));
    }
    int largest = 0;
    capture_fps_ = static_cast<int>(codec->maxFramerate);
    for (const auto& l : layers_) largest = std::max(largest, l.width * l.height);
    for (size_t i = 0; i < layers_.size(); ++i) {
      auto& l = layers_[i];
      l.rank = ResolutionRank(l.width, l.height, largest);
      l.stage = &perf::Get(StageName(l.rank));
      l.cadenceSkip = &perf::Get(std::string(StageName(l.rank)) + ".cadence_skip");
      l.inactiveSkip = &perf::Get(std::string(StageName(l.rank)) + ".inactive_skip");
      l.keyframes = &perf::Get(std::string(StageName(l.rank)) + ".keyframes");
      logf("InitEncode index=%zu resolution=%dx%d fps=%d rank=%s RID=not_available_in_VideoCodec",
          i, l.width, l.height, l.fps, StageName(l.rank));
      if (count > 1) {
        const auto& s = codec->simulcastStream[i];
        logf("screen allocation limits index=%zu min_kbps=%u target_kbps=%u max_kbps=%u active=%d",
             i, s.minBitrate, s.targetBitrate, s.maxBitrate, s.active);
      }
    }
    try {
      if (ForceFallback()) throw std::runtime_error("selftest forced Media Foundation");
      for (auto& layer : layers_) layer.nv = std::make_unique<NVEncoder>(layer.width, layer.height,
          layer.fps, std::max<uint32_t>(layer.bitrate, 1000), high_);
      SetActiveEncoderName("NVENC D3D11 x" + std::to_string(count));
      return 0;
    } catch (const std::exception& error) {
      logf("screen encoder NVENC initialization failed: %s", error.what());
      return Fallback() ? 0 : -1;
    }
  }
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
    std::lock_guard lock(mutex_); callback_ = callback; return 0;
  }
  int32_t Release() override {
    std::lock_guard lock(mutex_); layers_.clear(); ReleaseVideoProcessorCache(); return 0;
  }
  void SetRates(const RateControlParameters& rates) override {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < layers_.size(); ++i) {
      auto& l = layers_[i];
      uint32_t rate = rates.bitrate.GetSpatialLayerSum(i);
      if (!l.bitrate && rate) {
        l.key = true; l.next_us = 0;
        logf("screen encoder resumed index=%zu rank=%s bitrate=%u; next frame IDR",i,StageName(l.rank),rate);
      }
      l.bitrate = rate;
    }
    try {
      for (auto& l : layers_) if (l.bitrate) {
        if (l.nv) l.nv->SetRate(l.bitrate);
        if (l.mf) l.mf->SetRate(l.bitrate);
      }
    } catch (const std::exception& error) {
      logf("screen encoder SetRates failed: %s", error.what());
      if (!fallback_) Fallback(); else fatal_ = true;
    }
  }
  int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
    std::lock_guard lock(mutex_);
    NameThreadOnce(L"harmony-encode");
    if (!callback_ || layers_.empty() || fatal_) return -1;
    auto buffer = frame.video_frame_buffer();
    if (buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
      fprintf(stderr, "H264 encoder requires gpu::D3DFrameBuffer\n"); return -1;
    }
    auto* native = dynamic_cast<D3DFrameBuffer*>(buffer.get());
    if (!native) return -1;
    ++inputs_;
    std::vector<bool> due(layers_.size());
    for (size_t i = 0; i < layers_.size(); ++i) {
      auto& l = layers_[i];
      if (types && i < types->size() && (*types)[i] == webrtc::VideoFrameType::kVideoFrameKey) {
        l.key = true;
        logf("screen key request index=%zu rank=%s rtp=%u bitrate=%u",i,StageName(l.rank),frame.rtp_timestamp(),l.bitrate);
      }
      // A quarter of a period of tolerance: at 1 ms the top layer dropped
      // every frame that came a little early (selftest, 06/10/2026), and at
      // half a period a 30 fps layer fed at 60 would take every frame.
      due[i] = l.bitrate && LayerDue(frame.timestamp_us(), l.fps, capture_fps_, l.next_us);
      if (!due[i]) {
        l.stage->Drop();
        (l.bitrate ? l.cadenceSkip : l.inactiveSkip)->Record(0);
      }
    }
    std::vector<EncodedPacket> output(layers_.size());
    try {
      if (!fallback_ && FailAfter() > 0 && inputs_ == FailAfter()) throw std::runtime_error("selftest NVENC frame failure");
      Produce(*native, frame, due, output);
    } catch (const std::exception& error) {
      logf("screen encoder frame failed: %s", error.what());
      if (fallback_ || !Fallback()) return -1;
      // No callbacks were sent for this input: re-encode every due layer as an
      // IDR so fallback does not emit duplicates or broken reference chains.
      output.assign(layers_.size(), EncodedPacket{});
      try { Produce(*native, frame, due, output); }
      catch (const std::exception& error) { logf("screen encoder MF fallback frame failed: %s", error.what()); fatal_ = true; return -1; }
    }
    std::vector<size_t> delivery;
    for(size_t i=0;i<layers_.size();++i)delivery.push_back(i);
    // A switching receiver needs the requested IDR before a delta picture
    // from the old layer advances its capture clock past this same input.
    std::stable_sort(delivery.begin(),delivery.end(),[&](size_t a,size_t b) {
      return (due[a]&&output[a].key)>(due[b]&&output[b].key);
    });
    for (size_t position = 0; position < delivery.size(); ++position) {
      const size_t i=delivery[position];
      const bool last=position+1==delivery.size();
      if (!due[i]) {
        callback_->OnFrameDropped(frame.rtp_timestamp(), static_cast<int>(i), last);
        continue;
      }
      auto& l = layers_[i]; auto& packet = output[i];
      if(packet.key) {
        l.keyframes->Record(0);
        logf("screen encoder IDR index=%zu rank=%s bytes=%zu",i,StageName(l.rank),packet.bytes.size());
      }
      webrtc::EncodedImage encoded;
      encoded.SetEncodedData(webrtc::EncodedImageBuffer::Create(packet.bytes.data(), packet.bytes.size()));
      encoded.SetRtpTimestamp(frame.rtp_timestamp()); encoded.capture_time_ms_ = frame.timestamp_us() / 1000;
      encoded.ntp_time_ms_ = frame.ntp_time_ms(); encoded.rotation_ = frame.rotation();
      encoded._encodedWidth = l.width; encoded._encodedHeight = l.height;
      encoded.SetSimulcastIndex(static_cast<int>(i));
      encoded.set_end_of_temporal_unit(last);
      encoded._frameType = packet.key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
      encoded.qp_ = packet.qp;
      webrtc::CodecSpecificInfo specific{};
      specific.codecType = webrtc::kVideoCodecH264;
      specific.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
      specific.codecSpecific.H264.idr_frame = packet.key;
      l.bytes += packet.bytes.size();
      int64_t elapsed = perf::Stage::Now() - l.rate_start_ns;
      l.stage->SetValue(elapsed > 0 ? static_cast<int64_t>(l.bytes * 8000000ULL / elapsed) : 0);
      if (l.rank == 0 && native->captured_ns()) {
        static perf::Stage& stageLatencyCaptureToEncoded = perf::Get("latency.capture_to_encoded");
        stageLatencyCaptureToEncoded.Record(perf::Stage::Now() - native->captured_ns());
      }
      if (callback_->OnEncodedImage(encoded, &specific).error != webrtc::EncodedImageCallback::Result::OK) {
        logf("screen delivery failed index=%zu rank=%s rtp=%u pending=%d",i,StageName(l.rank),frame.rtp_timestamp(),l.key);
        return -1;
      }
      // Only an IDR accepted by the sender satisfies a pending request.
      // A delta packet or rejected callback must leave it for the next input.
      if (packet.key) l.key = false;
    }
    return 0;
  }
  EncoderInfo GetEncoderInfo() const override {
    std::lock_guard lock(mutex_);
    EncoderInfo info;
    info.supports_native_handle = true; info.supports_simulcast = true;
    info.is_hardware_accelerated = !fallback_; info.implementation_name = ActiveEncoderName();
    info.requested_resolution_alignment = 2; info.apply_alignment_to_all_simulcast_layers = true;
    info.scaling_settings = ScalingSettings::kOff;
    return info;
  }
 private:
  bool Fallback() {
    if (fallback_) return !fatal_;
    perf::Count("encoder.fallback");
    fallback_ = true;
    for (auto& l : layers_) { l.nv.reset(); l.key = true; }
    try {
      std::vector<std::unique_ptr<MFEncoder>> replacements;
      for (auto& l : layers_) replacements.push_back(std::make_unique<MFEncoder>(l.width, l.height,
          l.fps, std::max<uint32_t>(l.bitrate, 1000), high_));
      for (size_t i = 0; i < layers_.size(); ++i) layers_[i].mf = std::move(replacements[i]);
      SetActiveEncoderName("Media Foundation (Windows H.264) x" + std::to_string(layers_.size()));
      return true;
    } catch (const std::exception& error) {
      logf("screen encoder Media Foundation initialization failed: %s", error.what());
      SetActiveEncoderName("Media Foundation failed"); fatal_ = true; return false;
    }
  }
  void Produce(const D3DFrameBuffer& native, const webrtc::VideoFrame& frame,
               const std::vector<bool>& due, std::vector<EncodedPacket>& output) {
    // NVENC: every due layer submitted first, collected after (nvenc.h).
    try {
      for (size_t i = 0; i < layers_.size(); ++i)
        if (due[i] && layers_[i].nv)
          layers_[i].nv->Submit(native, frame.timestamp_us(), layers_[i].key, *layers_[i].stage);
      for (size_t i = 0; i < layers_.size(); ++i)
        if (due[i] && layers_[i].nv) output[i] = layers_[i].nv->Collect();
    } catch (...) {
      for (auto& l : layers_) if (l.nv) l.nv->Abort();
      throw;
    }
    for (size_t i = 0; i < layers_.size(); ++i) if (due[i] && !layers_[i].nv) {
      auto& l = layers_[i]; auto& stage = *l.stage;
      {
        auto scaled = Scale(native, l.width, l.height, DXGI_FORMAT_NV12);
        auto timer = stage.Begin();
        output[i].bytes = l.mf->Encode(*scaled, frame.timestamp_us(), l.key);
        NormalizeMF(output[i], l.sps, l.pps);
        if (l.key && !output[i].key) throw std::runtime_error("MF refused requested IDR");
        timer.Done();
      }
    }
  }
  mutable std::recursive_mutex mutex_;
  std::vector<Layer> layers_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  bool high_, fallback_ = false, fatal_ = false;
  int inputs_ = 0, capture_fps_ = 60;
};
class EncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    std::vector<webrtc::SdpVideoFormat> formats;
    for (const char* profile : {"42e02a", "64002a"})
      formats.emplace_back("H264", webrtc::SdpVideoFormat::Parameters{
          {"packetization-mode", "1"}, {"profile-level-id", profile}, {"level-asymmetry-allowed", "1"}});
    for (auto& format : builtin_->GetSupportedFormats())
      if (format.name == "VP8" || format.name == "VP9") formats.push_back(format);
    return formats;
  }
  std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
    if (format.name == "H264") {
      auto profile = format.parameters.find("profile-level-id");
      auto mode = format.parameters.find("packetization-mode");
      // The level in the negotiated line can differ from ours (the answer may
      // carry the other side's); what matters is the packetization and the
      // profile: high for 64/4d, constrained baseline for everything 42.
      if (mode != format.parameters.end() && mode->second != "1") return nullptr;
      bool high = profile != format.parameters.end() &&
                  (profile->second.rfind("64", 0) == 0 || profile->second.rfind("4d", 0) == 0);
      return std::make_unique<Encoder>(high);
    }
    if (format.name == "VP8" || format.name == "VP9") return builtin_->Create(env, format);
    return nullptr;
  }
 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> builtin_ = webrtc::CreateBuiltinVideoEncoderFactory();
};
}
void SetActiveEncoderName(std::string name) { std::lock_guard lock(name_mutex); active_name = std::move(name); logf("screen encoder selected=%s",active_name.c_str()); }
std::string ActiveEncoderName() { std::lock_guard lock(name_mutex); return active_name; }
void ConfigureSelftest(bool fallback, int frame) { force_fallback = fallback; fail_after = frame; }
bool ForceFallback() { return force_fallback.load(); }
int FailAfter() { return fail_after.load(); }
std::unique_ptr<webrtc::VideoEncoderFactory> CreateEncoderFactory() { return std::make_unique<EncoderFactory>(); }
}  // namespace gpu
