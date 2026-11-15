#pragma once
#include "api/audio/audio_mixer.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "receive-gain.h"
#include <functional>
#include <mutex>
#include <memory>
#include <set>
namespace audio {
// Keep libwebrtc's mixer, limiter and reverse transport. Only scale each
// receive source after decoding/raw meters and before that mixer.
class ReceiveMixer : public webrtc::AudioMixer {
  class Input : public Source {
   public:
    Input(Source* original,ReceiveMixer* owner):original_(original),owner_(owner) {}
    AudioFrameInfo GetAudioFrameWithInfo(int rate,webrtc::AudioFrame* frame) override {
      auto result=original_->GetAudioFrameWithInfo(rate,frame);
      if(result!=AudioFrameInfo::kNormal) return result;
      auto gain=owner_->Gain(uint32_t(Ssrc()));
      if(gain==0) { frame->Mute(); return AudioFrameInfo::kMuted; }
      ScaleReceivePcm(frame->mutable_data(),frame->samples_per_channel_*frame->num_channels_,gain);
      return result;
    }
    int Ssrc() const override { return original_->Ssrc(); }
    int PreferredSampleRate() const override { return original_->PreferredSampleRate(); }
   private: Source* original_; ReceiveMixer* owner_;
  };
 public:
  ReceiveMixer():inner_(webrtc::AudioMixerImpl::Create()) {}
  bool AddSource(Source* source) override {
    std::lock_guard lock(inputs_mu_); if(inputs_.contains(source)) return false;
    auto input=std::make_unique<Input>(source,this); if(!inner_->AddSource(input.get())) return false;
    inputs_.emplace(source,std::move(input)); return true;
  }
  void RemoveSource(Source* source) override {
    std::lock_guard lock(inputs_mu_); auto input=inputs_.find(source); if(input==inputs_.end()) return;
    inner_->RemoveSource(input->second.get()); inputs_.erase(input);
  }
  void Mix(size_t channels,webrtc::AudioFrame* frame) override { inner_->Mix(channels,frame); }
  // `fallback` is what a stream the routes do not name yet plays at: the
  // ordinary volume, or 0 while deafened. It used to be always 0, and a
  // friend whose new SSRC missed the last route update stayed silent until
  // the room changed again (22/10/2026). `unknown` is told once per such SSRC
  // so the session can rebuild the routes.
  void Gains(std::map<uint32_t,double> values,double fallback) {
    std::lock_guard lock(gains_mu_); gains_=std::move(values); fallback_=fallback;
  }
  void OnUnknown(std::function<void(uint32_t)> unknown) { std::lock_guard lock(gains_mu_); unknown_=std::move(unknown); }
  double Gain(uint32_t ssrc) {
    std::function<void(uint32_t)> report; double gain;
    {
      std::lock_guard lock(gains_mu_); auto it=gains_.find(ssrc); if(it!=gains_.end()) return it->second;
      gain=fallback_; if(reported_.insert(ssrc).second) report=unknown_;
    }
    if(report) report(ssrc);
    return gain;
  }
  ~ReceiveMixer() override = default;
 private:
  std::mutex inputs_mu_,gains_mu_;
  std::map<uint32_t,double> gains_;
  double fallback_=0;
  std::set<uint32_t> reported_;
  std::function<void(uint32_t)> unknown_;
  std::map<Source*,std::unique_ptr<Input>> inputs_;
  webrtc::scoped_refptr<webrtc::AudioMixerImpl> inner_;
};
}
