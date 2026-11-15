#include "../src/voice-state.h"
#include "../../native/src/speaking-level.h"
#include <limits>
#include <iostream>
void RunSpeakingTests(void (*check)(bool, const char*)) {
  app::VoiceState v; v.Join("room", "Room");
  check(!v.Level("quiet",0.001,100),"speaking excludes exact -60 dBFS");
  check(v.Level("quiet",0.0011,100)&&v.speaking["quiet"],"speaking lights quiet audio above -60 dBFS");
  check(!v.Level("quiet",std::numeric_limits<double>::infinity(),399)&&v.speaking["quiet"],"speaking holds 299 ms without accepting infinity");
  check(v.Level("quiet",std::numeric_limits<double>::quiet_NaN(),400)&&!v.speaking["quiet"],"speaking releases at 300 ms without accepting NaN");
  v.Level("me",0.002,500,true);v.muted=true;
  check(v.Level("me",0.002,501,true)&&!v.speaking["me"],"speaking clears previously active self immediately on mute");
  v.muted=false;v.Level("me",0.002,600,true);v.deafened=true;
  check(v.Level("me",0.002,601,true)&&!v.speaking["me"],"speaking clears previously active self immediately on deafen");
  v.Level("remote",0.0011,700);
  check(v.speaking["remote"]&&v.Volume("remote")==1,"speaking quiet remote is independent of playback gain");
  v.Volume("remote",0);v.Level("remote",0.0011,800);
  check(v.speaking["remote"],"speaking still measures before zero playback gain");
  {
    app::VoiceState reference;reference.Join("room","Room");voice::SpeakingLevel gate;
    bool actual=false;int emitted=0;
    for(int64_t now=1000;now<5000;now+=25) {
      double level=now<1300?0.0011:now<1700?0:now<2100?0.002:now<2500?std::numeric_limits<double>::quiet_NaN():now<3500?0.001:0.03;
      reference.muted=now>=3750 && now<4000;
      reference.Level("me",level,now,true);
      if(auto state=gate.Observe(level,now,!reference.muted)) {actual=*state;++emitted;}
      check(actual==reference.speaking["me"],"reduced level preserves every 25 ms speaking decision");
    }
    check(emitted<12,"steady levels emit only speaking transitions");
  }

}
#ifdef HARMONY_SPEAKING_TEST
int main() {
  RunSpeakingTests([](bool ok,const char* name){if(!ok){std::cerr<<"FAIL: "<<name<<'\n';std::exit(1);}});
  std::cout<<"PASS: speaking floor, hold, finite levels, mute and playback gain\n";
}
#else
#include "media-tests.h"
#include "../../native/src/noise-filter.h"
#include "../../native/src/receive-gain.h"
#include "../../native/src/echo-reference.h"
#include <chrono>

void RunVoiceTests(void (*check)(bool, const char*)) {
  RunSpeakingTests(check);
  RunMediaTests(check);
  app::VoiceState v;
  v.Join("a", "Sala A"); check(v.room == "a" && v.connecting, "voice enters selected room");
  v.Recovered(10, true, true); check(!v.connecting, "voice initial RTP confirms connection");
  v.Level("person-a", 0.01, 100); v.Join("b", "Sala B");
  check(v.room == "b" && v.speaking.empty(), "voice switches room and clears old speakers");
  v.muted = true; check(!v.Sending(), "voice mute silences microphone");
  v.muted = false; v.deafened = true; check(!v.Sending(), "voice deafen also silences microphone");
  v.deafened = false; check(v.Sending(), "voice hearing restores previous mute choice");
  check(!v.Level("quiet", 0.001, 100), "voice threshold excludes exactly -60 dBFS");
  check(v.Level("person-a", 0.0011, 100) && v.speaking["person-a"], "voice threshold includes a quiet sound above -60 dBFS");
  check(!v.Level("person-a", 0, 399) && v.speaking["person-a"], "voice holds speaker for 299 ms");
  check(v.Level("person-a", 0, 400) && !v.speaking["person-a"], "voice releases speaker at 300 ms");
  v.muted = true; check(!v.Level("me", 100, 600, true) && !v.speaking["me"], "voice muted self never lights"); v.muted = false;
  v.Volume("person-a", 0); v.Volume("person-b", 2);
  check(v.Volume("person-a") == 0 && v.Volume("person-b") == 2 && v.Volume("new") == 1, "voice per-person volume does not affect another");
  v.Volume("clamped", 100); check(v.Volume("clamped") == 2, "voice volume ceiling is 200 percent");
  v.Volume("clamped", -1); check(v.Volume("clamped") == 0, "voice volume floor is zero");
  auto path = std::filesystem::path(L"test-output/voice-volumes-fixture.json");
  check(v.SaveVolumes(path), "voice volumes saved atomically");
  app::VoiceState restored; check(restored.LoadVolumes(path) && restored.Volume("person-a") == 0 && restored.Volume("person-b") == 2, "voice volumes persist between sessions");
  v.dfn=false; v.strength=73; v.echo=false; v.noise=false; v.agc=false; v.micGain=0.37;v.cameraID="Redmi 15 (Câmera Virtual do Windows)";
  auto noisePath=std::filesystem::path(L"test-output/voice-noise-fixture.json");
  check(v.SaveNoise(noisePath) && restored.LoadNoise(noisePath) && !restored.dfn && restored.strength==73,"voice DFN preferences persist");
  check(restored.cameraID==v.cameraID,"voice accented camera preference persists");
  check(!restored.echo && !restored.noise && !restored.agc && restored.micGain==0.37,"voice microphone filters persist between sessions");
  v.input = "usb"; v.output = "headphone";
  v.Devices(true, {{"usb","USB"}}); check(v.input == "usb" && v.inputs.front().id.empty(), "voice selected device uses stable endpoint");
  v.Devices(true, {}); check(v.input.empty() && v.output == "headphone", "voice removed input falls back without changing output");
  v.Devices(false, {}); check(v.output.empty(), "voice removed output falls back to Windows default");
  {
    v.Recover(1000); v.Recover(1300);
    check(v.reconnecting && v.recoveryStarted == 1000, "voice recovery keeps original outage time");
    check(!v.Recovered(1500, true, false) && v.reconnecting, "voice ICE alone cannot confirm recovered media");
    check(v.Recovered(1800, true, true) && !v.reconnecting && v.recoveryMs == 800 && v.room == "b", "voice recovery returns to same room with measured time");
  }
  v.Recover(2000); v.Leave();
  check(v.room.empty() && !v.reconnecting && v.speaking.empty(), "voice leaving cancels recovery and speaking");
  check(!v.Recovered(2500,true,true) && v.room.empty(), "voice late recovery cannot resurrect departed room");
  struct Failure : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& b, int, size_t, int) override { b.assign(b.size(),0); return false; }
  };
  audio::FilterSlot failure(std::make_unique<Failure>());
  std::vector<int16_t> block(480, 1200); auto original = block;
  check(!failure.Process(block,48000,1) && block == original, "voice failed filter passes raw voice");
  failure.enabled=false; auto count=failure.fallbacks.load();
  check(!failure.Process(block,48000,1) && block==original && !failure.active && failure.fallbacks==count,"voice DFN disabled restores NS without failure count");
  struct Throws : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& b,int,size_t,int) override { b.assign(b.size(),0); throw 1; }
  };
  audio::FilterSlot throws(std::make_unique<Throws>());
  check(!throws.Process(block,48000,1) && block==original && throws.fallbacks==1,"voice filter exception preserves raw block counted");
  struct Malformed : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& b,int,size_t,int) override { b.resize(1); return true; }
  };
  audio::FilterSlot malformed(std::make_unique<Malformed>());
  check(!malformed.Process(block,48000,1) && block==original && malformed.fallbacks==1,"voice malformed filter preserves raw block counted");
  struct Slow : audio::NoiseFilter {
    bool Process(std::vector<int16_t>& b, int, size_t, int) override { std::this_thread::sleep_for(std::chrono::milliseconds(100)); b.assign(b.size(),0); return true; }
  };
  audio::FilterSlot slow(std::make_unique<Slow>());
  auto start = std::chrono::steady_clock::now(); bool filtered = slow.Process(block,48000,1);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  check(!filtered && block == original && elapsed >= 100, "voice late filter passes raw voice with current quantum only");
  check(!slow.Process(block,48000,1) && block == original, "voice busy filter never queues audio backlog");
  std::map<std::string,double> gains{{"a",0},{"b",2}};
  check(audio::ReceiveGain(gains,"a",false)==0 && std::abs(audio::ReceiveGain(gains,"b",false)-3.981)<0.01 && audio::ReceiveGain(gains,"c",false)==1, "voice receive channels keep independent gains");
  check(std::abs(audio::LoudnessGain(1.5)-1.995)<0.01 && audio::LoudnessGain(0.5)==0.5 && audio::LoudnessGain(1)==1, "volume above 100% steps in decibels, up to +12 dB at 200%");
  check(audio::ReceiveGain(gains,"b",true)==0, "voice deafening zeros receive channels");
  int16_t received[4]{1200,-2400,20000,-20000}; audio::ScaleReceivePcm(received,4,2);
  check(received[0]==2400 && received[1]==-4800 && received[2]>30000 && received[2]<32767 && received[3]<-30000 && received[3]>-32767, "voice receive PCM gain rounds the peaks off instead of clipping");
  int16_t unity[2]{1200,-20000}; audio::ScaleReceivePcm(unity,2,1); check(unity[0]==1200 && unity[1]==-20000, "voice receive at 100% leaves the samples exactly");
  audio::ScaleReceivePcm(received,4,0); check(std::all_of(received,received+4,[](auto s){return s==0;}), "voice receive zero gain produces silence");
  int16_t render[3]{0,1600,32000}, external[3]{900,400,2000}, reference[3]{};
  audio::MixEchoReference(render,external,reference,3);
  check(reference[0]==900 && reference[1]==2000 && reference[2]==32767, "voice AEC reference includes post-gain render and other applications");
  check(render[0]==0 && render[1]==1600 && render[2]==32000, "voice external reference never changes speaker output");
  check(audio::RenderedFrames(480,2,960)==480 && audio::RenderedFrames(480,2,480)==240 && audio::RenderedFrames(480,0,960)==0, "voice render count uses interleaved samples without overread");
  check(audio::RenderedFrames(480,2,9600)==480, "voice render count never exceeds output capacity");
}
#endif
