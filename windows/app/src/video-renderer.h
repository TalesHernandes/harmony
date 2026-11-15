#pragma once
#include "../../native/gpu/gpu.h"
#include <mutex>
namespace app {
class VideoRenderer final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit VideoRenderer(HWND target) : presenter_(gpu::Presenter::Create(target)) {}
  ~VideoRenderer() override { Attach(nullptr); }
  void Attach(webrtc::VideoTrackInterface* track) {
    if(track_) track_->RemoveSink(this); track_=track;
    if(track_) {webrtc::VideoSinkWants wants;track_->AddOrUpdateSink(this,wants);}
  }
  void OnFrame(const webrtc::VideoFrame& frame) override { if(presenter_)presenter_->OnFrame(frame); }
 private:
  std::unique_ptr<gpu::Presenter> presenter_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> track_;
};
}
