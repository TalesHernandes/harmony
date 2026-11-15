#include "media-preview.h"
#include "../../native/gpu/gpu.h"
#include "../../native/src/perf.h"
#include "../../native/src/log.h"
#include "libyuv.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
namespace app {
PreviewImage Preview(ScreenChoice choice) {
  if(choice.minimized) return {};
  static std::atomic<unsigned> sequence{0}; auto request=++sequence;
  logf("picker preview begin request=%u pid=%u monitor=%d",request,choice.pid,bool(choice.monitor));
  auto timer=perf::Get("picker.thumbnail").Begin();
  struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::mutex mu; std::condition_variable cv; PreviewImage image;
    void OnFrame(const webrtc::VideoFrame& frame) override {
      std::lock_guard lock(mu); if(!image.pixels.empty()) return;
      auto buffer=frame.video_frame_buffer()->ToI420(); if(!buffer) return;
      image.width=buffer->width(); image.height=buffer->height(); image.pixels.resize(size_t(image.width)*image.height*4);
      libyuv::I420ToARGB(buffer->DataY(),buffer->StrideY(),buffer->DataU(),buffer->StrideU(),buffer->DataV(),buffer->StrideV(),image.pixels.data(),image.width*4,image.width,image.height); cv.notify_all();
    }
  } sink;
  // 360 lines: the chooser's blocks are about 440 points wide.
  auto source=choice.window?gpu::CreateWindowSource(choice.window,1,360):gpu::CreateMonitorSource(choice.monitor,1,360);
  webrtc::VideoSinkWants wants; wants.max_pixel_count=640*360; source->AddOrUpdateSink(&sink,wants);
  {std::unique_lock lock(sink.mu);sink.cv.wait_for(lock,std::chrono::milliseconds(600),[&]{return !sink.image.pixels.empty();});}
  source->RemoveSink(&sink); source=nullptr;
  logf("picker preview end request=%u pixels=%d",request,!sink.image.pixels.empty());
  if(sink.image.pixels.empty()) timer.Drop(); else timer.Done(); return std::move(sink.image);
}
PreviewImage WindowIcon(HWND window) {
  DWORD_PTR value=0; SendMessageTimeoutW(window,WM_GETICON,ICON_SMALL2,0,SMTO_ABORTIFHUNG,50,&value);
  HICON icon=reinterpret_cast<HICON>(value); if(!icon) icon=reinterpret_cast<HICON>(GetClassLongPtrW(window,GCLP_HICONSM));
  if(!icon) return {};
  BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=32; info.bmiHeader.biHeight=-32; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
  void* bits=nullptr; HDC dc=CreateCompatibleDC(nullptr); HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
  if(!bitmap) {DeleteDC(dc);return {};}
  auto old=SelectObject(dc,bitmap); memset(bits,0,32*32*4); DrawIconEx(dc,0,0,icon,32,32,0,nullptr,DI_NORMAL);
  PreviewImage image{32,32,{}}; image.pixels.assign(static_cast<uint8_t*>(bits),static_cast<uint8_t*>(bits)+32*32*4);
  SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); return image;
}
}
