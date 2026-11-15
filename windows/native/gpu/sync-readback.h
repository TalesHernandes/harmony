// Only constructed with HARMONY_SYNC_PROBE=1. Never wait for the GPU.
#pragma once
#include "internal.h"
#include "rtc_base/time_utils.h"
#include "../src/sync-detectors.h"
#include "../src/sync-probe.h"
#include <array>
namespace gpu {
class SyncReadback {
 public:
  void Frame(ID3D11Texture2D* source, int width, int height,
             int64_t capture_us, int64_t raw_us, uint32_t rtp) {
    Poll();
    auto& slot=slots_[write_];
    if(slot.pending) { Gap("readback_full");return; }
    // Same normalized point as autoteste-tela.mjs; inside the game's 80px band.
    const UINT x=UINT(width/2), y=UINT(height*0.065);
    if(x+2>UINT(width)||y+2>UINT(height)) {Gap("invalid_region");return;}
    std::lock_guard lock(ContextMutex());
    if(!slot.texture) {
      D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=2;
      desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
      if(FAILED(Device()->CreateTexture2D(&desc,nullptr,&slot.texture))) {Gap("staging_create_failed");return;}
    }
    slot.event=sync_probe::Event("flash");
    slot.event["media_now_ms"]=webrtc::TimeMicros()/1000.0;
    slot.event["capture_us"]=capture_us;slot.event["raw_capture_us"]=raw_us;
    slot.event["rtp_timestamp"]=int64_t(rtp);
    slot.event["frame_sequence"]=++sequence_;
    slot.event["probe_x"]=int(x);slot.event["probe_y"]=int(y);
    D3D11_BOX box{x,y,0,x+2,y+2,1};
    Context()->CopySubresourceRegion(slot.texture.Get(),0,0,0,0,source,0,&box);
    slot.pending=true;slot.generation=generation_;write_=(write_+1)%slots_.size();
  }
  void Poll() {
    std::lock_guard lock(ContextMutex());
    for(size_t i=0;i<slots_.size();++i) {
      auto& slot=slots_[read_];if(!slot.pending)break;
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const auto hr=Context()->Map(slot.texture.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
      if(hr==DXGI_ERROR_WAS_STILL_DRAWING)break;
      if(FAILED(hr)) Gap("staging_map_failed");
      else {
        if(slot.generation!=last_generation_) {detector_.Gap();last_generation_=slot.generation;}
        const bool rise=detector_.Push(static_cast<const uint8_t*>(mapped.pData),mapped.RowPitch);
        Context()->Unmap(slot.texture.Get(),0);
        if(rise) {slot.event["readback_wall_ms"]=sync_probe::WallMs();sync_probe::Record(std::move(slot.event));}
      }
      slot.pending=false;read_=(read_+1)%slots_.size();
    }
  }
  ~SyncReadback() {
    Poll();for(auto& s:slots_)if(s.pending)Gap("readback_pending_at_stop");
  }
 private:
  void Gap(const char* reason) {
    ++generation_;auto e=sync_probe::Event("video_probe_gap");e["reason"]=reason;sync_probe::Record(std::move(e));
  }
  struct Slot {ComPtr<ID3D11Texture2D> texture;json::Value event;bool pending=false;uint64_t generation=0;};
  std::array<Slot,4> slots_;
  size_t read_=0,write_=0;int64_t sequence_=0;
  uint64_t generation_=0,last_generation_=0;
  sync_probe::FlashDetector detector_;
};
} // namespace gpu
