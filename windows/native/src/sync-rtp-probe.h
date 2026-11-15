// Opt-in metadata tap. Payload and timestamps are forwarded unchanged.
#pragma once
#include "sync-probe.h"
#include "api/frame_transformer_interface.h"
#include "api/make_ref_counted.h"
#include "api/rtp_sender_interface.h"
#include "rtc_base/time_utils.h"
#include <map>
#include <mutex>
namespace sync_probe {
class RtpProbe : public webrtc::FrameTransformerInterface {
 public:
  explicit RtpProbe(bool audio) : audio_(audio) {}
  void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> cb) override {
    std::lock_guard lock(mu_); callback_=std::move(cb);
  }
  void UnregisterTransformedFrameCallback() override {
    std::lock_guard lock(mu_); callback_=nullptr;
  }
  void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> cb, uint32_t ssrc) override {
    std::lock_guard lock(mu_); callbacks_[ssrc]=std::move(cb);
  }
  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
    std::lock_guard lock(mu_); callbacks_.erase(ssrc);
  }
  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> cb;
    {
      std::lock_guard lock(mu_);
      auto it=callbacks_.find(frame->GetSsrc());
      cb=it==callbacks_.end()?callback_:it->second;
    }
    if(!cb) return; // The sender was detached during teardown.
    auto e=Event(audio_?"audio_encoded":"video_encoded");
    e["media_now_ms"]=webrtc::TimeMicros()/1000.0;
    e["source"]=audio_?"screen_audio":"screen";
    e["ssrc"]=int64_t(frame->GetSsrc());
    auto info=frame->GetRtpTimestampInfo();
    const bool full=std::holds_alternative<webrtc::RtpTimestampWithOffset>(info);
    e["rtp_with_offset"]=full;
    e["rtp_timestamp"]=int64_t(std::visit([](auto t){return t.value;},info));
    const auto captured=frame->CaptureTime();
    e["capture_ms"]=captured?json::Value(captured->us()/1000.0):json::Value();
    const auto presentation=frame->GetPresentationTimestamp();
    e["presentation_us"]=presentation?json::Value(presentation->us()):json::Value();
    e["mime"]=frame->GetMimeType();
    if(!audio_) {
      auto* video=static_cast<webrtc::TransformableVideoFrameInterface*>(frame.get());
      e["rid"]=video->Rid().value_or("");
    } else {
      // TOC bytes describe Opus packet duration; no PCM or encoded payload is logged.
      auto data=frame->GetData();
      e["payload_size"]=int64_t(data.size());
      e["opus_toc"]=data.empty()?json::Value():json::Value(int(data[0]));
      e["opus_count_byte"]=data.size()<2?json::Value():json::Value(int(data[1]));
    }
    Record(std::move(e));
    cb->OnTransformedFrame(std::move(frame));
  }
 private:
  bool audio_;
  std::mutex mu_;
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;
  std::map<uint32_t,webrtc::scoped_refptr<webrtc::TransformedFrameCallback>> callbacks_;
};
inline void AttachRtpProbe(webrtc::RtpSenderInterface* sender, bool audio) {
  if(Enabled() && sender) sender->SetFrameTransformer(webrtc::make_ref_counted<RtpProbe>(audio));
}
} // namespace sync_probe
