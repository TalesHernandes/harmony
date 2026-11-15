#include "mf.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include <map>
#include <cstdio>

namespace gpu {
namespace {
class Decoder : public webrtc::VideoDecoder {
  DeviceLease device_lease_;
 public:
  bool Configure(const Settings& settings) override {
    std::lock_guard lock(mutex_);
    try {
      StartMF();
      transform_.Reset(); pending_.clear();
      Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&transform_)), "Windows H264 decoder");
      UINT token = 0;
      Check(MFCreateDXGIDeviceManager(&token, &manager_), "DXGI device manager");
      Check(manager_->ResetDevice(Device(), token), "DXGI ResetDevice");
      ComPtr<IMFAttributes> attributes;
      Check(transform_->GetAttributes(&attributes), "Decoder attributes");
      UINT aware = 0;
      Check(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), "Decoder D3D11 awareness");
      if (!aware) throw std::runtime_error("Windows H264 decoder is not D3D11 aware");
      Check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Decoder low latency");
      Check(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
          reinterpret_cast<ULONG_PTR>(manager_.Get())), "Decoder D3D manager");
      auto size = settings.max_render_resolution();
      auto input = VideoType(MFVideoFormat_H264, std::max(2, size.Width()), std::max(2, size.Height()), 60);
      Check(transform_->SetInputType(0, input.Get(), 0), "Decoder H264 input");
      // Establish NV12 before ProcessInput; STREAM_CHANGE may refine the size
      // after the first SPS, including the decoder's aligned allocation size.
      ComPtr<IMFMediaType> output;
      bool selected = false;
      for (DWORD i = 0; SUCCEEDED(transform_->GetOutputAvailableType(0, i, &output)); ++i) {
        GUID subtype{}; Check(output->GetGUID(MF_MT_SUBTYPE, &subtype), "Initial decoder subtype");
        if (subtype == MFVideoFormat_NV12) {
          Check(transform_->SetOutputType(0, output.Get(), 0), "Initial decoder NV12");
          selected = true; break;
        }
        output.Reset();
      }
      if (!selected) throw std::runtime_error("Decoder has no initial NV12 output");
      Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Decoder begin streaming");
      Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Decoder start stream");
      return true;
    } catch (const std::exception& error) { fprintf(stderr, "Decoder Configure: %s\n", error.what()); return false; }
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
    std::lock_guard lock(mutex_); callback_ = callback; return 0;
  }
  int32_t Release() override {
    std::lock_guard lock(mutex_); transform_.Reset(); manager_.Reset(); pending_.clear(); return 0;
  }
  int32_t Decode(const webrtc::EncodedImage& input, bool, int64_t) override {
    std::lock_guard lock(mutex_);
    NameThreadOnce(L"harmony-decode");
    if (!transform_ || !callback_) return -1;
    static perf::Stage& stageDecodeH264 = perf::Get("decode.h264");
    auto timer = stageDecodeH264.Begin();
    try {
      auto sample = MemorySample(input.data(), input.size(), input.capture_time_ms_ * 1000, 60);
      Check(sample->SetUINT32(MFSampleExtension_CleanPoint, input._frameType == webrtc::VideoFrameType::kVideoFrameKey), "Decoder keyframe");
      HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
      if (hr == MF_E_NOTACCEPTING) { Pull(); hr = transform_->ProcessInput(0, sample.Get(), 0); }
      Check(hr, "Decoder ProcessInput");
      pending_[input.capture_time_ms_ * 10000] = {input.RtpTimestamp(), input.rotation_, input.ntp_time_ms_};
      Pull(&timer); timer.Done(); return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Decoder Decode: %s\n", error.what()); return -1; }
  }
  bool Drain() {
    std::lock_guard lock(mutex_);
    try {
      Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "Decoder end stream");
      Check(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Decoder drain");
      Pull(); return true;
    } catch (const std::exception& error) { fprintf(stderr, "Decoder Drain: %s\n", error.what()); return false; }
  }
  DecoderInfo GetDecoderInfo() const override { return {"Media Foundation D3D11 DXVA", true}; }
 private:
  struct Metadata { uint32_t rtp; webrtc::VideoRotation rotation; int64_t ntp; };
  void Pull(perf::Stage::Timer* elapsed = nullptr) {
    while (auto sample = OutputSample(transform_.Get(), true)) {
      ComPtr<IMFMediaBuffer> buffer;
      Check(sample->GetBufferByIndex(0, &buffer), "Decoder buffer");
      ComPtr<IMFDXGIBuffer> dxgi;
      Check(buffer.As(&dxgi), "Decoder output must be DXGI (no CPU fallback)");
      ComPtr<ID3D11Texture2D> texture;
      Check(dxgi->GetResource(IID_PPV_ARGS(&texture)), "Decoder texture");
      UINT subresource = 0; Check(dxgi->GetSubresourceIndex(&subresource), "Decoder subresource");
      D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_NV12) throw std::runtime_error("DXVA output is not NV12");
      LONGLONG time = 0; Check(sample->GetSampleTime(&time), "Decoded sample time");
      Metadata metadata{static_cast<uint32_t>(time * 9 / 1000), webrtc::kVideoRotation_0, 0};
      auto found = pending_.find(time);
      if (found != pending_.end()) { metadata = found->second; pending_.erase(found); }
      ComPtr<IMFMediaType> type;
      Check(transform_->GetOutputCurrentType(0, &type), "Decoded output type");
      UINT width = 0, height = 0;
      Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Decoded visible size");
      auto native = webrtc::make_ref_counted<D3DFrameBuffer>(texture.Get(), subresource, 0, sample.Get(), width, height);
      auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(native).set_timestamp_us(time / 10)
          .set_rtp_timestamp(metadata.rtp).set_rotation(metadata.rotation).set_ntp_time_ms(metadata.ntp).build();
      // A sink may wait for its display clock. Attribute that wait to the
      // sink, otherwise --present would inflate the decoder's own time.
      if (elapsed) elapsed->Done();
      if (callback_->Decoded(frame) != 0) throw std::runtime_error("Decoded callback rejected frame");
    }
  }
  std::recursive_mutex mutex_;
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFDXGIDeviceManager> manager_;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  std::map<int64_t, Metadata> pending_;
};
class DecoderFactory : public webrtc::VideoDecoderFactory {
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
  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
    if (format.name == "H264") {
      auto profile = format.parameters.find("profile-level-id");
      auto mode = format.parameters.find("packetization-mode");
      if (profile == format.parameters.end() || mode == format.parameters.end() || mode->second != "1" ||
          (profile->second != "42e02a" && profile->second != "64002a")) return nullptr;
      return std::make_unique<Decoder>();
    }
    if (format.name == "VP8" || format.name == "VP9") return builtin_->Create(env, format);
    return nullptr;
  }
 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> builtin_ = webrtc::CreateBuiltinVideoDecoderFactory();
};
}
bool DrainDecoder(webrtc::VideoDecoder* decoder) {
  auto* native = dynamic_cast<Decoder*>(decoder);
  return native && native->Drain();
}
std::unique_ptr<webrtc::VideoDecoderFactory> CreateDecoderFactory() { return std::make_unique<DecoderFactory>(); }
}  // namespace gpu
