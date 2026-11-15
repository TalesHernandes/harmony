#pragma once
#include "internal.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

namespace gpu {
void StartMF();
void CodecValue(IMFTransform* transform, const GUID& key, ULONG value);
ComPtr<IMFMediaType> VideoType(GUID subtype, int width, int height, int fps);
ComPtr<IMFSample> MemorySample(const uint8_t* bytes, size_t size, int64_t time_us, int fps);
// Returns null only when the transform needs more input. Every other failure is
// explicit, including unsupported DXVA output rather than a hidden CPU decoder.
ComPtr<IMFSample> OutputSample(IMFTransform* transform, bool decode);
class MFEncoder {
 public:
  MFEncoder(int width, int height, int fps, uint32_t bitrate, bool high_profile);
  std::vector<uint8_t> Encode(const D3DFrameBuffer& frame, int64_t time_us, bool key);
  void SetRate(uint32_t bitrate);
 private:
  ComPtr<IMFTransform> transform_;
  ComPtr<ID3D11Texture2D> staging_;
  std::vector<uint8_t> nv12_;
  int width_, height_, fps_;
};
}  // namespace gpu

