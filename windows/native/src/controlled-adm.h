// Local test fixture only; never used for a secure server.
#pragma once
#include "api/audio/audio_device.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <cstring>
#include <vector>
#include <cmath>
#include <fstream>
#include <filesystem>
class FakeADM : public webrtc::AudioDeviceModule {
 public:
  std::atomic<bool> present{true},recording{true},playing{true};
  int failRecord=0,failPlay=0,recordOpens=0,playOpens=0,recordStarts=0,playStarts=0,recordDefaults=0,playDefaults=0,lastRecordType=-1,lastPlayType=-1;
  webrtc::AudioTransport* callback=nullptr;
  std::mutex callbackMu;
  std::atomic<bool> stopping{false}; std::thread pump;
  ~FakeADM() override { stopping=true; if(pump.joinable()) pump.join(); }
  std::vector<int16_t> testPcm; double position=0;
  uint32_t captureRate=48000; size_t captureChannels=1;
  void Pulse() {
    std::lock_guard lock(callbackMu); if(!callback) return;
    int16_t data[480]{}; uint32_t level=0; size_t frames=0; int64_t elapsed=0,ntp=0;
    std::vector<int16_t> capture((captureRate/100)*captureChannels);
    for(size_t f=0;f<captureRate/100;++f) {
      auto sample=testPcm.empty()?int16_t(6000*std::sin(6.283185307179586*440*position/48000)) : testPcm[size_t(position)%testPcm.size()];
      for(size_t c=0;c<captureChannels;++c) capture[f*captureChannels+c]=sample;
      position+=48000.0/captureRate;
    }
    if(recording) callback->RecordedDataIsAvailable(capture.data(),captureRate/100,2*captureChannels,captureChannels,captureRate,0,0,0,false,level);
    if(playing) callback->NeedMorePlayData(480,2,1,48000,data,frames,&elapsed,&ntp);
  }
  void EnablePump() {
    recording=false; playing=false;
    // Enabled only by the local controlled-device factory, never the real ADM.
    wchar_t path[32768]{}; auto n=GetEnvironmentVariableW(L"HARMONY_TEST_PCM",path,32768);
    if(n && n<32768) {
      std::ifstream in(std::filesystem::path(path),std::ios::binary|std::ios::ate); auto bytes=in.tellg();
      if(bytes>0 && bytes<48000*2*60 && int64_t(bytes)%2==0) { testPcm.resize(size_t(bytes)/2); in.seekg(0); in.read(reinterpret_cast<char*>(testPcm.data()),bytes); }
    }
    pump=std::thread([this] { while(!stopping) { Pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } });
  }
  int32_t RegisterAudioCallback(webrtc::AudioTransport* cb) override { std::lock_guard lock(callbackMu); callback=cb; return 0; }
  int32_t ActiveAudioLayer(AudioLayer* l) const override { return 0; }
  int32_t Init() override { return 0; }
  int32_t Terminate() override { return 0; }
  bool Initialized() const override { return true; }
  int16_t PlayoutDevices() override { return present ? 1 : 0; }
  int16_t RecordingDevices() override { return present ? 1 : 0; }
  int32_t PlayoutDeviceName(uint16_t i, char n[webrtc::kAdmMaxDeviceNameSize],
                            char g[webrtc::kAdmMaxGuidSize]) override { strcpy_s(n,webrtc::kAdmMaxDeviceNameSize,"Selected A"); strcpy_s(g,webrtc::kAdmMaxGuidSize,"device-a"); return 0; }
  int32_t RecordingDeviceName(uint16_t i, char n[webrtc::kAdmMaxDeviceNameSize],
                              char g[webrtc::kAdmMaxGuidSize]) override { strcpy_s(n,webrtc::kAdmMaxDeviceNameSize,"Selected A"); strcpy_s(g,webrtc::kAdmMaxGuidSize,"device-a"); return 0; }
  int32_t SetPlayoutDevice(uint16_t i) override { return 0; }
  int32_t SetPlayoutDevice(WindowsDeviceType d) override { ++playDefaults; lastPlayType=int(d); return 0; }
  int32_t SetRecordingDevice(uint16_t i) override { return 0; }
  int32_t SetRecordingDevice(WindowsDeviceType d) override { ++recordDefaults; lastRecordType=int(d); return 0; }
  int32_t PlayoutIsAvailable(bool* a) override { *a=true; return 0; }
  int32_t InitPlayout() override { ++playOpens; if(failPlay) { --failPlay; return -1; } return 0; }
  bool PlayoutIsInitialized() const override { return false; }
  int32_t RecordingIsAvailable(bool* a) override { *a=true; return 0; }
  int32_t InitRecording() override { ++recordOpens; if(failRecord) { --failRecord; return -1; } return 0; }
  bool RecordingIsInitialized() const override { return false; }
  int32_t StartPlayout() override { playing=true; ++playStarts; return 0; }
  int32_t StopPlayout() override { std::lock_guard lock(callbackMu); playing=false; return 0; }
  bool Playing() const override { return playing.load(); }
  int32_t StartRecording() override { recording=true; ++recordStarts; return 0; }
  int32_t StopRecording() override { std::lock_guard lock(callbackMu); recording=false; return 0; }
  bool Recording() const override { return recording.load(); }
  int32_t InitSpeaker() override { return 0; }
  bool SpeakerIsInitialized() const override { return false; }
  int32_t InitMicrophone() override { return 0; }
  bool MicrophoneIsInitialized() const override { return false; }
  int32_t SpeakerVolumeIsAvailable(bool* a) override { return 0; }
  int32_t SetSpeakerVolume(uint32_t v) override { return 0; }
  int32_t SpeakerVolume(uint32_t* v) const override { return 0; }
  int32_t MaxSpeakerVolume(uint32_t* v) const override { return 0; }
  int32_t MinSpeakerVolume(uint32_t* v) const override { return 0; }
  int32_t MicrophoneVolumeIsAvailable(bool* a) override { return 0; }
  int32_t SetMicrophoneVolume(uint32_t v) override { return 0; }
  int32_t MicrophoneVolume(uint32_t* v) const override { return 0; }
  int32_t MaxMicrophoneVolume(uint32_t* v) const override { return 0; }
  int32_t MinMicrophoneVolume(uint32_t* v) const override { return 0; }
  int32_t SpeakerMuteIsAvailable(bool* a) override { return 0; }
  int32_t SetSpeakerMute(bool e) override { return 0; }
  int32_t SpeakerMute(bool* e) const override { return 0; }
  int32_t MicrophoneMuteIsAvailable(bool* a) override { return 0; }
  int32_t SetMicrophoneMute(bool e) override { return 0; }
  int32_t MicrophoneMute(bool* e) const override { return 0; }
  int32_t StereoPlayoutIsAvailable(bool* a) const override { return 0; }
  int32_t SetStereoPlayout(bool e) override { return 0; }
  int32_t StereoPlayout(bool* e) const override { return 0; }
  int32_t StereoRecordingIsAvailable(bool* a) const override { return 0; }
  int32_t SetStereoRecording(bool e) override { return recording.load(); }
  int32_t StereoRecording(bool* e) const override { return recording.load(); }
  int32_t PlayoutDelay(uint16_t* d) const override { return 0; }
  bool BuiltInAECIsAvailable() const override { return false; }
  bool BuiltInAGCIsAvailable() const override { return false; }
  bool BuiltInNSIsAvailable() const override { return false; }
  int32_t EnableBuiltInAEC(bool e) override { return 0; }
  int32_t EnableBuiltInAGC(bool e) override { return 0; }
  int32_t EnableBuiltInNS(bool e) override { return 0; }
  int32_t GetPlayoutUnderrunCount() const override { return 0; }
  std::optional<Stats> GetStats() const override { return std::nullopt; }
};
