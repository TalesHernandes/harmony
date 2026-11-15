#pragma once
#include <algorithm>
#include <vector>
#include "api/video/builtin_video_bitrate_allocator_factory.h"

namespace media {
// Local RTP priority (high), not DSCP. Camera keeps the default weight of 1.
inline constexpr double kScreenBitratePriority = 4.0;

// Reserve a useful floor for the largest active picture before giving surplus
// to smaller pictures. Keep the built-in allocation above that floor.
// H.264 simulcast is the screen in this client; camera is single-stream VP8.
class ScreenBitrateAllocator final : public webrtc::VideoBitrateAllocator {
 public:
  ScreenBitrateAllocator(const webrtc::VideoCodec& codec,
                         std::unique_ptr<webrtc::VideoBitrateAllocator> base)
      : codec_(codec), base_(std::move(base)) {}
  webrtc::VideoBitrateAllocation Allocate(webrtc::VideoBitrateAllocationParameters p) override {
    auto rates = base_->Allocate(p);
    // The built-in allocator may round a tiny/zero rate up to a layer minimum.
    // A minimum is not permission to transmit more than the transport granted.
    if (rates.get_sum_bps() > p.total_bitrate.bps()) return {};
    int high = -1;
    for (int i = 0; i < codec_.numberOfSimulcastStreams; ++i) {
      const auto& s = codec_.simulcastStream[i];
      if (!s.active) continue;
      if (high < 0 || s.width * s.height > codec_.simulcastStream[high].width * codec_.simulcastStream[high].height) high = i;
    }
    if (high < 0) return rates;
    const uint32_t floor = codec_.simulcastStream[high].minBitrate * 1000;
    const uint32_t budget = std::min<uint64_t>(p.total_bitrate.bps(), rates.get_sum_bps());
    const uint32_t current = rates.GetSpatialLayerSum(high);
    // 600 kbit/s is an allocation policy, not a guaranteed resolution/frame rate.
    const uint32_t useful = std::min<uint32_t>(codec_.simulcastStream[high].maxBitrate * 1000,
                                              std::max<uint32_t>(floor, 600000));
    if (current >= useful || budget < floor) return rates;
    // Independent H.264 pictures have one temporal layer. Do not rewrite SVC.
    for (int i = 0; i < codec_.numberOfSimulcastStreams; ++i)
      if (codec_.simulcastStream[i].GetNumberOfTemporalLayers() != 1) return rates;
    webrtc::VideoBitrateAllocation reserved;
    reserved.SetBitrate(high, 0, floor);
    uint32_t remaining = budget - floor;
    std::vector<int> lower;
    for (int i = 0; i < codec_.numberOfSimulcastStreams; ++i)
      if (i != high && codec_.simulcastStream[i].active) lower.push_back(i);
    std::sort(lower.begin(), lower.end(), [&](int a, int b) {
      return codec_.simulcastStream[a].width * codec_.simulcastStream[a].height <
             codec_.simulcastStream[b].width * codec_.simulcastStream[b].height;
    });
    // Keep the lower pictures available too whenever the budget covers them.
    for (int i : lower) {
      const uint32_t keep = codec_.simulcastStream[i].minBitrate * 1000;
      if (remaining < keep) continue;
      reserved.SetBitrate(i, 0, keep); remaining -= keep;
    }
    const uint32_t highExtra = std::min(remaining, useful - floor);
    reserved.SetBitrate(high, 0, floor + highExtra); remaining -= highExtra;
    for (int i : lower) {
      const uint32_t have = reserved.GetSpatialLayerSum(i);
      if (!have) continue;
      const uint32_t desired = rates.GetSpatialLayerSum(i);
      const uint32_t extra = std::min(remaining, desired > have ? desired - have : 0);
      reserved.SetBitrate(i, 0, have + extra); remaining -= extra;
    }
    reserved.SetBitrate(high, 0, floor + highExtra + std::min(remaining, codec_.simulcastStream[high].maxBitrate * 1000 - floor - highExtra));
    return reserved;
  }
  void SetLegacyConferenceMode(bool enabled) override { base_->SetLegacyConferenceMode(enabled); }
 private:
  webrtc::VideoCodec codec_;
  std::unique_ptr<webrtc::VideoBitrateAllocator> base_;
};

class ScreenBitrateAllocatorFactory final : public webrtc::VideoBitrateAllocatorFactory {
 public:
  std::unique_ptr<webrtc::VideoBitrateAllocator> Create(const webrtc::Environment& env,
                                                      const webrtc::VideoCodec& codec) override {
    auto base = webrtc::CreateBuiltinVideoBitrateAllocatorFactory()->Create(env, codec);
    if (codec.codecType == webrtc::kVideoCodecH264 && codec.numberOfSimulcastStreams > 1)
      return std::make_unique<ScreenBitrateAllocator>(codec, std::move(base));
    return base;
  }
};
}
