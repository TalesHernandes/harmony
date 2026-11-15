#pragma once
#include "api/media_stream_interface.h"
#include <string>
#include <vector>
namespace camera {
int WorkerMain(uintptr_t pipe,const std::string& id);
int DeadlineSelfTest();
int FallbackSelfTest();
struct Device { std::string id,name; };
std::vector<Device> Devices();
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> Open(std::string id);
}
