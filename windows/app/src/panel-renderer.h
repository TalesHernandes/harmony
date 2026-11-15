#pragma once
#include <algorithm>
#include <chrono>
namespace app {
enum class PanelCadence { Camera, Screen };
constexpr std::chrono::microseconds PanelInterval(int monitorHz, PanelCadence cadence) {
  return std::chrono::microseconds(1000000 / std::clamp(monitorHz, 1, cadence == PanelCadence::Screen ? 60 : 30));
}
}
#ifndef HARMONY_PANEL_POLICY_ONLY
#include "../../native/gpu/internal.h"
#include "libyuv.h"
#include <dxgi1_2.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <thread>
namespace app {
// The W1 D3D presenter, hosted by WinUI rather than a separate HWND.
class PanelRenderer final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
  gpu::DeviceLease device_lease_; // first, so it is released after the swap chain
 public:
  explicit PanelRenderer(IUnknown* panel, PanelCadence cadence = PanelCadence::Camera) {
    gpu::Check(panel->QueryInterface(IID_PPV_ARGS(&panel_)),"WinUI swap panel");
    gpu::ComPtr<IDXGIDevice> device;gpu::Check(gpu::Device()->QueryInterface(IID_PPV_ARGS(&device)),"Tile DXGI device");
    gpu::ComPtr<IDXGIAdapter> adapter;device->GetAdapter(&adapter);gpu::ComPtr<IDXGIFactory2> factory;adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=2;desc.Height=2;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    gpu::Check(factory->CreateSwapChainForComposition(gpu::Device(),&desc,nullptr,&swap_),"Tile swap chain");gpu::Check(panel_->SetSwapChain(swap_.Get()),"Tile attach on UI");
    HDC dc=GetDC(nullptr);int hz=GetDeviceCaps(dc,VREFRESH);ReleaseDC(nullptr,dc);
    interval_=PanelInterval(hz,cadence);
    worker_=std::thread([this]{Run();});
  }
  ~PanelRenderer() override {Attach(nullptr);{std::lock_guard lock(mailbox_);stopping_=true;}wake_.notify_one();if(worker_.joinable())worker_.join();}
  void Unload() {if(panel_)panel_->SetSwapChain(nullptr);panel_.Reset();}
  std::atomic<int> presented{0};   // pictures on screen, for the W6 round
  void Size(int width,int height) {width_=std::max(0,width);height_=std::max(0,height);}
  // Fill the panel, cropping the picture's sides (cameras), instead of
  // fitting it whole with black around it (screens).
  void Cover(bool on) {cover_=on;}
  // The last drawing failure, for the audits.
  std::string LastError() {std::lock_guard lock(error_mu_);return last_error_;}
  void Attach(webrtc::VideoTrackInterface* track) {if(track_)track_->RemoveSink(this);track_=track;if(track_) {webrtc::VideoSinkWants wants;track_->AddOrUpdateSink(this,wants);}}
  void OnFrame(const webrtc::VideoFrame& frame) override {
    if(width_<2||height_<2)return;
    static perf::Stage& stageTileCallback = perf::Get("tile.callback");
    auto timer=stageTileCallback.Begin();
    {std::lock_guard lock(mailbox_);pending_=frame;}
    wake_.notify_one();timer.Done();
  }
 private:
  void Run() {
    perf::NameThread(L"harmony-preview");
    auto next=std::chrono::steady_clock::now();
    for(;;) {
      std::optional<webrtc::VideoFrame> frame;
      {std::unique_lock lock(mailbox_);wake_.wait(lock,[this]{return stopping_||pending_.has_value();});if(stopping_)return;
       wake_.wait_until(lock,next,[this]{return stopping_;});if(stopping_)return;frame.swap(pending_);}
      if(frame)Draw(*frame);
      next=std::chrono::steady_clock::now()+interval_;
    }
  }
  void Draw(const webrtc::VideoFrame& frame) {
    int width=width_,height=height_;if(width<2||height<2)return;
    try {
      auto buffer=frame.video_frame_buffer();gpu::D3DFrameBuffer* native=dynamic_cast<gpu::D3DFrameBuffer*>(buffer.get());
      webrtc::scoped_refptr<webrtc::I420BufferInterface> i420;
      auto& pixels = pixels_;
      if(!native) {
        i420=buffer->ToI420();if(!i420)return;pixels.resize(size_t(i420->width())*i420->height()*4);
        libyuv::I420ToARGB(i420->DataY(),i420->StrideY(),i420->DataU(),i420->StrideU(),i420->DataV(),i420->StrideV(),pixels.data(),i420->width()*4,i420->width(),i420->height());
      }
      static perf::Stage& stageTilePresent = perf::Get("tile.present");
      auto timer=stageTilePresent.Begin();std::unique_lock lock(gpu::ContextMutex(),std::try_to_lock);
      if(!lock.owns_lock()){timer.Drop();static perf::Stage& contextBusy=perf::Get("tile.context.busy");contextBusy.Record(0);return;}
      if(width!=drawn_width_||height!=drawn_height_) {
        // Blit keeps a view onto the back buffer; while it does, the resize
        // fails and the picture stays at its old size (the owner, 29/10/2026:
        // made bigger, "a resolução fica estática e feia").
        {gpu::ComPtr<ID3D11Texture2D> held;if(SUCCEEDED(swap_->GetBuffer(0,IID_PPV_ARGS(&held))))gpu::ForgetOutputViews(held.Get());}
        gpu::Check(swap_->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0),"Tile resize");drawn_width_=width;drawn_height_=height;}
      if(!native) {
        if(!uploaded_ || uploaded_->width()!=i420->width() || uploaded_->height()!=i420->height()) {
          auto texture=gpu::Texture(i420->width(),i420->height(),DXGI_FORMAT_B8G8R8A8_UNORM);
          uploaded_=webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get());
        }
        gpu::Context()->UpdateSubresource(uploaded_->texture(),0,nullptr,pixels.data(),i420->width()*4,0);native=uploaded_.get();
      }
      gpu::ComPtr<ID3D11Texture2D> backbuffer;gpu::Check(swap_->GetBuffer(0,IID_PPV_ARGS(&backbuffer)),"Tile backbuffer");
      // A phone camera arrives sideways with the turn to apply alongside (CVO); Blit turns it clockwise.
      int turn=int(frame.rotation())/90,shown_w=turn%2?native->height():native->width(),shown_h=turn%2?native->width():native->height();
      RECT source{0,0,native->width(),native->height()},target{};
      // A camera standing up in a block lying down (a phone as webcam) is
      // shown whole: filling the block kept only the chin (the owner,
      // 02/11/2026). Same orientation, it fills, as before.
      bool sameWay=(shown_w>=shown_h)==(width>=height);
      if(cover_&&sameWay) {
        // A camera fills its rounded block (the web's object-cover): the
        // picture's middle is kept, nothing outside it is left undrawn.
        double scale=std::max(double(width)/shown_w,double(height)/shown_h);int cw=int(width/scale),ch=int(height/scale);
        if(turn%2) std::swap(cw,ch);
        int sx=(native->width()-cw)/2,sy=(native->height()-ch)/2;source=RECT{sx,sy,sx+cw,sy+ch};target=RECT{0,0,width,height};
      } else {
        double scale=std::min(double(width)/shown_w,double(height)/shown_h);int w=int(shown_w*scale),h=int(shown_h*scale),x=(width-w)/2,y=(height-h)/2;
        target=RECT{x,y,x+w,y+h};
      }
      gpu::Blit(*native,backbuffer.Get(),source,target,turn);lock.unlock();gpu::Check(swap_->Present(0,DXGI_PRESENT_DO_NOT_WAIT),"Tile present");timer.Done();++presented;
    } catch(const std::exception& e) {perf::Count("tile.present.failure");std::lock_guard lock(error_mu_);last_error_=e.what();
    } catch(...) {perf::Count("tile.present.failure");}
  }
 private:
  // Draw runs on one worker; upload and Blit are ordered on the same context.
  std::vector<uint8_t> pixels_;
  webrtc::scoped_refptr<gpu::D3DFrameBuffer> uploaded_;
  gpu::ComPtr<ISwapChainPanelNative> panel_;gpu::ComPtr<IDXGISwapChain1> swap_;webrtc::scoped_refptr<webrtc::VideoTrackInterface> track_;
  std::mutex mailbox_;std::condition_variable wake_;std::optional<webrtc::VideoFrame> pending_;bool stopping_=false;std::thread worker_;std::chrono::microseconds interval_{33333};
  std::atomic<bool> cover_{false};std::mutex error_mu_;std::string last_error_;
  std::atomic<int> width_{0},height_{0};int drawn_width_=0,drawn_height_=0;
};
}

#endif
