#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include "api/media_stream_interface.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"

namespace gpu {
ID3D11Device* Device();
// Enable only after every UI swap-chain owner also holds a DeviceLease.
// The WinUI integration is owned by another worker; default stays safe.
void EnableIdleDeviceRelease();
// Every asynchronous GPU consumer keeps the shared device alive. The last
// consumer releases it, including the driver's context and worker resources.
class DeviceLease {
 public:
  DeviceLease();
  ~DeviceLease();
  DeviceLease(const DeviceLease&)=delete;
  DeviceLease& operator=(const DeviceLease&)=delete;
};

// Retain the capture/decoder owner so its texture cannot be recycled by a pool
// while downstream sinks still hold this frame.
class D3DFrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  D3DFrameBuffer(ID3D11Texture2D* texture, UINT subresource = 0,
                 int64_t captured_ns = 0, IUnknown* owner = nullptr,
                 int visible_width = 0, int visible_height = 0);
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  ID3D11Texture2D* texture() const { return texture_.Get(); }
  UINT subresource() const { return subresource_; }
  DXGI_FORMAT format() const { return format_; }
  int64_t captured_ns() const { return captured_ns_; }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
      int x, int y, int width, int height, int out_width, int out_height) override;
 private:
  DeviceLease device_lease_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<IUnknown> owner_;
  UINT subresource_;
  int width_, height_;
  DXGI_FORMAT format_;
  int64_t captured_ns_;
};
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateWindowSource(HWND window, int max_fps, int max_height=1080);
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateMonitorSource(HMONITOR monitor, int max_fps, int max_height=1080);
// Current target of the real GPU source returned by CreateWindowSource,
// including successors. Pass the original source, never GetSource()'s proxy.
HWND CaptureWindow(webrtc::VideoTrackSourceInterface* source);
std::unique_ptr<webrtc::VideoEncoderFactory> CreateEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> CreateDecoderFactory();
std::string ActiveEncoderName();
// Explicit local audit injection. Production uses WGC with automatic fallback
// and asynchronous NVENC; only the loopback-only screen selftest sets these.
void ConfigureCaptureSelftest(bool desktop_duplication);
// Image-only loopback probe: 0 = normal, 1 = stalled fullscreen WGC window,
// 2 = black fullscreen WGC window. The monitor acquisition stays real.
void ConfigureCaptureFaultSelftest(int mode);
void ConfigureNVEncSelftest(bool synchronous);
class Presenter : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  static std::unique_ptr<Presenter> Create(HWND target);
};
}  // namespace gpu
