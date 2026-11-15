#pragma once
#include <algorithm>
#include <cstdint>

namespace gpu {
enum class HoldReason { None, Minimized, Closed, Stalled };
// Keep source and output clocks separate: repeated pictures must not postpone
// stall detection, and resuming WGC can carry an older compositor timestamp.
struct CaptureHold {
  // Android's restart-deadline progress sample spans 350 ms. A 200-ms
  // heartbeat also advances that sample when video is the only inbound RTP.
  static constexpr int64_t kIntervalUs = 200000; // five pictures per second
  int64_t source_us=0,output_us=0;
  HoldReason notice=HoldReason::None;
  void Fresh(int64_t now) {source_us=now;notice=HoldReason::None;}
  bool Due(int64_t now,bool image,HoldReason reason) const {
    if(!image)return false;
    if(reason==HoldReason::None)reason=HoldReason::Stalled;
    if(reason==HoldReason::Stalled&&now-source_us<kIntervalUs)return false;
    return reason!=notice||now-output_us>=kIntervalUs;
  }
  int64_t Stamp(int64_t now) {output_us=std::max(now,output_us+1000);return output_us;}
};
}
