#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace voice {
// The same RMS floor and 300 ms hold as VoiceState::Level. Observe every
// 25 ms; only crossings leave the media thread.
struct SpeakingLevel {
  int64_t loudAt=0;
  bool loudSeen=false, speaking=false, initialized=false;
  std::optional<bool> Observe(double amplitude,int64_t now,bool allowed=true) {
    if(!allowed) loudSeen=false;
    else if(std::isfinite(amplitude) && amplitude>0.001) {loudAt=now;loudSeen=true;}
    bool on=allowed && loudSeen && now-loudAt<300;
    bool changed=!initialized || on!=speaking;
    initialized=true;speaking=on;
    if(changed) return on;
    return {};
  }
};
}
