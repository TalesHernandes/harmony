// Offline, production APM/DFN transport; all PCM stays in memory.
#include "resource-meter.h"
#define HARMONY_ENGINE_TEST 1
#include "../../native/src/audio.cpp"
#include "../../native/src/perf.h"
#include "../../native/src/engine-test-control.h"
#include <iostream>
namespace engine_test {
int64_t now=1000;bool online=true,applyOffers=false,h264Formats=false;int joins=0,offers=0;
std::function<void(const std::vector<int16_t>&)> reverse;
std::function<void(const int16_t*,size_t)> rendered;
std::function<net::Response(const std::string&,const json::Value&)> post;
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> screenSource,cameraSource;
webrtc::scoped_refptr<loopback::SystemAudioSource> systemAudio;
}
static uint64_t Cpu() {FILETIME c{},e{},k{},u{};GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u);return resource_meter::Time(k)+resource_meter::Time(u);}
template<class F> void Measure(char const* name,int count,F run) {
 auto cpu=Cpu();auto start=perf::Stage::Now();for(int i=0;i<count;++i)run(i);
 auto ns=perf::Stage::Now()-start;auto cpuMs=double(Cpu()-cpu)/10000;
 std::cout<<name<<" blocks="<<count<<" wall_ms_per_10ms="<<double(ns)/count/1e6<<" cpu_ms_per_10ms="<<cpuMs/count<<" core_percent_realtime="<<cpuMs/count*10<<'\n';
}
int main(int argc,char** argv) {
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 try {
  std::filesystem::path directory=argc>1?std::filesystem::absolute(argv[1]):audio::DfnDirectory();
  std::vector<int16_t> pcm(480);for(size_t i=0;i<480;++i)pcm[i]=int16_t(6000*std::sin(6.283185307179586*440*i/48000));
  auto seed=pcm;audio::DfnFilter dfn(directory);if(!dfn.Available())throw std::runtime_error("DFN unavailable");
  for(int i=0;i<100;++i){pcm=seed;dfn.Process(pcm,48000,1,50);}
  Measure("dfn-tone",3000,[&](int){pcm=seed;if(!dfn.Process(pcm,48000,1,50))throw std::runtime_error("DFN process failed");});
  std::vector<int16_t> silence(480);
  Measure("dfn-exact-silence",10000,[&](int){dfn.Process(silence,48000,1,50);});
  for(bool enabled:{false,true})for(bool audienceOn:{false,true}) {
   webrtc::AudioProcessing::Config config;config.echo_canceller.enabled=true;config.high_pass_filter.enabled=true;config.noise_suppression.enabled=true;config.gain_controller2.enabled=true;config.gain_controller2.adaptive_digital.enabled=true;
   auto apm=webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
   auto mic=webrtc::make_ref_counted<audio::MicSource>();
   auto slot=std::make_shared<audio::FilterSlot>(std::make_unique<audio::DfnFilter>(directory));slot->enabled=enabled;
   auto counter=std::make_shared<std::atomic<uint64_t>>(0);auto audience=std::make_shared<std::atomic<bool>>(audienceOn);
   audio::Transport transport(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false,false,audience);transport.capture_calls_=counter;
   uint32_t level=0;auto run=[&](int){transport.RecordedDataIsAvailable(seed.data(),480,2,1,48000,0,0,0,false,level);};
   for(int i=0;i<100;++i)run(i);
   std::string label="microphone-dfn-"+std::to_string(enabled)+"-audience-"+std::to_string(audienceOn);
   Measure(label.c_str(),3000,run);std::cout<<"dfn_active="<<slot->active<<" fallbacks="<<slot->fallbacks<<'\n';
  }
  std::cout<<"PASS: offline audio measurement\n";
 }catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
 CoUninitialize();return 0;
}
