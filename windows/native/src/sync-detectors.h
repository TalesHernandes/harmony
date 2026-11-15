// Bench-only content detectors; no clocks, devices or allocations.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace sync_probe {
class FlashDetector {
 public:
  bool Push(const uint8_t* bgra, size_t pitch) {
    bool white = true;
    for (size_t y=0;y<2;++y) for(size_t x=0;x<2;++x) {
      const auto* p=bgra+y*pitch+x*4;
      white = white && p[0]>220 && p[1]>220 && p[2]>220;
    }
    const bool rise = known_ && !on_ && white;
    known_=true; on_=white; return rise;
  }
  void Gap() { known_=false; }
 private:
  bool known_=false, on_=false;
};
class BeepDetector {
 public:
  // RMS uses 1 ms windows, not a whole 10 ms block: retain sample position.
  // Within the first loud window, locate the first sample above 0.015.
  std::optional<size_t> Push(const int16_t* pcm, size_t frames, size_t channels, size_t rate) {
    std::optional<size_t> onset;
    const size_t window=rate/1000;
    if(!channels || !window) return onset;
    for(size_t first=0;first<frames;first+=window) {
      const size_t end=first+window<frames?first+window:frames;
      double power=0;
      for(size_t i=first;i<end;++i) for(size_t c=0;c<channels;++c) {
        const double x=pcm[i*channels+c]/32768.0;power+=x*x;
      }
      const bool on=std::sqrt(power/((end-first)*channels))>0.015;
      if(known_ && on && !on_ && !onset) {
        for(size_t i=first;i<end && !onset;++i) for(size_t c=0;c<channels;++c)
          if(std::abs(pcm[i*channels+c]/32768.0)>0.015) {onset=i;break;}
      }
      known_=true;on_=on;
    }
    return onset;
  }
  void Gap() { known_=false; }
 private:
  bool known_=false, on_=false;
};
} // namespace sync_probe
