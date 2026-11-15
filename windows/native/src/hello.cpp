// Smallest program that proves libwebrtc links with our compiler settings.
#include <cstdio>
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/thread.h"

int main() {
  auto network = webrtc::Thread::CreateWithSocketServer();
  auto worker = webrtc::Thread::Create();
  auto signaling = webrtc::Thread::Create();
  network->Start();
  worker->Start();
  signaling->Start();
  auto factory = webrtc::CreatePeerConnectionFactory(
      network.get(), worker.get(), signaling.get(), nullptr,
      webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
      nullptr, nullptr);
  std::printf("factory %s\n", factory ? "ok" : "failed");
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  auto caps = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
  for (auto& c : caps.codecs) std::printf("video codec %s\n", c.name.c_str());
  return factory ? 0 : 1;
}
