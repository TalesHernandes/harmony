#include "internal.h"
#include "cadence.h"
#include "rtc_base/time_utils.h"
#include "sync-readback.h"
#include "capture-hold.h"
#ifndef HARMONY_CAPTURE_TEST
#include "capture-notice.h"
#include "capture-health.h"
#include "../src/follow-window.h"
#endif
#include "api/video/adapted_video_track_source.h"
#ifndef HARMONY_CAPTURE_TEST
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#endif
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dxgi1_2.h>
#include "../src/log.h"

namespace gpu {
struct CaptureRegression;
namespace {
std::atomic<bool> force_duplication{false};
std::atomic<int> capture_fault{0};
#ifndef HARMONY_CAPTURE_TEST
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
#endif
class CaptureSource : public webrtc::AdaptedVideoTrackSource {
#ifndef HARMONY_CAPTURE_TEST
  DeviceLease device_lease_;
#endif
 public:
  CaptureSource(HWND window, HMONITOR monitor, int fps,int height) : fps_(std::clamp(fps, 1, 60)), height_(height), capture_fps_(fps_) {
#ifndef HARMONY_CAPTURE_TEST
    original_exe_=window?media::FollowExecutable(window):std::wstring();
#endif
    // Capture owns an MTA thread so callers need no dispatcher or WinRT setup.
    worker_ = std::thread([this, window, monitor] { Run(window, monitor); });
  }
  ~CaptureSource() override { stop_ = true; if (worker_.joinable()) worker_.join(); }
  HWND window() const {return window_.load();}
  SourceState state() const override { return ended_ ? kEnded : ready_ ? kLive : kInitializing; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return true; }
  std::optional<bool> needs_denoising() const override { return false; }
  void ProcessConstraints(const webrtc::VideoTrackSourceConstraints& constraints) override {
    // Session restores the requested rate before enabling an encoding. Keep
    // this separate from sink wants: their minimum would cap every viewer.
    capture_fps_=constraints.max_fps ? std::clamp(int(*constraints.max_fps),1,fps_) : fps_;
  }
 private:
  friend struct gpu::CaptureRegression;
  // Opening is proven by a usable capture frame, not by a consumer accepting it.
  // Session deliberately waits for kLive before creating the first track/sink.
  bool PrepareFrame(int width,int height,int64_t time,int* ow,int* oh,int* cw,int* ch,int* cx,int* cy) {
    if(width<=0||height<=0)return false;
    if(!ready_.exchange(true)) logf("screen open step=first_frame width=%d height=%d",width,height);
    const int fps=capture_fps_.load();
    if(fps!=paced_fps_) {paced_fps_=fps;next_frame_us_=0;}
    // The idle preview uses PanelRenderer's 1e6/min(refresh,30) interval.
    // Do not use the half-period capture tolerance here: preview-only work
    // must never run ahead of its drawing cadence. Reset on audience growth.
    if(fps<fps_) {
      if(next_frame_us_ && time<next_frame_us_) return false;
      next_frame_us_=time+1000000LL/fps;
    }
    return AdaptFrame(width,height,time,ow,oh,cw,ch,cx,cy);
  }
#ifndef HARMONY_CAPTURE_TEST
  void Repeat(HoldReason reason) {
    const auto now=webrtc::TimeMicros();
    if(!hold_.Due(now,bool(last_image_),reason))return;
    if(reason==HoldReason::None)reason=HoldReason::Stalled;
    if(!notice_image_||reason!=hold_.notice) {
      notice_image_=CaptureNotice(last_image_.Get(),reason);
      logf("screen hold reason=%s fps=5",reason==HoldReason::Minimized?"minimized":reason==HoldReason::Closed?"closed":"stalled");
    }
    hold_.notice=reason;
    const int64_t stamp=hold_.Stamp(now);
    D3D11_TEXTURE2D_DESC desc{};notice_image_->GetDesc(&desc);
    int ow,oh,cw,ch,cx,cy;
    if(!PrepareFrame(desc.Width,desc.Height,stamp,&ow,&oh,&cw,&ch,&cx,&cy))return;
    const double cap=std::min({1.0,double(height_)/oh,double(height_*16/9)/ow});
    ow=std::max(2,int(ow*cap)/2*2);oh=std::max(2,int(oh*cap)/2*2);
    auto buffer=webrtc::make_ref_counted<D3DFrameBuffer>(notice_image_.Get(),0,perf::Stage::Now());
    auto adapted=buffer->CropAndScale(cx,cy,cw,ch,ow,oh);
    OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(adapted).set_timestamp_us(stamp)
        .set_rtp_timestamp(static_cast<uint32_t>(stamp*90/1000)).set_rotation(webrtc::kVideoRotation_0).build());
    static perf::Stage& heldFrames=perf::Get("capture.hold");heldFrames.Record(0);
  }
  static bool FullscreenWindow(HWND window,HMONITOR* monitor,RECT* crop) {
    if(!window||!IsWindow(window)||IsIconic(window)||GetAncestor(GetForegroundWindow(),GA_ROOT)!=window)return false;
    *monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof info};
    if(!GetMonitorInfoW(*monitor,&info)||!GetWindowRect(window,crop))return false;
    const auto& r=info.rcMonitor;
    return crop->left<=r.left&&crop->top<=r.top&&crop->right>=r.right&&crop->bottom>=r.bottom;
  }
#endif
  void Run(HWND window, HMONITOR monitor) {
#ifdef HARMONY_CAPTURE_TEST
    // Only the OS frame acquisition edge is controlled. Readiness, adaptation,
    // state, lifetime and the capture-owned MTA worker are production code.
    auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    while(!stop_) {
      int ow,oh,cw,ch,cx,cy;
      PrepareFrame(640,360,perf::Stage::Now()/1000,&ow,&oh,&cw,&ch,&cx,&cy);
      Sleep(5);
    }
    ended_=true;
    if(SUCCEEDED(apartment))CoUninitialize();
#else
    window_=window;
    const auto& original_exe=original_exe_;
    auto current_exe=original_exe;
    media::FollowSearch follow;
    NameThreadOnce(L"harmony-capture");
    const char* operation = "initialize MTA";
    auto step=[&](const char* name){operation=name;logf("screen open step=%s",name);};
    step(operation);
    try {
      struct Apartment {
        Apartment() { init_apartment(apartment_type::multi_threaded); }
        ~Apartment() { uninit_apartment(); }
      } apartment;
      if(monitor&&force_duplication.load()) {
        logf("screen capture selftest forced=DesktopDuplication");
        Duplicate(monitor);ended_=true;return;
      }
      step("GraphicsCaptureSession::IsSupported");
      if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("WGC unsupported");
      step("GraphicsCaptureItem activation factory");
      auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      GraphicsCaptureItem item{nullptr};
      step(window?"CreateForWindow":"CreateForMonitor");
      if (window) Check(interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), put_abi(item)), "WGC window");
      else Check(interop->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item)), "WGC monitor");
      step("WGC_created");
      step("D3D_device");
      ComPtr<IDXGIDevice> dxgi;
      Check(Device()->QueryInterface(IID_PPV_ARGS(&dxgi)), "IDXGIDevice");
      com_ptr<IInspectable> inspectable;
      Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "WinRT D3D11 device");
      auto device = inspectable.as<IDirect3DDevice>();
      auto size = item.Size();
      step("CreateFreeThreaded frame pool");
      auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(device,
          DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
      step("CreateCaptureSession");
      auto session = pool.CreateCaptureSession(item);
      step("IsCursorCaptureEnabled");
      session.IsCursorCaptureEnabled(true);
      // No yellow frame around the shared window (the owner saw it on the
      // game, 06/10/2026). Windows 11 asks for borderless access first; an
      // older Windows has neither call and keeps its border.
      try {
        step("borderless_access");
        auto access=GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
        if(access.wait_for(std::chrono::milliseconds(1000))==winrt::Windows::Foundation::AsyncStatus::Completed) {
          if(access.GetResults()==winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed) session.IsBorderRequired(false);
        } else {access.Cancel();logf("screen open timeout step=borderless_access deadline_ms=1000; retain border");}
      } catch (...) {}
      // Windows 11 24H2 spaces capture frames by MinUpdateInterval. Left at its
      // default, a 60 Hz window lost ~12% of its frames (every one that came a
      // little early), so it is 1 ms and the cap below does the pacing. A
      // 190 fps game then delivers every frame and we drop five of six; that
      // costs little (no copy for a dropped one). Half our own interval was
      // tried and lost ~10% of a 60 Hz window too (selftest, 06/10/2026).
      // Older Windows lacks the property, which is fine.
      try {
        session.MinUpdateInterval(
            winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(1)});
      } catch (...) {}
      std::atomic<bool> source_closed{false};
      auto closed = item.Closed(auto_revoke, [&source_closed](auto&&, auto&&) { source_closed = true; });
      // Woken by Windows when a frame lands, instead of polling: Sleep(1) is
      // 15.6 ms at the default timer resolution, which alone held a 60 Hz
      // window to ~47 frames (measured 05/10/2026).
      HANDLE arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!arrived) throw std::runtime_error("CreateEvent");
      // Closed however the loop ends, an exception included.
      std::unique_ptr<void, decltype(&CloseHandle)> arrived_owner(arrived, &CloseHandle);
      auto on_frame = pool.FrameArrived(auto_revoke, [arrived](auto&&, auto&&) { SetEvent(arrived); });
      step("StartCapture");
      session.StartCapture();
      step("first_frame_wait");
      auto first_deadline=GetTickCount64()+3000;
      LARGE_INTEGER qpc, frequency;
      QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&qpc);
      CaptureClock clock{perf::Stage::Now() - int64_t(qpc.QuadPart / frequency.QuadPart * 1000000000LL
          + qpc.QuadPart % frequency.QuadPart * 1000000000LL / frequency.QuadPart)};
      CaptureClock media_clock{webrtc::TimeNanos() - int64_t(qpc.QuadPart / frequency.QuadPart * 1000000000LL
          + qpc.QuadPart % frequency.QuadPart * 1000000000LL / frequency.QuadPart)};
      const char* regular_option = std::getenv("HARMONY_WGC_REGULAR_TIMESTAMPS");
      const bool regular_timestamps = regular_option && std::strcmp(regular_option, "1") == 0;
      RegularCaptureClock regular_clock(fps_);
      logf("screen capture regular_timestamps=%d fps=%d", int(regular_timestamps), fps_);
      std::unique_ptr<SyncReadback> sync_readback;
      if(sync_probe::Enabled()) sync_readback=std::make_unique<SyncReadback>();
      int64_t next_ns = 0;
      int64_t last_source_ns = 0, last_output_ns = 0;
      int64_t last_frame_ns=perf::Stage::Now();
      Direct3D11CaptureFramePool monitor_pool{nullptr};GraphicsCaptureSession monitor_session{nullptr};
      Direct3D11CaptureFramePool::FrameArrived_revoker monitor_on_frame;
      winrt::Windows::Graphics::SizeInt32 monitor_size{};HMONITOR fallback_monitor=nullptr;
      auto window_health=std::make_unique<CaptureBlackProbe>();
      while (!stop_) {
        const bool lost=source_closed||(window&&!IsWindow(window));
        const bool iconic=window&&IsIconic(window);
        if(lost||iconic) {
          if(monitor_session){monitor_on_frame.revoke();monitor_session.Close();monitor_pool.Close();monitor_session=nullptr;monitor_pool=nullptr;}
          if(window&&follow.Poll(lost,GetTickCount64())) {
            if(auto successor=media::FollowWindow(original_exe,window)) {
              // Open the replacement before retiring the old session. Failure
              // keeps the final picture and the original search deadline.
              try {
                GraphicsCaptureItem next{nullptr};
                Check(interop->CreateForWindow(successor,guid_of<GraphicsCaptureItem>(),put_abi(next)),"WGC follow window");
                auto next_size=next.Size();
                auto next_pool=Direct3D11CaptureFramePool::CreateFreeThreaded(device,DirectXPixelFormat::B8G8R8A8UIntNormalized,3,next_size);
                auto next_session=next_pool.CreateCaptureSession(next);
                next_session.IsCursorCaptureEnabled(true);
                try{next_session.IsBorderRequired(false);next_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(1)});}catch(...){}
                auto next_arrived=next_pool.FrameArrived(auto_revoke,[arrived](auto&&,auto&&){SetEvent(arrived);});
                next_session.StartCapture();
                closed.revoke();on_frame.revoke();session.Close();pool.Close();
                item=next;pool=next_pool;session=next_session;size=next_size;on_frame=std::move(next_arrived);
                source_closed=false;
                closed=item.Closed(auto_revoke,[&source_closed](auto&&,auto&&){source_closed=true;});
                auto name=[](const std::wstring& path){
                  auto at=path.find_last_of(L"\\/");auto base=path.substr(at==std::wstring::npos?0:at+1);
                  auto count=WideCharToMultiByte(CP_UTF8,0,base.data(),int(base.size()),nullptr,0,nullptr,nullptr);
                  if(count<=0)return std::string("não encontrado");
                  std::string text(count,'\0');WideCharToMultiByte(CP_UTF8,0,base.data(),int(base.size()),text.data(),count,nullptr,nullptr);return text;
                };
                auto successor_exe=media::FollowExecutable(successor);
                auto from=name(current_exe),to=name(successor_exe);
                logf("screen follow from=%s to=%s after_ms=%llu",from.c_str(),to.c_str(),GetTickCount64()-follow.since);
                current_exe=std::move(successor_exe);
                window=successor;window_=successor;
                next_ns=0;last_source_ns=0;window_health=std::make_unique<CaptureBlackProbe>();follow={};
                continue;
              } catch(...) {logf("screen follow candidate unavailable");}
            }
          }
          Repeat(lost?HoldReason::Closed:HoldReason::Minimized);
          WaitForSingleObject(arrived,100);continue;
        }
        operation="TryGetNextFrame";
        auto captured = pool.TryGetNextFrame();
        if(!ready_ && GetTickCount64()>=first_deadline) throw std::runtime_error("first_frame timeout deadline_ms=3000");
        HMONITOR selected=nullptr;RECT crop{};
        bool fullscreen=FullscreenWindow(window,&selected,&crop),black=false;
        if(fullscreen&&capture_fault==1&&captured){captured.Close();captured=nullptr;}
        if(captured&&fullscreen) {
          auto access=captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
          ComPtr<ID3D11Texture2D> texture;Check(access->GetInterface(IID_PPV_ARGS(&texture)),"WGC health surface");
          if(capture_fault==2) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);texture=Texture(desc.Width,desc.Height,DXGI_FORMAT_B8G8R8A8_UNORM);
            std::lock_guard lock(ContextMutex());ComPtr<ID3D11RenderTargetView> target;
            Check(Device()->CreateRenderTargetView(texture.Get(),nullptr,&target),"capture selftest black target");const float color[]{0,0,0,1};Context()->ClearRenderTargetView(target.Get(),color);
          }
          black=window_health->Poll(texture.Get(),webrtc::TimeMicros());
        }
        const bool fallback=fullscreen&&(black||(!captured&&(monitor_session||(last_image_&&webrtc::TimeMicros()-hold_.source_us>=CaptureHold::kIntervalUs))));
        if(!fallback&&monitor_session) {
          monitor_on_frame.revoke();monitor_session.Close();monitor_pool.Close();monitor_session=nullptr;monitor_pool=nullptr;
          logf("screen capture fallback=window");
        }
        if(fallback) {
          if(captured){captured.Close();captured=nullptr;}
          if(!monitor_session||fallback_monitor!=selected) {
            if(monitor_session){monitor_on_frame.revoke();monitor_session.Close();monitor_pool.Close();}
            GraphicsCaptureItem monitor_item{nullptr};Check(interop->CreateForMonitor(selected,guid_of<GraphicsCaptureItem>(),put_abi(monitor_item)),"WGC exclusive monitor");
            monitor_size=monitor_item.Size();fallback_monitor=selected;
            monitor_pool=Direct3D11CaptureFramePool::CreateFreeThreaded(device,DirectXPixelFormat::B8G8R8A8UIntNormalized,3,monitor_size);
            monitor_on_frame=monitor_pool.FrameArrived(auto_revoke,[arrived](auto&&,auto&&){SetEvent(arrived);});
            monitor_session=monitor_pool.CreateCaptureSession(monitor_item);
            try{monitor_session.IsBorderRequired(false);monitor_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(1)});}catch(...){}
            monitor_session.StartCapture();logf("screen capture fallback=monitor_crop reason=%s",black?"black":"stalled");
          }
          captured=monitor_pool.TryGetNextFrame();
        }
        if (!captured) {
          if(monitor && perf::Stage::Now()-last_frame_ns>2000000000LL) throw std::runtime_error("WGC monitor stalled; try Desktop Duplication");
          Repeat(HoldReason::Stalled);
          WaitForSingleObject(arrived, 100); continue;
        }
        last_frame_ns=perf::Stage::Now();
        static perf::Stage& stageCapture = perf::Get("capture");
        auto timer = stageCapture.Begin();
        int64_t dequeued = perf::Stage::Now();
        const int64_t source_ns = captured.SystemRelativeTime().count() * 100;
        int64_t now = clock.Timestamp(source_ns);
        const int64_t capture_us = media_clock.Timestamp(source_ns) / 1000;
        static perf::Stage& stageCaptureQueueAge = perf::Get("capture.queue_age");
        stageCaptureQueueAge.Record(std::max<int64_t>(0, dequeued - now));
        static perf::Stage& stageCaptureSourceInterval = perf::Get("capture.source_interval");
        if(last_source_ns) stageCaptureSourceInterval.Record(now-last_source_ns);
        last_source_ns=now;
        auto content = captured.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) { timer.Drop(); captured.Close(); continue; }
        auto& active_size=fallback?monitor_size:size;
        if (content.Width != active_size.Width || content.Height != active_size.Height) {
          // The first frame after resize can still have the old pool texture.
          // Recreate before copying, otherwise the source box may exceed it.
          timer.Drop(); captured.Close(); active_size = content;
          step("Recreate frame pool");
          (fallback?monitor_pool:pool).Recreate(device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, active_size);
          continue;
        }
        int source_x=0,source_y=0;
        if(fallback) {
          // Focus is checked again before publishing monitor pixels. Alt+Tab
          // must never turn a window share into an unrelated desktop share.
          HMONITOR current=nullptr;RECT current_crop{};
          if(!FullscreenWindow(window,&current,&current_crop)||current!=fallback_monitor){captured.Close();Repeat(HoldReason::Stalled);continue;}
          MONITORINFO info{sizeof info};GetMonitorInfoW(current,&info);
          source_x=std::clamp<LONG>(current_crop.left-info.rcMonitor.left,0,content.Width);
          source_y=std::clamp<LONG>(current_crop.top-info.rcMonitor.top,0,content.Height);
          content.Width=std::clamp<LONG>(current_crop.right-info.rcMonitor.left,0,content.Width)-source_x;
          content.Height=std::clamp<LONG>(current_crop.bottom-info.rcMonitor.top,0,content.Height)-source_y;
          if(content.Width<=0||content.Height<=0){captured.Close();continue;}
        }
        // The rate cap tolerates half a frame of jitter. A 60 Hz window into a
        // 60 fps cap arrives every 16.7 ms give or take a few, and a tolerance
        // of 1 ms dropped every early one. The schedule never builds up debt:
        // after a gap it restarts from now.
        if (!CaptureDue(now / 1000, fps_, next_ns)) { timer.Drop(); captured.Close(); continue; }
        int ow, oh, cw, ch, cx, cy;
        if (!PrepareFrame(content.Width, content.Height, now / 1000, &ow, &oh, &cw, &ch, &cx, &cy)) {
          // Opening can precede the first sink. A static window may supply
          // only this one frame: retain it now so attaching a track later
          // cannot leave the new source live but permanently pictureless.
          if(!last_image_) {
            auto access=captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;Check(access->GetInterface(IID_PPV_ARGS(&texture)),"WGC first retained surface");
            last_image_=Texture(content.Width,content.Height,DXGI_FORMAT_B8G8R8A8_UNORM);
            std::lock_guard lock(ContextMutex());
            D3D11_BOX box{UINT(source_x),UINT(source_y),0,UINT(source_x+content.Width),UINT(source_y+content.Height),1};
            Context()->CopySubresourceRegion(last_image_.Get(),0,0,0,0,texture.Get(),0,&box);
            hold_.Fresh(webrtc::TimeMicros());
          }
          timer.Drop(); captured.Close(); continue;
        }
        double cap=std::min({1.0,double(height_)/oh,double(height_*16/9)/ow});
        ow=std::max(2,int(ow*cap)/2*2); oh=std::max(2,int(oh*cap)/2*2);
        auto access = captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        Check(access->GetInterface(IID_PPV_ARGS(&texture)), "WGC surface");
        // Detach on the GPU: capture may recycle its pool as soon as Close runs.
        auto owned = Texture(content.Width, content.Height, DXGI_FORMAT_B8G8R8A8_UNORM);
        {
          std::lock_guard lock(ContextMutex());
          D3D11_BOX box{UINT(source_x),UINT(source_y),0,UINT(source_x+content.Width),UINT(source_y+content.Height),1};
          Context()->CopySubresourceRegion(owned.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
        }
        captured.Close();
        last_image_=owned;notice_image_.Reset();hold_.Fresh(webrtc::TimeMicros());
        auto buffer = webrtc::make_ref_counted<D3DFrameBuffer>(owned.Get(), 0, now);
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> adapted = buffer;
        if (ow != content.Width || oh != content.Height || cw != content.Width || ch != content.Height)
          adapted = buffer->CropAndScale(cx, cy, cw, ch, ow, oh);
        timer.Done();
        static perf::Stage& stageCaptureOutputInterval = perf::Get("capture.output_interval");
        if(last_output_ns) stageCaptureOutputInterval.Record(now-last_output_ns);
        last_output_ns=now;
        const int64_t stamped_us = hold_.Stamp(regular_timestamps ? regular_clock.Timestamp(capture_us) : capture_us);
        static perf::Stage& stageCaptureTimestampAdjustment = perf::Get("capture.timestamp_adjustment");
        stageCaptureTimestampAdjustment.Record(std::llabs(stamped_us - capture_us) * 1000);
        static perf::Stage& stageCaptureTimestampCrowded = perf::Get("capture.timestamp_crowded");
        stageCaptureTimestampCrowded.SetValue(regular_clock.crowded());
        if(sync_readback) sync_readback->Frame(owned.Get(), content.Width, content.Height,
            stamped_us, capture_us, static_cast<uint32_t>(stamped_us * 90 / 1000));
        OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(adapted)
            .set_timestamp_us(stamped_us).set_rtp_timestamp(static_cast<uint32_t>(stamped_us * 90 / 1000))
            .set_rotation(webrtc::kVideoRotation_0).build());
      }
      if(monitor_session){monitor_on_frame.revoke();monitor_session.Close();monitor_pool.Close();}
      on_frame.revoke(); closed.revoke(); session.Close(); pool.Close();
    } catch (const winrt::hresult_error& error) {
      logf("screen open failed step=%s HRESULT=0x%08lx", operation, static_cast<unsigned long>(error.code().value));
    } catch (const std::exception& error) { logf("screen open failed step=%s reason=%s",operation,error.what()); }
    if(monitor&&!stop_) Duplicate(monitor);
    // Once a valid image existed, acquisition failure is a hold state. Keep
    // the same live track until the owner explicitly stops or replaces it.
    while(!stop_&&last_image_) {Repeat(window&&!IsWindow(window)?HoldReason::Closed:HoldReason::Stalled);Sleep(100);}
    ended_ = true;
#endif
  }
#ifndef HARMONY_CAPTURE_TEST
  void Duplicate(HMONITOR monitor) {
    try {
      ComPtr<IDXGIDevice> dxgi; Check(Device()->QueryInterface(IID_PPV_ARGS(&dxgi)),"DXGI duplication device");
      ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter),"DXGI duplication adapter");
      ComPtr<IDXGIOutput1> chosen;DXGI_MODE_ROTATION rotation=DXGI_MODE_ROTATION_IDENTITY;
      for(UINT i=0;;++i) {
        ComPtr<IDXGIOutput> output; if(adapter->EnumOutputs(i,&output)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);if(desc.Monitor==monitor) {output.As(&chosen);rotation=desc.Rotation;break;}
      }
      if(!chosen) throw std::runtime_error("Desktop Duplication monitor on other adapter");
      ComPtr<IDXGIOutputDuplication> duplication; Check(chosen->DuplicateOutput(Device(),&duplication),"DuplicateOutput");
      logf("screen capture fallback=DesktopDuplication; monitor chosen explicitly");
      struct PollWait {
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        ~PollWait() {if(timer)CloseHandle(timer);}
        void Wait() {
          static perf::Stage& stageCaptureIdleWait = perf::Get("capture.idle_wait");
          auto measured=stageCaptureIdleWait.Begin();
          LARGE_INTEGER delay{};delay.QuadPart=-10000; // one millisecond
          if(timer&&SetWaitableTimer(timer,&delay,0,nullptr,nullptr,FALSE))
            WaitForSingleObject(timer,100);
          else Sleep(1);
          measured.Done();
        }
      } idle;
      int64_t next=0,interval=1000000000LL/fps_;
      while(!stop_) {
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
        // A blocking DXGI wait can retain the shared D3D device inside the
        // driver, starving NVENC and the video processor on other threads.
        // Poll without holding that device and wait on the CPU instead.
        auto hr=duplication->AcquireNextFrame(0,&info,&resource);
        // Sleep(1) alone may use a coarse timer in this process, losing
        // frames at 60 fps even if the game requested fine timers for itself.
        if(hr==DXGI_ERROR_WAIT_TIMEOUT) {Repeat(HoldReason::Stalled);idle.Wait();continue;}
        Check(hr,"AcquireNextFrame");
        try {
          int64_t now=perf::Stage::Now(); ComPtr<ID3D11Texture2D> texture;Check(resource.As(&texture),"Desktop Duplication texture");
          D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
          if(!next||now>=next-interval/2) {
            static perf::Stage& stageCapture = perf::Get("capture");
            auto timer=stageCapture.Begin();
            bool portrait=rotation==DXGI_MODE_ROTATION_ROTATE90||rotation==DXGI_MODE_ROTATION_ROTATE270;
            int width=portrait?desc.Height:desc.Width,height=portrait?desc.Width:desc.Height;
            int ow,oh,cw,ch,cx,cy;
            if(!PrepareFrame(width,height,now/1000,&ow,&oh,&cw,&ch,&cx,&cy)) {duplication->ReleaseFrame();continue;}
            auto owned=Texture(width,height,DXGI_FORMAT_B8G8R8A8_UNORM);
            auto raw=webrtc::make_ref_counted<D3DFrameBuffer>(texture.Get(),0,now);
            Blit(*raw,owned.Get(),RECT{0,0,LONG(desc.Width),LONG(desc.Height)},RECT{0,0,width,height},rotation>DXGI_MODE_ROTATION_IDENTITY?int(rotation)-1:0,false);
            desc.Width=width;desc.Height=height;
            double scale=std::min({1.0,double(height_)/desc.Height,double(height_*16/9)/desc.Width});
            auto buffer=webrtc::make_ref_counted<D3DFrameBuffer>(owned.Get(),0,now);
            auto resized=buffer->CropAndScale(0,0,desc.Width,desc.Height,std::max(2,int(desc.Width*scale)/2*2),std::max(2,int(desc.Height*scale)/2*2));
            last_image_=owned;notice_image_.Reset();hold_.Fresh(webrtc::TimeMicros());
            const auto stamp=hold_.Stamp(webrtc::TimeMicros());
            ready_=true;OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(resized).set_timestamp_us(stamp).set_rtp_timestamp(static_cast<uint32_t>(stamp*90/1000)).set_rotation(webrtc::kVideoRotation_0).build());
            next=(next&&next>now-interval?next:now)+interval;timer.Done();
          }
        } catch(...) {duplication->ReleaseFrame();throw;}
        duplication->ReleaseFrame();
      }
    } catch(std::exception const&) {logf("screen Desktop Duplication unavailable; use borderless window");}
  }
 #endif
  int fps_,height_;
  std::atomic<int> capture_fps_;
  int paced_fps_=0;
  int64_t next_frame_us_=0;
  std::atomic<bool> stop_{false}, ended_{false}, ready_{false};
  std::atomic<HWND> window_{nullptr};
  std::thread worker_;
#ifndef HARMONY_CAPTURE_TEST
  std::wstring original_exe_;
  CaptureHold hold_;
  ComPtr<ID3D11Texture2D> last_image_,notice_image_;
#endif
};
}
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateWindowSource(HWND window, int max_fps,int max_height) {
  return webrtc::make_ref_counted<CaptureSource>(window, nullptr, max_fps,max_height);
}
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateMonitorSource(HMONITOR monitor, int max_fps,int max_height) {
  return webrtc::make_ref_counted<CaptureSource>(nullptr, monitor, max_fps,max_height);
}
HWND CaptureWindow(webrtc::VideoTrackSourceInterface* source) {return source?static_cast<CaptureSource*>(source)->window():nullptr;}
void ConfigureCaptureSelftest(bool desktop_duplication) {force_duplication=desktop_duplication;}
void ConfigureCaptureFaultSelftest(int mode) {capture_fault=mode;}
}  // namespace gpu

