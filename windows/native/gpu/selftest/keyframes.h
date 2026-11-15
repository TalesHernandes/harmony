// Exercise the production encoder and inspect Annex B output, without WGC.
#include "video/encoder_rtcp_feedback.h"
class FeedbackProbe : public webrtc::VideoStreamEncoderInterface {
 public:
  std::vector<webrtc::VideoFrameType> requested;
  void SendKeyFrame(const std::vector<webrtc::VideoFrameType>& layers) override { requested=layers; }
  void AddAdaptationResource(webrtc::scoped_refptr<webrtc::Resource>) override {}
  std::vector<webrtc::scoped_refptr<webrtc::Resource>> GetAdaptationResources() override { return {}; }
  void SetSource(webrtc::VideoSourceInterface<webrtc::VideoFrame>*, const webrtc::DegradationPreference&) override {}
  void SetSink(EncoderSink*, bool) override {}
  void SetStartBitrate(int) override {}
  void OnLossNotification(const webrtc::VideoEncoder::LossNotification&) override {}
  void OnBitrateUpdated(webrtc::DataRate, webrtc::DataRate, uint8_t, int64_t, double) override {}
  void SetFecControllerOverride(webrtc::FecControllerOverride*) override {}
  void ConfigureEncoder(webrtc::VideoEncoderConfig, size_t) override {}
  void ConfigureEncoder(webrtc::VideoEncoderConfig, size_t, webrtc::SetParametersCallback) override {}
  void Stop() override {}
};
class KeyframeProbe : public webrtc::EncodedImageCallback {
 public:
  std::array<int,3> seen{}, idr{};
  std::vector<int> order;
  uint32_t timestamp=0;
  bool ended=false;
  int reject = -1;
  void Delivery(uint32_t at,int index,bool last) {
    if(at!=timestamp) {timestamp=at;order.clear();ended=false;}
    if(ended)throw std::runtime_error("callback after temporal unit ended");
    order.push_back(index);ended=last;
  }
  Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
    int i = image.SimulcastIndex().value_or(-1);
    Delivery(image.RtpTimestamp(),i,image.is_end_of_temporal_unit().value_or(false));
    if (i < 0 || i > 2) throw std::runtime_error("keyframe invalid stream index");
    bool key = false;
    for (size_t p = 0; p + 3 < image.size(); ++p)
      if (!image.data()[p] && !image.data()[p+1] && image.data()[p+2] == 1 &&
          (image.data()[p+3] & 31) == 5) key = true;
    if (key != (image._frameType == webrtc::VideoFrameType::kVideoFrameKey))
      throw std::runtime_error("keyframe metadata disagrees with NAL");
    ++seen[i]; if (key) ++idr[i];
    return Result(i == reject ? Result::ERROR_SEND_FAILED : Result::OK);
  }
  void OnFrameDropped(uint32_t at,int index,bool last) override {Delivery(at,index,last);}
};
static int Keyframes(bool retry) {
  for (bool reverse : {false, true}) {
    auto env = webrtc::CreateEnvironment();
    auto factory = gpu::CreateEncoderFactory();
    auto encoder = factory->Create(env, factory->GetSupportedFormats().front());
    webrtc::VideoCodec codec{};
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 1920; codec.height = 1080; codec.maxFramerate = 60;
    codec.startBitrate = codec.maxBitrate = 19800;
    codec.numberOfSimulcastStreams = 3;
    codec.mode = webrtc::VideoCodecMode::kScreensharing;
    webrtc::VideoBitrateAllocation rates;
    for (int rank = 0; rank < 3; ++rank) {
      int i = reverse ? 2-rank : rank;
      auto& layer = codec.simulcastStream[i];
      layer.width = 1920 >> rank; layer.height = 1080 >> rank;
      layer.maxFramerate = 60 >> rank;
      layer.targetBitrate = layer.maxBitrate = rank == 0 ? 15000 : rank == 1 ? 4000 : 800;
      layer.minBitrate = 100; layer.active = true; layer.qpMax = 51;
      rates.SetBitrate(i, 0, layer.targetBitrate * 1000);
    }
    KeyframeProbe sink;
    if (encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(
        webrtc::VideoEncoder::Capabilities(false), 4, 1200)))
      throw std::runtime_error("keyframe InitEncode failed");
    encoder->RegisterEncodeCompleteCallback(&sink);
    auto setRates = [&] { encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(rates, 60)); };
    setRates();
    auto texture = gpu::Texture(1920,1080,DXGI_FORMAT_B8G8R8A8_UNORM);
    std::vector<uint32_t> pixels(1920*1080, 0xff305080);
    gpu::Context()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),1920*4,0);
    auto buffer = webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get());
    int64_t now = 1000000;
    auto encode = [&](int request, int step = 16667) {
      now += step;
      std::vector<webrtc::VideoFrameType> types(3, webrtc::VideoFrameType::kVideoFrameDelta);
      if (request >= 0) types[request] = webrtc::VideoFrameType::kVideoFrameKey;
      return encoder->Encode(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
          .set_timestamp_us(now).set_rtp_timestamp(uint32_t(now*90/1000)).build(), &types);
    };
    auto require = [](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); };
    require(encode(-1) == 0, "keyframe warmup");
    int high = reverse ? 2 : 0;
    FeedbackProbe feedbackSink;
    // These SSRCs are fixture identifiers, in codec stream order. Exercise the
    // pinned libwebrtc's actual feedback mapper, without claiming wire RTCP.
    std::vector<uint32_t> ssrcs = reverse ? std::vector<uint32_t>{303,202,101} : std::vector<uint32_t>{101,202,303};
    webrtc::EncoderRtcpFeedback feedback(env, true, ssrcs, &feedbackSink, {});
    for (int request : {high, 1, 2-high, high, high, high}) {
      Sleep(310); // Respect libwebrtc's per-SSRC PLI rate limit.
      feedbackSink.requested.clear();
      feedback.OnReceivedIntraFrameRequest(ssrcs[request]);
      require(feedbackSink.requested.size() == 3, "keyframe SSRC feedback delivered");
      for (int i=0; i<3; ++i)
        require((feedbackSink.requested[i] == webrtc::VideoFrameType::kVideoFrameKey) == (i == request),
            "keyframe SSRC maps to codec index");
      auto before = sink.idr;
      setRates();
      require(encode(request, 66667) == 0, "keyframe isolated encode");
      require(sink.order.front()==request,"requested IDR precedes old-layer deltas");
      require(sink.ended&&sink.order.size()==3,"all callbacks finish one temporal unit");
      for (int i = 0; i < 3; ++i)
        require(sink.idr[i]-before[i] == (i == request), "keyframe isolated stream IDR");
    }
    // A request received between low-layer deadlines must survive delta inputs.
    int low = 2-high;
    auto before = sink.idr;
    require(encode(low, 1000) == 0 && sink.idr == before, "keyframe low cadence defers");
    setRates();
    require(encode(-1, 66667) == 0 && sink.idr[low] == before[low]+1 &&
        sink.idr[high] == before[high], "keyframe pending survives cadence and rates");
    // High takes even an early input: no second cadence gate.
    before = sink.idr;
    require(encode(high, 1000) == 0 && sink.idr[high] == before[high]+1,
        "keyframe high maximum cadence");
    std::cout << "keyframe SSRC/isolated/repeated/cadence/rates order=" << reverse
              << " backend=" << gpu::ActiveEncoderName() << " PASS\n";
    if (retry) {
      sink.reject = high;
      require(encode(high, 66667) != 0, "keyframe callback rejection injected");
      before = sink.idr; sink.reject = -1;
      setRates();
      require(encode(-1) == 0 && sink.idr[high] == before[high]+1,
          "keyframe pending survives rejected delivery");
      std::cout << "keyframe rejected delivery order=" << reverse << " PASS\n";
    }
    rates.SetBitrate(high,0,0);setRates();
    require(encode(low,66667)==0&&sink.order.front()==low&&sink.ended&&sink.order.size()==3,
        "IDR priority preserves temporal end with disabled high layer");
    encoder->Release();
  }
  return 0;
}
