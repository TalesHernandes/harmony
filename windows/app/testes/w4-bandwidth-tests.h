#pragma once
#include "../../native/src/screen-bitrate.h"
#include "call/bitrate_allocator.h"
#include "video/config/simulcast.h"

static void RunBandwidthRegression(const webrtc::RtpParameters& screen,
                                   const webrtc::RtpParameters& camera) {
  struct Limits : webrtc::BitrateAllocator::LimitObserver {
    void OnAllocationLimitsChanged(webrtc::BitrateAllocationLimits) override {}
  } limits;
  struct Track : webrtc::BitrateAllocatorObserver {
    uint32_t rate = 0;
    uint32_t OnBitrateUpdated(webrtc::BitrateAllocationUpdate update) override {
      rate = update.target_bitrate.bps(); return 0;
    }
    std::optional<webrtc::DataRate> GetUsedRate() const override { return std::nullopt; }
  } screenTrack, cameraTrack;
  webrtc::BitrateAllocator allocation(&limits, webrtc::DataRate::Zero());
  allocation.AddObserver(&screenTrack, {30000, 19800000, 0, 0, false,
      screen.encodings.front().bitrate_priority, std::nullopt});
  allocation.AddObserver(&cameraTrack, {30000, uint32_t(camera.encodings.front().max_bitrate_bps.value_or(900000)),
      0, 0, false, camera.encodings.front().bitrate_priority, std::nullopt});
  auto env = webrtc::CreateEnvironment();
  std::array<webrtc::Resolution, 3> resolutions{{{476,268},{952,536},{1904,1072}}};
  auto streams = webrtc::GetSimulcastConfig(resolutions, false, false, env.field_trials(), webrtc::kVideoCodecH264);
  Check(streams.size() == 3, "bandwidth installed library provides three layer limits");
  for (size_t i = 0; i < streams.size(); ++i)
    std::cout << "Library layer " << i << " min=" << streams[i].min_bitrate_bps
              << " target=" << streams[i].target_bitrate_bps << " max=" << streams[i].max_bitrate_bps << '\n';
  for (int bwe : {1800000, 1200000, 2400000, 6000000, 300000, 0}) {
    webrtc::TargetTransferRate estimate;
    estimate.at_time = webrtc::Timestamp::Millis(1000);
    estimate.target_rate = webrtc::DataRate::BitsPerSec(bwe);
    estimate.network_estimate.at_time = estimate.at_time;
    estimate.network_estimate.round_trip_time = webrtc::TimeDelta::Millis(1);
    allocation.OnNetworkEstimateChanged(estimate);
    std::cout << "BWE=" << bwe << " screen=" << screenTrack.rate << " camera=" << cameraTrack.rate << '\n';
    if (bwe == 1800000) Check(screenTrack.rate > 2 * cameraTrack.rate, "bandwidth screen precedes camera");
    Check(uint64_t(screenTrack.rate) + cameraTrack.rate <= uint64_t(bwe), "bandwidth transport respects estimate");
    Check(cameraTrack.rate <= 900000, "bandwidth camera respects 900k ceiling");
    for (bool smallMinima : {false,true}) for (bool reversed : {false,true}) {
      webrtc::VideoCodec codec;
      codec.codecType = webrtc::kVideoCodecH264; codec.numberOfSimulcastStreams = 3;
      codec.active = true; codec.maxBitrate = 19800; codec.minBitrate = 30;
      codec.maxFramerate = 60;
      uint32_t allMinimum = 0;
      for (int i = 0; i < 3; ++i) {
        auto& s = codec.simulcastStream[reversed ? 2-i : i];
        s.width=streams[i].width; s.height=streams[i].height; s.active=true;
        s.minBitrate=smallMinima ? 30 : streams[i].min_bitrate_bps/1000;
        s.targetBitrate=streams[i].target_bitrate_bps/1000;
        s.maxBitrate=i==2?15000:i==1?4000:800;
        s.maxFramerate=i==2?60:i==1?30:15;
        s.numberOfTemporalLayers=1; allMinimum+=s.minBitrate*1000;
      }
      const int high = reversed ? 0 : 2;
      media::ScreenBitrateAllocatorFactory factory;
      auto fixed = factory.Create(env,codec);
      auto builtin = webrtc::CreateBuiltinVideoBitrateAllocatorFactory()->Create(env,codec);
      for (int repeat = 0; repeat < 3; ++repeat) {
        auto before = builtin->Allocate({screenTrack.rate,60});
        auto after = fixed->Allocate({screenTrack.rate,60});
        std::cout << "Allocation reversed=" << reversed << " screen=" << screenTrack.rate
                  << " stock_high=" << before.GetSpatialLayerSum(high)
                  << " fixed_high=" << after.GetSpatialLayerSum(high) << " minimum_sum=" << allMinimum << '\n';
        Check(after.get_sum_bps() <= screenTrack.rate, "bandwidth layer allocation respects estimate");
        if (screenTrack.rate >= codec.simulcastStream[high].minBitrate*1000)
          Check(after.GetSpatialLayerSum(high) >= codec.simulcastStream[high].minBitrate*1000,
                "bandwidth requested high survives affordable high");
        if (screenTrack.rate >= allMinimum) {
          const uint32_t highMinimum = codec.simulcastStream[high].minBitrate*1000;
          // 600 kbit/s is a policy floor, not a claim of 1080p60 quality.
          const uint32_t useful = std::min<uint32_t>(std::max<uint32_t>(600000, highMinimum), codec.simulcastStream[high].maxBitrate*1000);
          const uint32_t expected = std::min(useful, screenTrack.rate - (allMinimum - highMinimum));
          Check(after.GetSpatialLayerSum(high) >= expected,
                "bandwidth high grows before lower surplus");
          Check(after.GetSpatialLayerSum(high) >= codec.simulcastStream[high].minBitrate*1000,
                "bandwidth requested high survives affordable minima");
          for(int i=0;i<3;++i) Check(after.GetSpatialLayerSum(i)>=codec.simulcastStream[i].minBitrate*1000,
                "bandwidth affordable lower layers retain minima");
        }
      }
      if (smallMinima) {
        auto startup = factory.Create(env,codec);
        for (const auto [budget, expectedHigh] : std::array<std::pair<uint32_t,uint32_t>,6>{{
            {30000,30000},{60000,30000},{90000,30000},{110000,50000},{280000,220000},{660000,600000}}}) {
          const auto rate = startup->Allocate({budget,60});
          Check(rate.get_sum_bps() <= budget,"bandwidth startup stays within budget");
          Check(rate.GetSpatialLayerSum(high) == expectedHigh,"bandwidth startup explicit high floor");
          for(int i=0;i<3;++i) if(i!=high)
            Check(rate.GetSpatialLayerSum(i)<=30000,"bandwidth startup lower layers only minima");
        }
      }
      codec.simulcastStream[high].active=false;
      auto inactive=factory.Create(env,codec)->Allocate({screenTrack.rate,60});
      Check(inactive.GetSpatialLayerSum(high)==0,"bandwidth unwanted high remains off");
    }
  }
  allocation.RemoveObserver(&cameraTrack); allocation.RemoveObserver(&screenTrack);
}
