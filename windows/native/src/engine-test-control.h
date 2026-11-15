// Seams compiled exclusively by the regression executable. Production keeps
// the same Session/Transport implementations and the Windows scheduler/ADM.
#pragma once
#include "net.h"
#include "loopback.h"
#include "api/media_stream_interface.h"
#include <vector>
namespace engine_test {
extern int64_t now;
extern bool online;
extern bool applyOffers;
extern bool h264Formats;
extern int joins, offers;
extern std::function<void(const std::vector<int16_t>&)> reverse;
// What the transport hands the output device, sidetone included.
extern std::function<void(const int16_t*,size_t)> rendered;
extern std::function<net::Response(const std::string&,const json::Value&)> post;
extern webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> screenSource, cameraSource;
extern webrtc::scoped_refptr<loopback::SystemAudioSource> systemAudio;
}
