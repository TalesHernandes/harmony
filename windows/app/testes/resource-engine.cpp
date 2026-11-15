#include "resource-meter.h"
#define HARMONY_RESOURCE_MEASUREMENT 1
// Compile the production implementation itself; only OS/HTTP/clock edges are doubled.
#define HARMONY_ENGINE_TEST 1
#include "pc/session_description.h"
#define HARMONY_GPU 0
#define HARMONY_CAPTURE_TEST 1
#include "../../native/gpu/capture.cpp"
#include "../../native/src/voice.cpp"
#include "../../native/src/audio.cpp"
#include "fake-adm.h"
#include "modules/audio_device/include/audio_device_factory.h"
#include "../src/device-proof.h"
#include <iostream>
#include <fstream>
#include "api/video/adapted_video_track_source.h"
#include "api/make_ref_counted.h"
namespace engine_test {
int64_t now=1000;
bool online=true;
bool applyOffers=false;
bool h264Formats=false;
int joins=0,offers=0;
std::function<void(const std::vector<int16_t>&)> reverse;
std::function<void(const int16_t*,size_t)> rendered;
std::function<net::Response(const std::string&,const json::Value&)> post;
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> screenSource,cameraSource;
webrtc::scoped_refptr<loopback::SystemAudioSource> systemAudio;
}

namespace voice {
class EngineRegression {
 public:
  static void Run(int cycles) {
    net::Client client("http://127.0.0.1:9");Options options;
    options.simulated_device=true;options.mic=true;
    engine_test::post=[](auto const&,auto const&){return net::Response{200,"{}"};};
    resource_meter::Mark("engine-baseline");
    for(int i=0;i<cycles;++i) {
      {
        Session s(&client,options);
        if(!s.CreateFactory() || !s.CreateConnection(json::Value::Array()))throw std::runtime_error("factory/connection failed");
        // Actual PC/factory and production audio with the existing in-memory ADM.
        s.mic_=s.factory_->CreateAudioTrack("mic",s.audio_.mic.get());
        *s.audio_.audience=true;
        s.audio_.device->Init();s.audio_.device->InitRecording();s.audio_.device->StartRecording();
        s.audio_.device->InitPlayout();s.audio_.device->StartPlayout();
        Sleep(i==0?3000:100);
        auto label="cycle-"+std::to_string(i+1)+"-during";resource_meter::Mark(label.c_str());
        s.Leave();label="cycle-"+std::to_string(i+1)+"-leave-alive";resource_meter::Mark(label.c_str());
      }
      Sleep(100);auto label="cycle-"+std::to_string(i+1)+"-destroyed";resource_meter::Mark(label.c_str());
    }
    Sleep(3000);resource_meter::Mark("engine-final-settled");
  }
};
}
int main(int argc,char** argv) {
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  try {voice::EngineRegression::Run(argc>1?std::stoi(argv[1]):20);std::cout<<"PASS: controlled room lifecycle measured\n";}
  catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
  CoUninitialize();return 0;
}
