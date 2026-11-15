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
static int checks=0;
static void Check(bool value,const char* message) { ++checks; if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
#include "w4-bandwidth-tests.h"
#include "media-tests.h"
namespace gpu {
struct CaptureRegression {
  static auto Freeze() {
    auto source=webrtc::make_ref_counted<CaptureSource>(nullptr,nullptr,60,1080);
    source->stop_=true;source->worker_.join();
    return source;
  }
  static bool Frame(CaptureSource* source,int64_t us) {
    int ow,oh,cw,ch,cx,cy;
    return source->PrepareFrame(1920,1080,us,&ow,&oh,&cw,&ch,&cx,&cy);
  }
};
}

class FakeTrack : public webrtc::Notifier<webrtc::AudioTrackInterface> {
 public:
  bool on=true;
  std::string kind() const override { return kAudioKind; }
  std::string id() const override { return "mic"; }
  bool enabled() const override { return on; }
  bool set_enabled(bool enabled) override { on=enabled; return true; }
  TrackState state() const override { return kLive; }
  webrtc::AudioSourceInterface* GetSource() const override { return nullptr; }
  void AddSink(webrtc::AudioTrackSinkInterface*) override {}
  void RemoveSink(webrtc::AudioTrackSinkInterface*) override {}
};
class SilentRender : public webrtc::AudioTransport {
 public:
  int32_t RecordedDataIsAvailable(const void*,size_t,size_t,size_t,uint32_t,uint32_t,int32_t,uint32_t,bool,uint32_t&) override { return 0; }
  int32_t NeedMorePlayData(size_t frames,size_t bytes,size_t channels,uint32_t,void* data,size_t& returned,int64_t*,int64_t*) override { memset(data,0,frames*bytes); returned=frames*channels; return 0; }
  void PullRenderData(int,int,size_t channels,size_t frames,void* data,int64_t*,int64_t*) override { memset(data,0,frames*channels*2); }
};
static void RunSuppressionRegression() {
  struct Backend : audio::NoiseFilter {
    int mode=0,calls=0;
    bool Process(std::vector<int16_t>& block,int,size_t,int) override {
      ++calls;
      if(mode==2) std::this_thread::sleep_for(std::chrono::milliseconds(12));
      if(mode==3) throw 1;
      if(mode==4) { block.resize(1); return true; }
      std::fill(block.begin(),block.end(),777); return mode!=1;
    }
  };
  struct Sink : webrtc::AudioTrackSinkInterface {
    int16_t first=0;
    void OnData(const void* data,int,int,size_t,size_t) override { first=static_cast<const int16_t*>(data)[0]; }
  } sink;
  webrtc::AudioProcessing::Config config;
  config.noise_suppression.enabled=true;
  config.noise_suppression.level=webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  config.echo_canceller.enabled=true; config.high_pass_filter.enabled=true; config.gain_controller2.enabled=true;
  auto apm=webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
  auto mic=webrtc::make_ref_counted<audio::MicSource>(); mic->AddSink(&sink);
  int16_t gateSample = 123;
  mic->Mute(true); mic->Deliver(&gateSample,48000,1,1);
  Check(sink.first == 0,"immediate mute blocks microphone source delivery");
  mic->Mute(false); mic->Deliver(&gateSample,48000,1,1);
  Check(sink.first == gateSample,"unmute restores microphone source delivery");

  auto backend=std::make_unique<Backend>(); auto* control=backend.get();
  auto slot=std::make_shared<audio::FilterSlot>(std::move(backend));
  auto counter=std::make_shared<std::atomic<uint64_t>>(0);
  auto audience=std::make_shared<std::atomic<bool>>(true);
  audio::Transport transport(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false,false,audience);
  transport.capture_calls_=counter;
  std::array<int16_t,480> pcm; pcm.fill(1200); uint32_t level=0;
  auto pulse=[&] { transport.RecordedDataIsAvailable(pcm.data(),480,2,1,48000,0,0,0,false,level); };
  auto recover=[&] {
    control->mode=0;
    for(int i=0;i<20;++i) {
      pulse();
      Check(!slot->active && slot->nsApplied && apm->GetConfig().noise_suppression.enabled && sink.first!=777,
            "engine DFN probation publishes NS only for 20 good blocks");
    }
    pulse();
    Check(slot->active && !slot->nsApplied && !apm->GetConfig().noise_suppression.enabled && sink.first==777,
          "engine active DFN never overlaps APM NS");
    auto now=apm->GetConfig();
    Check(now.echo_canceller.enabled && now.high_pass_filter.enabled && now.gain_controller2.enabled,
          "engine NS transition preserves AEC HPF AGC");
  };
  recover();
  for(int i=0;i<40;++i) { pulse(); Check(slot->active && !apm->GetConfig().noise_suppression.enabled,"engine steady DFN keeps NS disabled"); }
  for(int mode : {1,2,3,4}) {
    control->mode=mode; auto before=slot->fallbacks.load(); pulse();
    Check(!slot->active && !slot->nsApplied && slot->fallbacks==before+1 && apm->GetConfig().noise_suppression.enabled && sink.first!=777,
          "engine fallen DFN restores APM NS immediately");
    pulse(); Check(!slot->active && slot->nsApplied && sink.first!=777,"engine DFN failure has at most one unsuppressed block");
    recover();
  }
  slot->enabled=false; pulse();
  Check(!slot->active && slot->nsApplied && apm->GetConfig().noise_suppression.enabled,"engine disabled DFN restores NS in same block");
  slot->enabled=true;
  // Ten 190 ms good bursts followed by a failure must never toggle NS off.
  for(int burst=0;burst<10;++burst) {
    control->mode=0;
    for(int i=0;i<19;++i) { pulse(); Check(!slot->active && slot->nsApplied && apm->GetConfig().noise_suppression.enabled,"engine DFN hysteresis rejects 19-block bursts"); }
    control->mode=1; pulse();
    Check(slot->nsApplied && apm->GetConfig().noise_suppression.enabled,"engine DFN bad probe resets consecutive recovery");
  }
  recover();
  *audience=false;auto calls=control->calls;
  for(int i=0;i<50;++i) pulse();
  Check(control->calls==calls && !slot->active && slot->nsApplied && slot->reason==audio::FilterSlot::Alone,
        "W12 alone pauses DFN Process and reports Alone while publishing APM");
  *audience=true;
  recover();
  Check(control->calls==calls+21 && sink.first==777,"W12 listener resumes DFN publication after 20 recovery blocks");
  std::cout<<"W12 DFN recovery: 20 recovery quanta (200 ms of audio); DFN publication on the next quantum\n";
  *audience=false;calls=control->calls;for(int i=0;i<25;++i) pulse();
  Check(control->calls==calls && !slot->active && slot->reason==audio::FilterSlot::Alone,"W12 listener leaves and DFN pauses again");
  Check(std::string(audio::FilterSlot::ReasonText(audio::FilterSlot::Alone))=="DFN em pausa: ninguém ouve (sozinho na sala ou no mudo)","W12 Alone says in Portuguese why DFN is paused");
  mic->RemoveSink(&sink);
  // No backend/model: repeated callbacks must retain kHigh, with truthful status.
  slot=std::make_shared<audio::FilterSlot>();
  audio::Transport absent(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false);
  absent.capture_calls_=counter;
  apm->ApplyConfig(config);
  for(int i=0;i<40;++i) absent.RecordedDataIsAvailable(pcm.data(),480,2,1,48000,0,0,0,false,level);
  Check(!slot->active && slot->nsApplied && apm->GetConfig().noise_suppression.enabled &&
        apm->GetConfig().noise_suppression.level==webrtc::AudioProcessing::Config::NoiseSuppression::kHigh,"engine absent DFN retains high NS");
  // The same pipeline serves direct calls, which always have a peer and do
  // not receive room offers. They must retain DFN without audience updates.
  auto direct=audio::CreatePipeline(false,std::make_unique<Backend>(),true);
  Check(direct.device && direct.device->Init()==0 && direct.device->InitRecording()==0 && direct.device->StartRecording()==0,
        "W12 direct-call pipeline starts controlled capture");
  auto deadline=GetTickCount64()+1500;
  while(!direct.filter->active && GetTickCount64()<deadline) Sleep(5);
  direct.device->StopRecording();direct.device->Terminate();
  Check(direct.filter->active && direct.filter->reason==audio::FilterSlot::Running,
        "W12 direct-call DFN stays enabled without room audience tracking");
  std::cout<<"DFN hysteresis: 20 good blocks = 200 ms; 10 bursts of 19 good + failure: no NS toggles; raw gap <= 10 ms\n";
}
static void RunRealCaptureFormats() {
  auto directory=std::filesystem::absolute("out");
  for(auto rate : {8000,16000,44100,48000,96000,192000}) for(size_t channels : {1,2,4,8}) {
    auto apm=webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
    auto mic=webrtc::make_ref_counted<audio::MicSource>();
    auto slot=std::make_shared<audio::FilterSlot>(std::make_unique<audio::DfnFilter>(directory));
    auto counter=std::make_shared<std::atomic<uint64_t>>(0);
    audio::Transport transport(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false);
    transport.capture_calls_=counter;
    auto adm=webrtc::make_ref_counted<FakeADM>(); adm->playing=false; adm->captureRate=rate; adm->captureChannels=channels;
    adm->RegisterAudioCallback(&transport);
    // Activation proves the production recovery streak of 20 good quanta.
    // A later supported deadline fallback must not make format compatibility
    // depend on the scheduler at exactly the last of 60 pulses.
    for(int i=0;i<60&&!(slot->active&&slot->reason==audio::FilterSlot::Running);++i) adm->Pulse();
    Check(slot->active && slot->reason==audio::FilterSlot::Running,"real DFN active through controlled ADM at every capture format");
    adm->RegisterAudioCallback(nullptr);
    std::cout<<"Real DFN ADM "<<rate<<" Hz / "<<channels<<" channels: active\n";
  }
}
static void RunCaptureFormats() {
  struct Strict : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& block,int rate,size_t channels,int) override {
      if(rate!=48000 || channels!=1 || block.size()!=480) return false;
      std::fill(block.begin(),block.end(),777); return true;
    }
  };
  struct Sink : webrtc::AudioTrackSinkInterface {
    int rate=0; size_t channels=0,frames=0; int16_t first=0; std::array<int16_t,480> pcm{};
    void OnData(const void* data,int bits,int r,size_t c,size_t f) override {
      Check(bits==16,"capture publication is signed 16 bit"); rate=r; channels=c; frames=f; first=static_cast<const int16_t*>(data)[0]; if(f==480 && c==1) std::copy_n(static_cast<const int16_t*>(data),480,pcm.begin());
    }
  } sink;
  for(auto rate : {8000,16000,44100,48000,96000,192000}) for(size_t channels : {1,2,4,8}) {
    auto apm=webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
    auto mic=webrtc::make_ref_counted<audio::MicSource>(); mic->AddSink(&sink);
    auto slot=std::make_shared<audio::FilterSlot>(std::make_unique<Strict>());
    auto counter=std::make_shared<std::atomic<uint64_t>>(0);
    audio::Transport transport(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false);
    transport.capture_calls_=counter;
    auto owned=webrtc::make_ref_counted<FakeADM>(); auto& adm=*owned; adm.playing=false; adm.captureRate=rate; adm.captureChannels=channels; adm.RegisterAudioCallback(&transport);
    for(int i=0;i<25;++i) adm.Pulse();
    Check(slot->active && slot->fallbacks==0 && sink.rate==48000 && sink.channels==1 && sink.frames==480 && sink.first==777,
          "capture all ADM formats activate DFN and publish mono 48k");
    Check(!slot->nsApplied && !apm->GetConfig().noise_suppression.enabled,"capture active DFN excludes APM NS");
    slot->micGain=0; adm.Pulse(); Check(sink.first==0 && slot->active,"capture microphone zero gain silences publication");
    slot->micGain=2; adm.Pulse(); Check(sink.first==1554 && slot->active,"capture microphone gain applied after DFN"); slot->micGain=1;
    slot->enabled=false; slot->echo=false; slot->noise=false; slot->agc=false; adm.Pulse();
    auto c=apm->GetConfig();
    Check(!c.echo_canceller.enabled && !c.noise_suppression.enabled && !c.gain_controller2.enabled && !slot->active && !slot->nsApplied,
          "capture live microphone filters disabled at APM");
    for(int i=0;i<5;++i) adm.Pulse();
    double energy=0; unsigned crossings=0;
    for(size_t i=0;i<sink.pcm.size();++i) { energy+=double(sink.pcm[i])*sink.pcm[i]; if(i && sink.pcm[i-1]<=0 && sink.pcm[i]>0) ++crossings; }
    std::cout<<"Published tone "<<rate<<"/"<<channels<<" RMS="<<std::sqrt(energy/480)<<" crossings="<<crossings<<"\n";
    Check(std::sqrt(energy/480)>2000 && std::sqrt(energy/480)<6000 && crossings>=3 && crossings<=6,
          "capture resampling preserves microphone tone energy and frequency");
    slot->echo=true; slot->noise=true; slot->agc=true; adm.Pulse(); c=apm->GetConfig();
    Check(c.echo_canceller.enabled && c.noise_suppression.enabled && c.gain_controller2.enabled,
          "capture live microphone filters enabled at APM");
    Check(slot->reason==audio::FilterSlot::Disabled,"capture DFN disabled reason visible");
    adm.RegisterAudioCallback(nullptr); mic->RemoveSink(&sink);
    std::cout<<"ADM "<<rate<<" Hz / "<<channels<<" channels: DFN active, mono 480 published, filters live\n";
  }
}
namespace voice {
class EngineRegression {
 public:
  class VideoSource : public webrtc::AdaptedVideoTrackSource {
   public:
    SourceState current=kLive;
    SourceState state() const override{return current;}
    bool remote() const override{return false;}
    bool is_screencast() const override{return true;}
    std::optional<bool> needs_denoising() const override{return false;}
  };
  static void CaptureOpening() {
    // Caller owns a window. Selftest sleeps without a pump; WinUI uses STA
    // and dispatches messages. Both enqueue to service -> voice -> capture MTA.
    bool allOpened=true;
    for(bool ui : {false,true}) {
      std::atomic<bool> done=false,opened=false;
      std::thread caller([&] {
        // ScreenSelfTest has no caller apartment after TestAudioDevice balances COM.
        auto apartment=ui?CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED):CO_E_NOTINITIALIZED;
        HWND notice=CreateWindowExW(0,L"STATIC",L"Opening fixture",0,0,0,1,1,HWND_MESSAGE,nullptr,nullptr,nullptr);
        std::thread service([&] {
          std::thread voice([&] {
            net::Client client("http://127.0.0.1:8103");Options options;options.simulated_device=true;options.mic=false;
            Session s(&client,options);s.session_id_="opening-session";
            Check(s.CreateFactory()&&s.CreateConnection(json::Value::Array()),"opening creates peer connection");
            auto source=gpu::CreateMonitorSource(reinterpret_cast<HMONITOR>(1),60,1080);
            engine_test::screenSource=source;
            int offers=0;engine_test::post=[&](const std::string& path,const json::Value&){if(path=="/api/voice/offer")++offers;return net::Response{409,"{}"};};
            s.options_.share_monitor=reinterpret_cast<HMONITOR>(1);s.options_.share_audio=false;s.ShareScreen();s.wanted_=true;
            auto until=GetTickCount64()+600;
            // No consumer/track is installed before the source becomes live.
            while(!s.screen_&&GetTickCount64()<until) {engine_test::now+=25;s.Tick();Sleep(5);}
            opened=s.screen_&&source->state()==webrtc::MediaSourceInterface::kLive&&offers==1;
            engine_test::screenSource=nullptr;engine_test::post={};s.wanted_=false;
          });
          voice.join();done=true;
        });
        while(!done) {
          if(ui) {MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
          Sleep(25);
        }
        service.join();if(notice)DestroyWindow(notice);if(SUCCEEDED(apartment))CoUninitialize();
      });
      caller.join();
      const char* label=ui?"opening STA app reaches track and offer without prior sink":"opening sleeping selftest reaches track and offer without prior sink";
      if(!opened)std::cerr<<label<<'\n';else std::cout<<label<<": PASS\n";
      allOpened=allOpened&&opened;

    }
    Check(allOpened,"opening both callers reach offer without prior sink");
    std::promise<bool> pending;auto done=pending.get_future();auto start=GetTickCount64();
    std::thread callback([&]{Sleep(100);pending.set_value(true);});
    Check(!Await(done,"test_description",10)&&GetTickCount64()-start<80,"opening description timeout is bounded");
    callback.join();Check(Await(done,"test_description",10),"opening late callback remains safe after timeout");
  }
  static void ScreenCamera(bool h264=true,bool cameraFirst=false) {
    using namespace engine_test;
    net::Client client("http://127.0.0.1:18106");Options options;options.simulated_device=true;options.mic=false;options.share_fps=60;options.share_height=1080;
    Session s(&client,options);
    auto evidence=[](const std::string& label,const std::string& sdp) {
      if(sdp.empty())return;
      std::cout<<"SDP-BEGIN "<<label<<'\n';std::istringstream in(sdp);std::string line;
      while(std::getline(in,line))if(line.starts_with("m=")||line.starts_with("a=mid:")||line.starts_with("a=rtpmap:")||line.starts_with("a=fmtp:")||line.starts_with("a=group:BUNDLE")||line.starts_with("a=rid:")||line.starts_with("a=simulcast:"))std::cout<<line<<'\n';
      std::cout<<"SDP-END\n";
    };
    post=[&](const std::string& path,const json::Value& body){evidence(path,body["sdp"].Str());auto r=client.Post(path,body);evidence(path+"-reply",r.Json()["sdp"].Str());if(r.status>=400)std::cerr<<path<<": "<<r.body<<'\n';return r;};
    auto body=json::Value::Object();body["user"]=1;
    auto joined=client.Post("/join?name=sender",body);Check(joined.status==200,"round6 Go SFU join");
    s.session_id_=joined.Json()["session_id"].Str();applyOffers=true;
    h264Formats=h264;
    Check(s.CreateFactory()&&s.CreateConnection(json::Value::Array()),"round6 real peer connection");
    s.ApplyOffer(joined.Json());Check(!s.applied_answer_.empty(),"round6 initial answer accepted");
    auto source=webrtc::make_ref_counted<VideoSource>();cameraSource=source;
    auto nextOffer=[&] {
      json::Value offer;for(int i=0;i<100;++i){auto event=client.Get("/event?name=sender").Json();if(!event["sdp"].Str().empty()){offer=event;break;}Sleep(20);}
      Check(!offer["sdp"].Str().empty(),"round7 server offer arrives");evidence("server-offer",offer["sdp"].Str());return offer;
    };
    if(cameraFirst) {s.SetCamera(true,"fixture");auto offer=nextOffer();s.ApplyOffer(offer);Check(s.applied_offer_==offer["sdp"].Str(),"round7 camera first answer accepted");}
    s.CommitScreen(source);Check(s.screen_offered_,"round6 screen offered");
    auto screenMid=s.send_mids_["screen"];
    {
      // A start rate set on a running transport fires a probe sized to the voice
      // alone and the estimate falls to it (r21). The screen's answer must not
      // carry one.
      std::string remote;s.pc_->remote_description()->ToString(&remote);
      Check(remote.find("x-google-start-bitrate")==std::string::npos,"round22 screen answer sets no start bitrate");
    }
    if(cameraFirst) {s.SetCamera(false,"fixture");s.ApplyOffer(nextOffer());}
    s.SetCamera(true,"fixture");
    json::Value offer=nextOffer();
    Check(offer["send_mids"]["screen"].Str()==screenMid&&offer["send_mids"]["cam"].Str()!=screenMid,"round6 server preserves screen mid and gives camera own mid");
    auto codecs=[&](const std::string& sdp){std::istringstream in(sdp);std::string line;while(std::getline(in,line))if(line.starts_with("m=")||line.starts_with("a=mid:")||line.starts_with("a=rtpmap:96 ")||line.starts_with("a=rid:")||line.starts_with("a=simulcast:")||line.starts_with("a=group:BUNDLE"))std::cout<<line<<'\n';};
    std::cout<<"Round6 server offer codec/mid/RID evidence (no ICE credentials):\n";codecs(offer["sdp"].Str());
    auto before=s.screen_tr_->sender()->GetParameters();Check(before.encodings.size()==3,"round6 three screen layers before renegotiation");
    for(size_t i=0;i<3;++i)Check(before.encodings[i].rid==kLayers[i].rid&&before.encodings[i].active,"round6 layers active before renegotiation");
    s.ApplyOffer(offer);
    Check(s.send_mids_["screen"]==screenMid&&s.screen_tr_->mid()==screenMid&&s.screen_tr_->sender()->track()==s.screen_,"round6 screen mid and source preserved");
    Check(s.send_mids_["cam"]!=screenMid,"round6 camera gets own mid");
    bool cameraOwn=false;for(auto& tr:s.pc_->GetTransceivers())if(tr->mid()==s.send_mids_["cam"])cameraOwn=tr->sender()->track()==s.camera_;
    Check(cameraOwn,"round6 camera track in its own slot");
    auto p=s.screen_tr_->sender()->GetParameters();Check(p.encodings.size()==3,"round6 three screen layers survive");
    for(size_t i=0;i<3;++i)Check(p.encodings[i].rid==kLayers[i].rid&&p.encodings[i].active,"round6 screen RID order and active layers preserved");
    std::cout<<"Round6 real transceivers: screen mid/source unchanged, camera has own mid, alta/media/baixa active\n";
    Check(s.applied_offer_==offer["sdp"].Str(),"round6 camera answer accepted by Go SFU");
    auto originalPc=s.pc_;auto cameraMid=s.send_mids_["cam"];
    for(int cycle=0;cycle<4;++cycle) {
      for(bool enabled:{false,true}) {
        s.SetCamera(enabled,"fixture");auto update=nextOffer();s.ApplyOffer(update);
        Check(s.applied_offer_==update["sdp"].Str(),"round7 repeated camera answer accepted");
        Check(s.pc_==originalPc&&s.screen_tr_->mid()==screenMid&&s.screen_tr_->sender()->track()==s.screen_,"round7 repeated camera preserves connection and screen");
        if(enabled) {cameraMid=s.send_mids_["cam"];Check(!cameraMid.empty()&&cameraMid!=screenMid,"round7 repeated camera owns separate mid");}
        auto parameters=s.screen_tr_->sender()->GetParameters();Check(parameters.encodings.size()==3,"round7 repeated camera three RIDs");
        for(size_t i=0;i<3;++i)Check(parameters.encodings[i].active&&parameters.encodings[i].rid==kLayers[i].rid,"round7 repeated camera active RID");
        for(auto& tr:s.pc_->GetTransceivers())if(tr->mid()==cameraMid) {
          Check(tr->sender()->track()==s.camera_,"round7 repeated camera own track or empty");
          if(enabled) {auto d=webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer,s.applied_answer_);auto* m=d->description()->GetContentDescriptionByName(cameraMid);Check(m&&std::any_of(m->codecs().begin(),m->codecs().end(),[](const auto& c){return c.name=="VP8";}),"round7 camera negotiates server VP8");}
        }
        if(h264) {auto d=webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer,s.applied_answer_);auto* m=d->description()->GetContentDescriptionByName(screenMid);Check(m&&m->codecs().front().name=="H264","round7 screen keeps H264 preference");}
      }
    }
    std::cout<<"Round7 four camera off/on cycles; stable mids, three RIDs; camera-first="<<cameraFirst<<": PASS\n";
    s.CloseMedia();post={};cameraSource=nullptr;applyOffers=false;h264Formats=false;
    std::cout<<"Round6 screen + camera Go SFU negotiation: PASS\n";
  }
  static void Media() {
    using namespace engine_test;
    auto metadata=json::Value::Object();metadata["camera"]="Redmi 15 (Câmera Virtual do Windows)";auto parsed=json::Parse(metadata.Dump());
    Check(parsed&&(*parsed)["camera"].Str()=="Redmi 15 (Câmera Virtual do Windows)"&&net::Narrow(net::Wide((*parsed)["camera"].Str()))==(*parsed)["camera"].Str(),"engine accented camera name survives JSON and Windows conversion");
    net::Client client("http://127.0.0.1:8103");Options options;options.simulated_device=true;options.mic=false;options.share_fps=60;options.share_height=1080;
    Session s(&client,options);s.session_id_="media-session";
    Check(s.CreateFactory()&&s.CreateConnection(json::Value::Array()),"engine media creates real peer connection");
    auto source=webrtc::make_ref_counted<VideoSource>();s.screen_=s.factory_->CreateVideoTrack(source,"screen");
    webrtc::RtpTransceiverInit init;init.direction=webrtc::RtpTransceiverDirection::kSendOnly;
    for(int i:{2,1,0}){webrtc::RtpEncodingParameters e;e.rid=kLayers[i].rid;e.scale_resolution_down_by=kLayers[i].scale;init.send_encodings.push_back(e);}
    auto added=s.pc_->AddTransceiver(s.screen_,init);Check(added.ok(),"engine media adds permuted RIDs");s.screen_tr_=added.MoveValue();
    int offers=0;post=[&](const std::string& path,const json::Value&){if(path=="/api/voice/offer")++offers;return net::Response{409,"{}"};};
    s.OfferScreen(1);Check(offers==1&&!s.delayed_.empty(),"engine refused screen offer schedules retry");
    auto old=s.screen_offer_generation_;auto timers=std::move(s.delayed_);s.delayed_.clear();
    // Only the generation is under test here: the slot is taken out of the
    // stop's reach, because a stopped slot is not offered again until a remote
    // answer settles it, and this connection has no server to answer.
    auto kept=s.screen_tr_;kept->sender()->SetTrack(nullptr);s.screen_tr_=nullptr;s.StopScreenNow("stopped");
    Check(s.screen_offer_generation_>old&&!kept->stopping(),"engine stop invalidates screen offer generation");
    s.screen_tr_=kept;s.screen_=s.factory_->CreateVideoTrack(source,"new-screen");s.screen_tr_->sender()->SetTrack(s.screen_.get());++s.screen_offer_generation_;
    for(auto& [at,task]:timers)task();Check(offers==1&&s.delayed_.empty(),"engine stale screen timer cannot offer replacement");
    auto offer=Create(s.pc_.get(),true);Check(!offer.empty()&&SetLocal(s.pc_.get(),webrtc::SdpType::kOffer,offer),"engine media has local SDP");
    s.send_mids_["screen"]=s.screen_tr_->mid().value_or("");s.layers_on_={"alta"};s.TuneScreen();
    auto params=s.screen_tr_->sender()->GetParameters();for(const auto& e:params.encodings){auto l=std::find_if(std::begin(kLayers),std::end(kLayers),[&](const auto& l){return e.rid==l.rid;});Check(l!=std::end(kLayers)&&e.max_framerate&&*e.max_framerate==l->fps&&e.scale_resolution_down_by&&*e.scale_resolution_down_by==l->scale&&e.active==(e.rid=="alta"),"engine screen parameters follow RID rather than position");}
    {
      auto capture=gpu::CaptureRegression::Freeze();
      struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {void OnFrame(const webrtc::VideoFrame&) override {}} preview;
      webrtc::VideoSinkWants wants;static_cast<webrtc::VideoTrackSourceInterface*>(capture.get())->AddOrUpdateSink(&preview,wants);
      auto original=s.screen_;s.screen_=s.factory_->CreateVideoTrack(capture,"W12-capture");s.screen_tr_->sender()->SetTrack(s.screen_.get());s.screen_offered_=true;
      auto nobody=json::Parse("{\"screen\":false}");s.ApplyWatchers(*nobody);
      // Execute the actual five-second shrink; before this it may still encode.
      auto tasks=std::move(s.delayed_);s.delayed_.clear();for(auto& [at,task]:tasks)task();
      Check(s.layers_on_.empty(),"W12 watcher shrink disables all encoding layers");
      int idle=0;for(int64_t us=1000000;us<2000000;us+=1000)idle+=gpu::CaptureRegression::Frame(capture.get(),us);
      HDC dc=GetDC(nullptr);int hz=std::clamp(GetDeviceCaps(dc,VREFRESH),1,30);ReleaseDC(nullptr,dc);
      Check(idle<=hz+1,"W12 no viewer admits no more frames than preview cadence before texture copy");
      auto viewer=json::Parse("{\"screen\":true,\"layers\":[\"alta\"]}");s.ApplyWatchers(*viewer);
      Check(s.layers_on_==std::set<std::string>{"alta"} && gpu::CaptureRegression::Frame(capture.get(),2000000),"W12 watcher restores capture before first encoded frame");
      int full=0;for(int i=1;i<=60;++i)full+=gpu::CaptureRegression::Frame(capture.get(),2000000+int64_t(i)*1000000/60);
      Check(full==60,"W12 viewer receives requested 60 fps source cadence");
      std::cout<<"W12 capture admitted: idle="<<idle<<"/s requested="<<full<<"/s\n";
      s.ApplyWatchers(*nobody);tasks=std::move(s.delayed_);s.delayed_.clear();for(auto& [at,task]:tasks)task();
      idle=0;for(int64_t us=4000000;us<5000000;us+=1000)idle+=gpu::CaptureRegression::Frame(capture.get(),us);
      Check(idle<=hz+1,"W12 last viewer leaves and pre-copy preview pacing returns");
      static_cast<webrtc::VideoTrackSourceInterface*>(capture.get())->RemoveSink(&preview);s.screen_=original;s.screen_tr_->sender()->SetTrack(original.get());
    }
    s.layers_on_={"alta","media","baixa"};s.TuneScreen();
    s.camera_=s.factory_->CreateVideoTrack(source,"bandwidth-camera");
    auto cameraAdded=s.pc_->AddTransceiver(s.camera_);
    Check(cameraAdded.ok(),"bandwidth camera is on in real engine peer connection");
    auto cameraSender=cameraAdded.value()->sender();
    auto cameraParams=cameraSender->GetParameters();cameraParams.encodings[0].max_bitrate_bps=900000;
    Check(cameraSender->SetParameters(cameraParams).ok(),"bandwidth camera ceiling accepted");
    RunBandwidthRegression(s.screen_tr_->sender()->GetParameters(),cameraSender->GetParameters());
    auto existingPc=s.pc_;auto previousCamera=s.camera_;
    auto replacement=webrtc::make_ref_counted<VideoSource>();cameraSource=replacement;
    s.SetCamera(true,"second-camera");
    Check(s.pc_==existingPc && s.camera_!=previousCamera && s.camera_->GetSource()!=previousCamera->GetSource() && s.camera_id_=="second-camera",
          "camera device switch replaces source without recreating peer connection");

    // Exercise the real replacement and late-audio paths; only capture and
    // WASAPI are doubled, while PeerConnection/transceivers are real.
    class AudioSource : public loopback::SystemAudioSource {public:AudioSource()=default;};
    systemAudio=webrtc::make_ref_counted<AudioSource>();
    s.screen_offered_=true;s.options_.share_audio=true;s.CommitScreen(source);
    Check(s.screen_audio_tr_&&s.screen_audio_&&s.screen_audio_tr_->sender()->track()==s.screen_audio_,"engine late sound creates missing transceiver");
    Check(!s.screen_offered_,"engine late sound renegotiates before publication");
    auto audioTransceiver=s.screen_audio_tr_;s.options_.share_audio=false;s.CommitScreen(source);
    Check(!s.screen_audio_&&!audioTransceiver->sender()->track(),"engine silent replacement removes audio track");
    s.options_.share_audio=true;s.CommitScreen(source);
    Check(s.screen_audio_tr_==audioTransceiver&&audioTransceiver->sender()->track(),"engine repeated sound reuses transceiver");
    auto previous=s.screen_;auto waiting=webrtc::make_ref_counted<VideoSource>();waiting->current=webrtc::MediaSourceInterface::kInitializing;screenSource=waiting;
    s.active_screen_options_=s.options_;s.options_.share_monitor=reinterpret_cast<HMONITOR>(1);s.ShareScreen();
    Check(s.screen_==previous&&s.pending_screen_source_==waiting,"engine opening replacement preserves prior track");
    waiting->current=webrtc::MediaSourceInterface::kEnded;s.wanted_=true;s.Tick();
    Check(s.screen_==previous&&!s.pending_screen_source_,"engine failed replacement retains prior track");
    waiting->current=webrtc::MediaSourceInterface::kInitializing;screenSource=waiting;s.options_.share_monitor=reinterpret_cast<HMONITOR>(1);s.ShareScreen();now+=3001;s.Tick();
    Check(s.screen_==previous&&!s.pending_screen_source_,"engine opening first frame deadline preserves prior track");
    // The server's offer may put the screen in its own slots (ApplyOffer follows
    // send_mids); stopping has to empty those too, or each share keeps its
    // capture and encoder alive until leaving (W10: no picture on the 16th).
    auto slot=s.pc_->AddTransceiver(webrtc::MediaType::VIDEO);auto soundSlot=s.pc_->AddTransceiver(webrtc::MediaType::AUDIO);
    Check(slot.ok()&&soundSlot.ok()&&s.screen_audio_,"engine adds server-style screen slots");
    slot.value()->sender()->SetTrack(s.screen_.get());soundSlot.value()->sender()->SetTrack(s.screen_audio_.get());
    auto ownScreen=s.screen_tr_;auto ownSound=s.screen_audio_tr_;
    s.StopScreenNow("stopped");
    Check(!s.screen_&&!slot.value()->sender()->track()&&!soundSlot.value()->sender()->track()&&!ownScreen->sender()->track()&&!ownSound->sender()->track(),
          "engine stopped screen leaves no sender holding its picture or sound");
    // The server stops those slots on unpublish; the next share must offer its
    // own three layers instead of reusing a slot that will never send again.
    Check(!s.screen_tr_&&!s.screen_audio_tr_&&!s.screen_offered_,"engine stopped screen forgets the slots the server closes");
    Check((ownScreen->stopping()||ownScreen->stopped())&&(slot.value()->stopping()||slot.value()->stopped())&&(soundSlot.value()->stopping()||soundSlot.value()->stopped()),
          "engine stopped screen ends every slot that carried it, freeing its encoder");
    Check(s.layers_on_==std::set<std::string>{"alta","media","baixa"},"engine next share starts with every layer on");
    auto armed=s.delayed_.size();auto nobody=json::Parse("{\"screen\":false}");s.ApplyWatchers(*nobody);
    Check(s.delayed_.size()==armed&&s.layers_on_==std::set<std::string>{"alta","media","baixa"},"engine late nobody for a stopped screen arms no switch-off for the next");
    int layered=0;post=[&](const std::string& path,const json::Value&){if(path=="/api/voice/offer")++layered;return net::Response{409,"{\"error\":\"reconnect_needed\"}"};};
    std::vector<Update> limit;s.options_.changed=[&](Update u){limit.push_back(u);};
    if(s.pc_->signaling_state()!=webrtc::PeerConnectionInterface::kStable)SetLocal(s.pc_.get(),webrtc::SdpType::kRollback,"");
    auto waitingTasks=s.delayed_.size();s.CommitScreen(source);
    Check(layered==1&&!s.screen_&&s.delayed_.size()==waitingTasks&&!limit.empty()&&limit.back().reason=="reconnect_needed",
          "engine next share offers layers and stops at once on the server's slot limit");
    screenSource=source;s.options_.share_monitor=reinterpret_cast<HMONITOR>(1);s.ShareScreen();
    Check(layered==1&&!s.pending_screen_source_&&!limit.empty()&&limit.back().reason=="reconnect_needed","engine share past the limit says so without offering again");
    s.options_.changed=nullptr;
    std::string layer;post=[&](const std::string& path,const json::Value& body){if(path=="/api/voice/layer")layer=body["layer"].Str();return net::Response{204,"{}"};};
    s.options_.watch="viewer";s.layer_request_.desired=0;s.tile_layers_["viewer"].desired=2;s.tile_layers_["viewer"].decode.cap=1;s.Tick();
    Check(layer=="media","engine fullscreen respects decoder ceiling");
    s.options_.watch.clear();s.VideoSize("viewer",1000,1,false);now+=400;s.Tick();
    Check(layer=="baixa","engine hidden tile requests lowest layer");
    std::vector<Update> mediaUpdates;s.options_.changed=[&](Update update){mediaUpdates.push_back(update);};
    cameraSource=source;s.SetCamera(true,"accented-camera");
    Check(s.camera_&&s.camera_wanted_,"engine camera enabled records restoration intent");
    s.CloseMedia();Check(!s.camera_&&s.camera_wanted_,"engine recreation preserves camera intent");
    Check(s.CreateConnection(json::Value::Array()),"engine recreation creates replacement connection");
    s.RestoreOutgoingMedia();Check(s.camera_&&s.camera_id_=="accented-camera","engine recreation restores camera track");
    s.camera_failed_=true;s.SetCamera(false,s.camera_id_);
    Check(!mediaUpdates.empty()&&mediaUpdates.back().reason=="camera_unavailable","engine failed camera notice survives disabling");
    s.StopScreenNow("stopped");s.Leave();post={};screenSource=nullptr;cameraSource=nullptr;systemAudio=nullptr;
  }
  static void LayerRetries() {
    using namespace engine_test;
    for(bool tile:{false,true}) {
      net::Client client("http://127.0.0.1:8103");Options options;options.simulated_device=true;options.mic=false;options.watch="viewer";
      Session s(&client,options);
      Check(s.CreateFactory()&&s.CreateConnection(json::Value::Array()),"layer retry creates simulated engine");
      if(tile)s.tile_layers_["viewer"].desired=2;
      int posts=0;bool success=false;
      post=[&](const std::string& path,const json::Value& body) {
        if(path=="/api/voice/layer") {++posts;Check(!body["session_id"].Str().empty(),"layer request has a session");return net::Response{success?204:503,""};}
        return net::Response{204,""};
      };
      s.wanted_=true;
      for(int i=0;i<80;++i){now+=25;s.Tick();}
      Check(posts==0,"layer requests wait for a nonempty session");
      s.session_id_="layer-session";
      auto start=now;
      for(;now<start+2000;now+=25)s.Tick();
      Check(posts>0&&posts<=4,"layer failures make at most four POSTs in two simulated seconds");
      auto failed=posts;success=true;
      for(int i=0;i<240;++i){now+=25;s.Tick();}
      Check(posts==failed+1,"layer recovery sends once then stops");
      std::cout<<"Layer retry "<<(tile?"tile":"watch")<<": "<<failed<<" failed POSTs / 2s; "<<posts-failed<<" recovery POST / 6s\n";
      s.wanted_=false;s.delayed_.clear();post={};
    }
  }
  static void Run() {
    // No audio device: the pipeline sentinel observes the actual destruction thread.
    DWORD releaseThread = 0, workerThread = 0;
    {
      net::Client client("http://127.0.0.1:8103"); Session session(&client, {});
      session.worker_thread_ = webrtc::Thread::Create(); session.worker_thread_->Start();
      workerThread = session.worker_thread_->BlockingCall([] { return GetCurrentThreadId(); });
      session.audio_.captureCalls = std::shared_ptr<std::atomic<uint64_t>>(new std::atomic<uint64_t>(0),
        [&](auto* value) { releaseThread = GetCurrentThreadId(); delete value; });
    }
    Check(releaseThread == workerThread && releaseThread != GetCurrentThreadId(), "engine releases the audio pipeline on its worker before destroying the thread");
    LayerRetries();
    CaptureOpening();
    Media();
    {net::Client client("http://127.0.0.1:8103");Options options;options.watch="viewer";Session s(&client,options);
     s.tracks_["screen"]={"viewer","screen","video"};s.tile_layers_["viewer"].desired=0;
     constexpr int64_t start=1000000; // The PC has been up for much longer than the grace period.
     s.ObserveDecode("screen",{0,0},start);
     for(int i=1;i<=5;++i)s.ObserveDecode("screen",{0,uint64_t(i*21)},start+i*1000);
     Check(s.tile_layers_["viewer"].decode.cap==0,"W12 startup keyframe gaps do not lower full-screen decode quality");
     s.ObserveDecode("screen",{100,105},start+21000);
     for(int i=1;i<=3;++i)s.ObserveDecode("screen",{uint64_t(100+i*5),uint64_t(105+i*21)},start+21000+i*1000);
     Check(s.tile_layers_["viewer"].decode.cap==1&&s.layer_request_.decode.cap==1,"W12 sustained loss after startup still lowers both tile and large-window quality");
     s.ObserveDecode("screen",{0,0},start+25000);
     for(int i=1;i<=3;++i)s.ObserveDecode("screen",{0,uint64_t(i*21)},start+25000+i*1000);
     Check(s.tile_layers_["viewer"].decode.cap==1,"W12 restarted receiver counters get a fresh grace period without losing the existing cap");}
    {net::Client client("http://127.0.0.1:8103");Options options;options.username="self";Session s(&client,options);
     s.audio_.audience=std::make_shared<std::atomic<bool>>(true);
     s.UpdateAudience();Check(!s.audio_.audience->load(),"W12 empty offer has no audience");
     s.tracks_["0"]={"self","mic","audio"};s.UpdateAudience();Check(!s.audio_.audience->load(),"W12 own track is not audience");
     s.tracks_["1"]={"listener","mic","audio"};s.UpdateAudience();Check(s.audio_.audience->load(),"W12 silent remote track counts as audience");
     s.tracks_.clear();s.UpdateAudience();Check(!s.audio_.audience->load(),"W12 replacement offer clears departed audience");}

    // The test cannot fall back to headphones, even when the ADM is asked
    // to select its default output or the CABLE disappears after opening.
    class CableADM : public FakeADM {
     public:bool cable=false;
      int32_t PlayoutDeviceName(uint16_t,char n[webrtc::kAdmMaxDeviceNameSize],char g[webrtc::kAdmMaxGuidSize])override {
        strcpy_s(n,webrtc::kAdmMaxDeviceNameSize,cable?"CABLE Input (VB-Audio Virtual Cable)":"Headphones");strcpy_s(g,webrtc::kAdmMaxGuidSize,"output-a");return 0;
      }
    };
    // This checks endpoint selection only. ADM2's stream category is a separate
    // policy; mapping the default role alone does not prove absence of ducking.
    {auto plain=webrtc::make_ref_counted<FakeADM>();auto wrapped=webrtc::make_ref_counted<audio::Device>(plain,std::unique_ptr<audio::Transport>{});
     wrapped->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice);wrapped->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice);
     Check(plain->lastPlayType==int(webrtc::AudioDeviceModule::kDefaultDevice)&&plain->lastRecordType==int(webrtc::AudioDeviceModule::kDefaultDevice),"engine call maps the communications default to the ordinary device");
     // The output stream's category is what lowers the other sounds (W12).
     Check(audio::StreamCategory(AudioCategory_Communications,false)==AudioCategory_Other&&audio::StreamCategory(AudioCategory_Communications,true)==AudioCategory_Communications&&audio::StreamCategory(AudioCategory_Media,false)==AudioCategory_Media,"only a communications output stream is remarked, so other sounds stay up");}
    {class Watch : public FakeADM { public: bool microphone=false,output=false;
       int32_t InitRecording() override { microphone=audio::OpeningMicrophone(); return FakeADM::InitRecording(); }
       int32_t InitPlayout() override { output=audio::OpeningMicrophone(); return FakeADM::InitPlayout(); } };
     auto watch=webrtc::make_ref_counted<Watch>();auto wrapped=webrtc::make_ref_counted<audio::Device>(watch,std::unique_ptr<audio::Transport>{});
     wrapped->InitRecording();wrapped->InitPlayout();
     Check(watch->microphone&&!watch->output&&!audio::OpeningMicrophone(),"the module opens the microphone knowing it is the microphone, and only then");}
    // The Core Audio module opens every microphone in its own format (the
    // owner's Fifine is 192 kHz) and lists two pseudo-devices first, which the
    // wrapper hides so the device lists keep naming real devices only.
    if(GetEnvironmentVariableW(L"HARMONY_TEST_NO_DEVICES",nullptr,0)==0) {auto core=webrtc::CreateWindowsCoreAudioAudioDeviceModule(webrtc::CreateEnvironment(),true);
     Check(core!=nullptr&&core->Init()==0,"engine Core Audio module starts");
     auto device=webrtc::make_ref_counted<audio::Device>(core,std::unique_ptr<audio::Transport>{},false,2);
     bool real=device->RecordingDevices()==std::max(0,core->RecordingDevices()-2)&&device->PlayoutDevices()==std::max(0,core->PlayoutDevices()-2);
     auto named=[&](bool capture,int16_t count){for(int16_t i=0;i<count;++i){char n[webrtc::kAdmMaxDeviceNameSize]{},g[webrtc::kAdmMaxGuidSize]{};
       capture?device->RecordingDeviceName(uint16_t(i),n,g):device->PlayoutDeviceName(uint16_t(i),n,g);
       std::string name=n;real=real&&name.rfind("Default - ",0)!=0&&name.rfind("Communication - ",0)!=0;}};
     named(true,device->RecordingDevices());named(false,device->PlayoutDevices());
     Check(real,"engine device lists name real devices only");
     Check(!device->BuiltInAECIsAvailable(),"engine capture never uses the 16 kHz built-in Windows effects");
     if(device->RecordingDevices()>0)Check(device->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice)==0&&device->InitRecording()==0,"engine the default microphone opens whatever its Windows format");
     device->StopRecording();core->Terminate();}
    else std::cout<<"SKIP: physical Core Audio device test (HARMONY_TEST_NO_DEVICES)\n";
     {auto mixer=webrtc::make_ref_counted<audio::ReceiveMixer>();std::vector<uint32_t> told;mixer->OnUnknown([&](uint32_t s){told.push_back(s);});
      mixer->Gains({{1,0.5},{2,0}},1);
      Check(mixer->Gain(1)==0.5&&mixer->Gain(2)==0,"engine routed and unnamed receivers keep their gains");
      Check(mixer->Gain(3)==1&&mixer->Gain(3)==1&&told==std::vector<uint32_t>{3},"engine a rejoined friend's unrouted SSRC plays and asks once for new routes");
      mixer->Gains({},0);Check(mixer->Gain(4)==0,"engine an unrouted SSRC stays silent while deafened");}
    auto cableAdm=webrtc::make_ref_counted<CableADM>();auto cableDevice=webrtc::make_ref_counted<audio::Device>(cableAdm,std::unique_ptr<audio::Transport>{},true);
    Check(cableDevice->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice)!=0&&cableAdm->playDefaults==0,"engine test output never selects default device");
    Check(cableDevice->SetPlayoutDevice(uint16_t(0))!=0,"engine test output rejects headphones index");
    Check(cableDevice->InitPlayout()!=0&&cableDevice->StartPlayout()!=0&&cableAdm->playStarts==0,"engine absent CABLE never starts playback");
    cableAdm->cable=true;Check(cableDevice->InitPlayout()==0&&cableDevice->StartPlayout()==0&&cableAdm->playStarts==1,"engine test output opens only CABLE");
    cableDevice->StopPlayout();cableAdm->cable=false;Check(cableDevice->InitPlayout()!=0&&cableDevice->StartPlayout()!=0&&cableAdm->playStarts==1,"engine removed CABLE cannot resume playback");
    {net::Client local("http://127.0.0.1:8103");Options required;required.simulated_device=true;required.test_require_cable=true;Session isolated(&local,required);
     Check(isolated.CreateFactory()&&isolated.audio_.device->InitPlayout()!=0,"engine CABLE requirement reaches factory");isolated.Leave();}
    using namespace engine_test;
    net::Client client("http://127.0.0.1:8103");
    std::vector<Update> updates;
    Options options; options.room_id="same-room"; options.changed=[&](Update u) { updates.push_back(u); };
    Session s(&client,options); s.session_id_="same-session"; s.room_id_="same-room";
    s.worker_thread_=webrtc::Thread::Create(); s.worker_thread_->Start();
    auto adm=webrtc::make_ref_counted<FakeADM>(); s.audio_.device=adm;
    s.audio_.captureCalls=std::make_shared<std::atomic<uint64_t>>(0);
    s.audio_.renderCalls=std::make_shared<std::atomic<uint64_t>>(0);
    auto apm=webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
    auto mic=webrtc::make_ref_counted<audio::MicSource>();
    SilentRender render;
    auto level=std::make_shared<std::atomic<double>>(0); auto at=std::make_shared<std::atomic<uint64_t>>(0);
    audio::Transport transport(apm.get(),mic.get(),std::make_shared<audio::FilterSlot>(),level,at,s.audio_.renderCalls,true);
    transport.capture_calls_=s.audio_.captureCalls;
    transport.reference_missing_=std::make_shared<std::atomic<uint64_t>>(0); transport.reference_dropped_=std::make_shared<std::atomic<uint64_t>>(0); transport.reference_age_=std::make_shared<std::atomic<uint64_t>>(0);
    transport.SetEngine(&render); adm->RegisterAudioCallback(&transport);
    auto track=webrtc::make_ref_counted<FakeTrack>(); s.mic_=track;
    json::Value announced; int requests=0;
    post=[&](const std::string& path,const json::Value& body) {
      ++requests; announced=body; return net::Response{200,"{\"ice_restart\":true}"};
    };
    s.audio_.filter=std::make_shared<audio::FilterSlot>();
    s.SetMicFilters(false,false,false);
    Check(!s.audio_.filter->echo && !s.audio_.filter->noise && !s.audio_.filter->agc,"engine microphone preferences reach capture slot");
    s.SetNoise(false,73); Check(!s.audio_.filter->enabled && s.audio_.filter->strength==73,"engine DFN preferences reach capture slot");
    s.SetNoise(true,200); Check(s.audio_.filter->enabled && s.audio_.filter->strength==100,"engine DFN strength clamped at capture slot");
    s.SetMute(true,false); Check(!track->enabled() && announced["muted"].Bool(),"engine mute disables track and announces state");
    s.SetMute(false,true); Check(!track->enabled() && announced["deafened"].Bool(),"engine deafen disables microphone");
    s.SetMute(false,false); Check(track->enabled(),"engine unmute restores track");
    s.inputs_={{"device-a","A"}}; s.outputs_=s.inputs_;
    s.SetDevice(true,"device-a"); s.SetDevice(false,"device-a");
    Check(s.options_.input=="device-a" && s.options_.output=="device-a" && adm->recordStarts==1 && adm->playStarts==1,"engine opens explicitly selected devices");
    adm->Pulse();
    s.CheckDeviceRecovery(true); s.CheckDeviceRecovery(false);
    // Unplug A, with the first default opening failing. The stopped ADM must
    // not erase either intent; unchanged enumeration must still retry.
    adm->present=false; adm->failRecord=1; adm->failPlay=1; s.inputs_.clear(); s.outputs_.clear();
    s.SetDevice(true,"device-a"); s.SetDevice(false,"device-a");
    Check(!adm->Recording() && !adm->Playing(),"engine failed fallback stops device");
    Check(s.capture_recovery_.pending && !s.capture_recovery_.opened,"engine rejects success zero");
    now+=600; s.CheckDeviceRecovery(true); s.CheckDeviceRecovery(false);
    Check(adm->recordStarts==2 && adm->playStarts==2 && adm->recordDefaults==2 && adm->playDefaults==2,"engine retries and restarts both intents after failure");
    Check(s.options_.input.empty() && s.options_.output.empty() && s.session_id()=="same-session","engine fallback opens default in same session");
    s.CheckDeviceRecovery(true); s.CheckDeviceRecovery(false);
    Check(s.capture_recovery_.pending && s.playout_recovery_.pending,"engine opening is not recovery without callbacks");
    adm->Pulse();
    s.CheckDeviceRecovery(true); s.CheckDeviceRecovery(false);
    Check(!s.capture_recovery_.pending && !s.playout_recovery_.pending && updates.back().reason=="recovered","engine callbacks confirm device recovery");
    Check(*s.audio_.captureCalls==2 && *s.audio_.renderCalls==2,"engine default swap grows actual Transport callbacks");
    app::VoiceState beforeDevice,afterDevice;
    beforeDevice.input=beforeDevice.output="device-a"; beforeDevice.inputReady=beforeDevice.outputReady=true;
    beforeDevice.captureCallbacks=beforeDevice.renderCallbacks=1; afterDevice=beforeDevice;
    afterDevice.input=s.options_.input; afterDevice.output=s.options_.output;
    afterDevice.captureCallbacks=*s.audio_.captureCalls; afterDevice.renderCallbacks=unsigned(*s.audio_.renderCalls);
    Check(app::SelectedDeviceReady(beforeDevice,"device-a","device-a") && app::DefaultDeviceRecovered(beforeDevice,afterDevice),"engine selftest accepts selected A to flowing default B");
    afterDevice.inputReady=false;
    Check(!app::DefaultDeviceRecovered(beforeDevice,afterDevice),"engine selftest rejects success zero");
    afterDevice.inputReady=true; afterDevice.captureCallbacks=beforeDevice.captureCallbacks;
    Check(!app::DefaultDeviceRecovered(beforeDevice,afterDevice),"engine selftest rejects stalled capture after swap");
    afterDevice.captureCallbacks=2; afterDevice.renderCallbacks=beforeDevice.renderCallbacks;
    Check(!app::DefaultDeviceRecovered(beforeDevice,afterDevice),"engine selftest rejects stalled render after swap");
    std::array<int16_t,960> a,b; a.fill(100); b.fill(200);
    std::vector<int16_t> observed;
    reverse=[&](auto& pcm) { observed.push_back(pcm.front()); };
    transport.ReferenceForTest(a.data()); transport.ReferenceForTest(b.data());
    adm->Pulse(); adm->Pulse(); adm->Pulse();
    Check(observed==std::vector<int16_t>{100,200,0},"engine APM reverse input sees A B silence through Transport");
    reverse={};
    // A permanent failure has a finite device retry budget and increasing delay.
    adm->failRecord=100; s.SetDevice(true,"");
    for(int i=0;i<20;++i) { now+=5000; s.CheckDeviceRecovery(true); }
    Check(s.capture_recovery_.attempts==5,"engine device retries are bounded");
    Session::Meter meter; int16_t pcm[512]{};
    // 128 of 512 samples at +-128 is RMS 128/32768/2 (about -54 dBFS): a quiet sound.
    for(int i=0;i<128;++i) pcm[i]=(i%2 ? -128 : 128);
    meter.OnData(pcm,16,48000,1,512);
    Check(std::abs(meter.Read(now)-128.0/32768/2)<1e-9,"engine meter is linear RMS over 512 samples");
    memset(pcm,0,sizeof(pcm)); meter.OnData(pcm,16,48000,1,512);
    Check(meter.Read(now)==0,"engine meter silence clears window");
    app::VoiceState ring; ring.Join("same-room","Same room");
    for(int i=0;i<128;++i) pcm[i]=(i%2 ? -32 : 32);
    meter.OnData(pcm,16,48000,1,512);
    Check(meter.Read(now)<0.001 && !ring.Level("speaker",meter.Read(now),now),"engine PCM below -60 dBFS does not light ring");
    for(int i=0;i<128;++i) pcm[i]=(i%2 ? -128 : 128);
    meter.OnData(pcm,16,48000,1,512); ring.Level("speaker",meter.Read(now),now);
    Check(ring.speaking["speaker"] && !ring.Level("speaker",0,now+299) && ring.Level("speaker",0,now+300) && !ring.speaking["speaker"],"engine PCM ring lights on a quiet sound and releases at web 300 ms");
    // Losing connectivity during HTTP must not turn a 404 into an offline rejoin.
    post=[&](const std::string&,const json::Value&) { ++requests; online=false; return net::Response{404,"{\"error\":\"not_found\"}"}; };
    s.Recover("network");
    Check(joins==0 && s.wanted_,"engine network loss during HTTP does not spend rejoin budget");
    post=[&](const std::string&,const json::Value&) { ++requests; return net::Response{200,"{\"ice_restart\":true}"}; };
    // Network path: no POST/rejoin budget while offline, even beyond all windows.
    online=false; int before=requests; s.Recover("network"); auto old=s.recovery_generation_; auto outage=s.outage_at_;
    for(int i=0;i<30;++i) { now+=10000; s.RestartAttempt(old); }
    Check(requests==before && joins==0 && s.wanted_ && s.room_id_=="same-room","engine offline consumes no attempts and retains room");
    online=true; s.Recover("network");
    Check(requests==before+1 && offers==1 && s.recovery_generation_>old && s.outage_at_==outage && s.recovery_at_==now,"engine online renews window and retains total outage");
    before=requests; auto timers=s.delayed_.size(); s.RestartAttempt(old);
    Check(requests==before && s.delayed_.size()==timers,"engine old network timer is invalidated");
    std::vector<std::function<void()>> due;
    for(auto it=s.delayed_.begin();it!=s.delayed_.end() && it->first<=now;) { due.push_back(std::move(it->second)); it=s.delayed_.erase(it); }
    for(auto& task:due) task();
    Check(requests==before && joins==0,"engine queued offline timers do not resurrect old generation");
    // Resume path: advance clock through sleep during an outstanding restart.
    old=s.recovery_generation_; now+=3600000; s.Recover("resume"); before=requests; timers=s.delayed_.size();
    s.RestartAttempt(old);
    Check(requests==before && s.delayed_.size()==timers && joins==0 && s.recovery_at_==now && s.outage_at_==outage,"engine resume invalidates expired timer and renews window");
    due.clear();
    for(auto it=s.delayed_.begin();it!=s.delayed_.end() && it->first<=now;) { due.push_back(std::move(it->second)); it=s.delayed_.erase(it); }
    for(auto& task:due) task();
    Check(requests==before && joins==0,"engine queued pre-sleep timers are invalidated");
    // Server path: a missing SFU session actually invokes Join rather than ICE.
    post=[&](const std::string&,const json::Value&) { ++requests; return net::Response{404,"{\"error\":\"not_found\"}"}; };
    s.restart_applied_=false; s.RestartAttempt(s.recovery_generation_);
    Check(joins==1 && s.wanted_ && s.room_id_=="same-room","engine missing server session rejoins same room");
    s.Leave(); before=requests; s.RestartAttempt(s.recovery_generation_-1);
    Check(requests==before && !s.wanted_,"engine leave cancels late restart");
    adm->RegisterAudioCallback(nullptr);
  }
};
}
// The Diagnóstico recording: armed, it waits for a listener, writes the AEC
// dump, the sent audio and a line a second, disarms, and keeps two folders.
static void RunCallRecording() {
  auto root=std::filesystem::absolute("test-output/recording");std::error_code error;std::filesystem::remove_all(root,error);
  webrtc::AudioProcessing::Config config;config.echo_canceller.enabled=true;
  auto apm=webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
  auto mic=webrtc::make_ref_counted<audio::MicSource>();
  auto slot=std::make_shared<audio::FilterSlot>();slot->enabled=false;
  auto counter=std::make_shared<std::atomic<uint64_t>>(0);
  auto audience=std::make_shared<std::atomic<bool>>(false);
  // Stereo 48 kHz, a 400 Hz tone with three clipped samples per quantum.
  std::array<int16_t,960> pcm{};uint32_t level=0;
  for(size_t i=0;i<480;++i)pcm[i*2]=pcm[i*2+1]=int16_t(8000*std::sin(2*3.14159265*400*i/48000.));
  pcm[10]=32767;pcm[11]=32767;pcm[100]=-32768;
  auto saved=[&](int want){
    for(int i=0;i<300;++i){auto s=audio::ReadRecording();if(s.state==audio::RecordingStatus::Saved&&s.seconds==want)return s;Sleep(10);}
    return audio::ReadRecording();
  };
  auto run=[&](int quanta,std::function<void(int)> each={}){
    audio::Transport transport(apm.get(),mic.get(),slot,std::make_shared<std::atomic<double>>(0),counter,counter,false,false,audience);
    transport.capture_calls_=counter;
    // Real time, roughly: the files open on the recording queue, not here.
    for(int i=0;i<quanta;++i){transport.RecordedDataIsAvailable(pcm.data(),480,4,2,48000,30,0,0,false,level);if(each)each(i);Sleep(2);}
  };
  audio::ArmRecording(root,1);
  run(50);
  Check(audio::ReadRecording().state==audio::RecordingStatus::Armed,"recording waits while nobody else is in the call");
  *audience=true;
  std::filesystem::path first;
  for(int round=0;round<3;++round){
    if(round)audio::ArmRecording(root,1);
    run(150);
    auto s=saved(1);
    Check(s.state==audio::RecordingStatus::Saved&&s.seconds==1,"recording saves itself after its seconds and disarms");
    if(!round)first=s.folder;
    Check(std::filesystem::file_size(s.folder/L"eco.aecdump",error)>10000,"recording writes the AEC dump");
    Check(std::filesystem::file_size(s.folder/L"enviado.wav",error)==44+100*480*2,"recording writes one second of the sent microphone");
    std::ifstream numbers(s.folder/L"numeros.txt");std::string text((std::istreambuf_iterator<char>(numbers)),{});
    Check(text.find("microfone 48000 Hz 2 canais")!=std::string::npos&&text.find(" 300 | ")!=std::string::npos,"recording counts clipped device samples each second");
  }
  size_t folders=0;for(auto& e:std::filesystem::directory_iterator(root))folders+=e.is_directory();
  Check(folders==2&&!std::filesystem::exists(first),"recording keeps only the two newest folders");
  audio::ArmRecording(root,60);
  run(120,[](int i){if(i==60)audio::DisarmRecording();});
  auto s=saved(0);
  auto wav=std::filesystem::file_size(s.folder/L"enviado.wav",error);
  Check(s.state==audio::RecordingStatus::Saved&&wav>44&&wav<=44+62*480*2&&(wav-44)%960==0,
        "switching the recording off mid-call stops and keeps what it has");
  audio::RecordStats(*webrtc::RTCStatsReport::Create(webrtc::Timestamp::Zero()));
  std::cout<<"Call recording: AEC dump + sent WAV + numbers, waits for a listener, keeps two, stops when switched off\n";
}
// "Ouvir minha voz" on the simulated device: what plays is the filtered
// microphone with the gain slider, it follows the slider live, and the
// canceller never gets it as a reference (it would cancel the voice).
static void RunVoiceTest() {
  struct Mark : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& block,int,size_t,int) override { std::fill(block.begin(),block.end(),int16_t(4321)); return true; }
  };
  std::mutex mu; bool voiceInReference=false; size_t references=0;
  engine_test::reverse=[&](const std::vector<int16_t>& r) {
    std::lock_guard lock(mu); ++references; for(auto v:r) if(v) voiceInReference=true;
  };
  audio::MonitorSettings settings; settings.simulatedDevice=true; settings.backend=std::make_unique<Mark>(); settings.agc=false;
  {
    audio::Monitor monitor(std::move(settings));
    Check(monitor.Running(),"voice test opens the simulated microphone and output");
    auto wait=[&](uint64_t played) { auto deadline=GetTickCount64()+3000; while(monitor.Played()<played && GetTickCount64()<deadline) Sleep(10); };
    wait(40); monitor.TakePeak(); auto from=monitor.Played(); wait(from+20);
    Check(monitor.Played()>=from+20 && monitor.TakePeak()==4321,"voice test plays the filtered microphone back");
    monitor.Filters(true,50,true,true,false,0.5);
    from=monitor.Played(); wait(from+10); monitor.TakePeak(); from=monitor.Played(); wait(from+20);
    Check(monitor.TakePeak()==2160,"voice test follows the gain slider live");
    std::lock_guard lock(mu);
    Check(references>20 && !voiceInReference,"voice test never hands its playback to the echo canceller");
  }
  engine_test::reverse=nullptr;
  std::cout<<"Voice test: filtered microphone plays back, follows the slider, stays out of the echo reference\n";
}
// "Ouvir minha voz" in a call: the filtered microphone joins the room's sound
// on its way to the output, never the echo canceller's reference, and goes
// silent when the pipeline's owner turns it off (muting does).
static void RunSidetone() {
  struct Mark : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& block,int,size_t,int) override { std::fill(block.begin(),block.end(),int16_t(4321)); return true; }
  };
  struct Room : webrtc::AudioTransport {
    int32_t RecordedDataIsAvailable(const void*,size_t,size_t,size_t,uint32_t,uint32_t,int32_t,uint32_t,bool,uint32_t&) override { return 0; }
    int32_t NeedMorePlayData(size_t frames,size_t,size_t channels,uint32_t,void* samples,size_t& out,int64_t*,int64_t*) override {
      std::fill_n(static_cast<int16_t*>(samples),frames*channels,int16_t(100)); out=frames; return 0;
    }
    void PullRenderData(int,int,size_t,size_t,void*,int64_t*,int64_t*) override {}
  } room;
  std::mutex mu; int loudest=0; bool voiceInReference=false;
  engine_test::reverse=[&](const std::vector<int16_t>& r) { std::lock_guard lock(mu); for(auto v:r) if(v>100) voiceInReference=true; };
  engine_test::rendered=[&](const int16_t* pcm,size_t n) { std::lock_guard lock(mu); for(size_t i=0;i<n;++i) loudest=std::max(loudest,int(pcm[i])); };
  auto peak=[&] { std::lock_guard lock(mu); return std::exchange(loudest,0); };
  {
    auto p=audio::CreatePipeline(false,std::make_unique<Mark>(),true,false);
    Check(p.device && p.sidetone,"sidetone pipeline opens the simulated devices");
    p.filter->agc=false;
    p.device->RegisterAudioCallback(&room);
    Check(!p.device->Init() && !p.device->InitRecording() && !p.device->StartRecording() && !p.device->InitPlayout() && !p.device->StartPlayout(),"sidetone devices start");
    Sleep(500); peak(); Sleep(200);
    Check(peak()==100 && p.sidetonePlayed->load()==0,"without the switch only the room plays");
    p.sidetone->store(true);
    auto deadline=GetTickCount64()+3000; while(p.sidetonePlayed->load()<30 && GetTickCount64()<deadline) Sleep(10);
    peak(); Sleep(200);
    Check(p.sidetonePlayed->load()>=30 && peak()==100+4321,"the switch mixes the filtered microphone into the room");
    p.sidetone->store(false); Sleep(50); peak(); Sleep(200);
    Check(peak()==100,"turning it off (or muting) silences the voice");
    { std::lock_guard lock(mu); Check(!voiceInReference,"the sidetone never reaches the echo canceller's reference"); }
    p.device->StopRecording(); p.device->StopPlayout(); p.device->RegisterAudioCallback(nullptr); p.device->Terminate();
  }
  engine_test::reverse=nullptr; engine_test::rendered=nullptr;
  std::cout<<"Sidetone: own voice joins the room's sound, off when muted, out of the echo reference\n";
}
int main(int argc,char** argv) {
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  // The real microphone and Core Audio, played only into CABLE Input: never
  // into the owner's headphones.
  if(argc>1&&std::string(argv[1])=="--voice-test-cable") {
    audio::MonitorSettings settings; settings.requireCable=true;
    audio::Monitor monitor(std::move(settings));
    Check(monitor.Running(),"voice test opens the real microphone and CABLE Input");
    Sleep(10000);
    std::cout<<"voice test on the real device: "<<monitor.Played()<<" quanta played in 10 s, peak "<<monitor.TakePeak()<<", gaps "<<monitor.Gaps()<<"\n";
    Check(monitor.Gaps()<=2,"voice test plays without dropping or running dry");
    Check(monitor.Played()>900,"voice test plays the real microphone back");
    CoUninitialize(); return 0;
  }
  RunMediaTests(Check);
  if(argc>1&&std::string(argv[1]).starts_with("--round6")){voice::EngineRegression::ScreenCamera(std::string(argv[1])!="--round6-vp8",std::string(argv[1])=="--round6-camera-first");CoUninitialize();return 0;}
  RunCaptureFormats();
  RunRealCaptureFormats();
  RunSuppressionRegression();
  RunCallRecording();
  RunVoiceTest();
  RunSidetone();
  voice::EngineRegression::Run();
  audio::ExternalReference reference; std::array<int16_t,960> a,b; a.fill(100); b.fill(200);
  reference.OnData(a.data(),16,48000,2,480); reference.OnData(b.data(),16,48000,2,480);
  Check(reference.Read()==a && reference.Read()==b,"engine external reference consumes A then B exactly once");
  Check(reference.Read()==std::array<int16_t,960>{} && reference.missing==1,"engine external reference shortage is counted silence");
  for(int i=0;i<10;++i) { a.fill(int16_t(i)); reference.OnData(a.data(),16,48000,2,480); }
  Check(reference.dropped==2 && reference.Read()[0]==2,"engine external reference excess drops oldest and counts");
  engine_test::now+=80;
  Check(reference.Read()==std::array<int16_t,960>{} && reference.dropped==9 && reference.missing==2,"engine reference alignment discards expired oldest blocks");
  std::cout<<checks<<" engine checks passed\n"; CoUninitialize(); return 0;
}
