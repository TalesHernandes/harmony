#include "internal.h"
#include "libyuv.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <cstdio>

namespace gpu {
namespace {
class D3DPresenter : public Presenter {
  DeviceLease device_lease_;
 public:
  explicit D3DPresenter(HWND target) : target_(target) {
    ComPtr<IDXGIDevice> device;
    Check(Device()->QueryInterface(IID_PPV_ARGS(&device)), "Presenter DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    Check(device->GetAdapter(&adapter), "Presenter adapter");
    ComPtr<IDXGIFactory2> factory;
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Presenter factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Check(factory->CreateSwapChainForHwnd(Device(), target, &desc, nullptr, nullptr, &swap_), "Presenter swap chain");
    Check(factory->MakeWindowAssociation(target, DXGI_MWA_NO_ALT_ENTER), "Presenter window association");
  }
  void OnFrame(const webrtc::VideoFrame& frame) override {
    NameThreadOnce(L"harmony-present");
    static perf::Stage& stagePresent = perf::Get("present");
    auto timer = stagePresent.Begin();
    try {
      std::lock_guard lock(ContextMutex());
      RECT rect{}; if (!GetClientRect(target_, &rect)) return;
      int width = rect.right, height = rect.bottom;
      if (width <= 0 || height <= 0) { timer.Drop(); return; }
      if (width != width_ || height != height_) {
        // Blit's cached view holds the back buffer, and a held buffer cannot resize.
        { ComPtr<ID3D11Texture2D> held; if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&held)))) ForgetOutputViews(held.Get()); }
        Check(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Presenter resize");
        width_ = width; height_ = height;
      }
      auto buffer = frame.video_frame_buffer();
      webrtc::scoped_refptr<D3DFrameBuffer> uploaded;
      D3DFrameBuffer* native = nullptr;
      if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
        native = dynamic_cast<D3DFrameBuffer*>(buffer.get());
        if (!native) throw std::runtime_error("Unknown native presenter buffer");
      } else {
        auto i420 = buffer->ToI420();
        if (!i420) throw std::runtime_error("Presenter I420 conversion failed");
        std::vector<uint8_t> bgra(static_cast<size_t>(i420->width()) * i420->height() * 4);
        if (libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
            i420->DataV(), i420->StrideV(), bgra.data(), i420->width() * 4, i420->width(), i420->height()) != 0)
          throw std::runtime_error("Presenter BGRA conversion failed");
        auto texture = Texture(i420->width(), i420->height(), DXGI_FORMAT_B8G8R8A8_UNORM);
        Context()->UpdateSubresource(texture.Get(), 0, nullptr, bgra.data(), i420->width() * 4, 0);
        uploaded = webrtc::make_ref_counted<D3DFrameBuffer>(texture.Get());
        native = uploaded.get();
      }
      ComPtr<ID3D11Texture2D> backbuffer;
      Check(swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Presenter backbuffer");
      // A phone camera arrives sideways with the turn to apply alongside (CVO); Blit turns it clockwise.
      int turn = static_cast<int>(frame.rotation()) / 90;
      int shown_width = turn % 2 ? native->height() : native->width(), shown_height = turn % 2 ? native->width() : native->height();
      double scale = std::min(static_cast<double>(width) / shown_width, static_cast<double>(height) / shown_height);
      int out_width = static_cast<int>(shown_width * scale), out_height = static_cast<int>(shown_height * scale);
      int x = (width - out_width) / 2, y = (height - out_height) / 2;
      Blit(*native, backbuffer.Get(), RECT{0,0,native->width(),native->height()}, RECT{x,y,x+out_width,y+out_height}, turn);
      Check(swap_->Present(0, 0), "Presenter Present");
      timer.Done();
    } catch (const std::exception& error) { fprintf(stderr, "Presenter: %s\n", error.what()); }
  }
 private:
  HWND target_;
  int width_ = 0, height_ = 0;
  ComPtr<IDXGISwapChain1> swap_;
};
}
std::unique_ptr<Presenter> Presenter::Create(HWND target) {
  try { return std::make_unique<D3DPresenter>(target); }
  catch (const std::exception& error) { fprintf(stderr, "Presenter Create: %s\n", error.what()); return nullptr; }
}
}  // namespace gpu
