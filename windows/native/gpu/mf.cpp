#include "mf.h"
#include "libyuv.h"
#include <cstring>

namespace gpu {
void StartMF() {
  struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Apartment() { if (status != RPC_E_CHANGED_MODE) Check(status, "MF thread COM"); }
    ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
  };
  thread_local Apartment apartment;
  static const HRESULT status = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  Check(status, "MFStartup");
}
void CodecValue(IMFTransform* transform, const GUID& key, ULONG value) {
  ComPtr<ICodecAPI> codec;
  Check(transform->QueryInterface(IID_PPV_ARGS(&codec)), "ICodecAPI");
  VARIANT parameter{}; parameter.vt = VT_UI4; parameter.ulVal = value;
  Check(codec->SetValue(&key, &parameter), "CodecAPI SetValue");
}
ComPtr<IMFMediaType> VideoType(GUID subtype, int width, int height, int fps) {
  ComPtr<IMFMediaType> type;
  Check(MFCreateMediaType(&type), "MFCreateMediaType");
  Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Video major type");
  Check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Video subtype");
  Check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "Frame size");
  Check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1), "Frame rate");
  Check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Pixel aspect");
  Check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Interlace mode");
  return type;
}
ComPtr<IMFSample> MemorySample(const uint8_t* bytes, size_t size, int64_t time_us, int fps) {
  ComPtr<IMFMediaBuffer> buffer;
  Check(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer), "MFCreateMemoryBuffer");
  BYTE* output = nullptr;
  Check(buffer->Lock(&output, nullptr, nullptr), "MF buffer Lock");
  memcpy(output, bytes, size);
  Check(buffer->Unlock(), "MF buffer Unlock");
  Check(buffer->SetCurrentLength(static_cast<DWORD>(size)), "MF buffer length");
  ComPtr<IMFSample> sample;
  Check(MFCreateSample(&sample), "MFCreateSample");
  Check(sample->AddBuffer(buffer.Get()), "AddBuffer");
  Check(sample->SetSampleTime(time_us * 10), "Sample time");
  Check(sample->SetSampleDuration(10000000LL / fps), "Sample duration");
  return sample;
}
ComPtr<IMFSample> OutputSample(IMFTransform* transform, bool decode) {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO info{};
    Check(transform->GetOutputStreamInfo(0, &info), "OutputStreamInfo");
    ComPtr<IMFSample> allocated;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
      if (decode) throw std::runtime_error("DXVA decoder did not provide GPU samples");
      Check(MFCreateSample(&allocated), "Output sample");
      ComPtr<IMFMediaBuffer> buffer;
      Check(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Output buffer");
      Check(allocated->AddBuffer(buffer.Get()), "Output AddBuffer");
    }
    MFT_OUTPUT_DATA_BUFFER output{}; output.pSample = allocated.Get();
    DWORD status = 0;
    HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    ComPtr<IMFSample> sample = allocated;
    if (!allocated && output.pSample) sample.Attach(output.pSample);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return nullptr;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      ComPtr<IMFMediaType> type;
      bool selected = false;
      for (DWORD i = 0; SUCCEEDED(transform->GetOutputAvailableType(0, i, &type)); ++i) {
        GUID subtype{}; Check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Decoder subtype");
        if (subtype == MFVideoFormat_NV12) {
          Check(transform->SetOutputType(0, type.Get(), 0), "Decoder NV12 output");
          selected = true; break;
        }
        type.Reset();
      }
      if (!selected) throw std::runtime_error("DXVA decoder has no NV12 output");
      continue;
    }
    Check(hr, "ProcessOutput");
    return sample;
  }
}
MFEncoder::MFEncoder(int width, int height, int fps, uint32_t bitrate, bool high_profile)
    : width_(width), height_(height), fps_(fps) {
  StartMF();
  // The synchronous Windows encoder is available even when the vendor driver
  // denies sessions. This deliberately avoids reusing a failing NVENC MFT.
  Check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&transform_)), "Windows H264 encoder");
  ComPtr<IMFAttributes> attrs;
  if (SUCCEEDED(transform_->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
  auto output = VideoType(MFVideoFormat_H264, width, height, fps);
  Check(output->SetUINT32(MF_MT_AVG_BITRATE, bitrate), "MF bitrate");
  Check(output->SetUINT32(MF_MT_MPEG2_PROFILE, high_profile ? eAVEncH264VProfile_High : eAVEncH264VProfile_Base), "MF profile");
  Check(output->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_2), "MF level");
  Check(transform_->SetOutputType(0, output.Get(), 0), "Encoder H264 output");
  auto input = VideoType(MFVideoFormat_NV12, width, height, fps);
  Check(transform_->SetInputType(0, input.Get(), 0), "Encoder NV12 input");
  CodecValue(transform_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  CodecValue(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
  CodecValue(transform_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  CodecValue(transform_.Get(), CODECAPI_AVEncMPVGOPSize, 0xffffffff);
  Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MF begin streaming");
  Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MF start stream");
}
void MFEncoder::SetRate(uint32_t bitrate) {
  CodecValue(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
}
std::vector<uint8_t> MFEncoder::Encode(const D3DFrameBuffer& frame, int64_t time_us, bool key) {
  // The layer is scaled and converted to NV12 together on the GPU. Read
  // those planes directly, without the old intermediate I420 allocation.
  webrtc::scoped_refptr<D3DFrameBuffer> converted;
  const D3DFrameBuffer* input = &frame;
  if (frame.format() != DXGI_FORMAT_NV12 || frame.width() != width_ || frame.height() != height_) {
    converted = Scale(frame, width_, height_, DXGI_FORMAT_NV12); input = converted.get();
  }
  nv12_.resize(static_cast<size_t>(width_) * height_ * 3 / 2);
  {
    std::lock_guard lock(ContextMutex());
    D3D11_TEXTURE2D_DESC desc{}; input->texture()->GetDesc(&desc);
    if (!staging_) {
      desc.Width = width_; desc.Height = height_;
      desc.ArraySize = 1; desc.MipLevels = 1; desc.BindFlags = 0; desc.MiscFlags = 0;
      desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Check(Device()->CreateTexture2D(&desc, nullptr, &staging_), "MF NV12 staging");
    }
    auto context = Context();
    D3D11_BOX area{0, 0, 0, UINT(width_), UINT(height_), 1};
    context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, input->texture(), input->subresource(), &area);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "MF NV12 readback");
    auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    int status = libyuv::NV12Copy(bytes, mapped.RowPitch, bytes + mapped.RowPitch * height_, mapped.RowPitch,
        nv12_.data(), width_, nv12_.data() + width_ * height_, width_, width_, height_);
    context->Unmap(staging_.Get(), 0);
    Check(status == 0 ? S_OK : E_FAIL, "MF NV12 plane copy");
    static perf::Stage& copies = perf::Get("gpu.cpu_copies"); copies.Record(0);
  }
  if (key) CodecValue(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
  auto sample = MemorySample(nv12_.data(), nv12_.size(), time_us, fps_);
  Check(transform_->ProcessInput(0, sample.Get(), 0), "Encoder ProcessInput");
  auto encoded = OutputSample(transform_.Get(), false);
  if (!encoded) throw std::runtime_error("Low latency MF encoder buffered a frame");
  ComPtr<IMFMediaBuffer> buffer;
  Check(encoded->ConvertToContiguousBuffer(&buffer), "Encoded buffer");
  BYTE* data = nullptr; DWORD size = 0;
  Check(buffer->Lock(&data, nullptr, &size), "Encoded Lock");
  std::vector<uint8_t> bytes(data, data + size);
  Check(buffer->Unlock(), "Encoded Unlock");
  return bytes;
}
}  // namespace gpu
