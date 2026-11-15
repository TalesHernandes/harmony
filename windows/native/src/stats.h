// What the engine says it did, once per second: per layer sent (size, rate,
// kbit/s, encoder, what limits it) and per picture received (decoder, rate,
// freezes, how long it waited). The panel and the bench read Text(); the
// differences are computed here, by the only reader, from the previous report.
#pragma once
#include <map>
#include <mutex>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "json.h"

namespace stats {

class Poller : public webrtc::RTCStatsCollectorCallback {
 public:
  // Asks for a report; the answer arrives on libwebrtc's signalling thread.
  void Request(webrtc::PeerConnectionInterface* pc);
  // The last complete summary (empty until the second report).
  std::string Text() const;
  // The last report's raw counters, in the shape the bench reads from a
  // browser's getStats (scripts/bancada/bancada.mjs, rtc()), so it computes
  // the native program's rates exactly as it computes the Edge's.
  json::Value Counters() const;
  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override;

 private:
  struct Prev {
    double t = 0;
    uint64_t bytes = 0;
    uint32_t frames = 0;
    double encode_s = 0;
  };
  mutable std::mutex mu_;
  std::string text_;
  json::Value counters_;
  std::map<std::string, Prev> prev_;
};

}  // namespace stats
