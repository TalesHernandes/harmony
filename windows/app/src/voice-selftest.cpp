#include "service.h"
#include "platform.h"
#include "device-proof.h"
#include "../../native/src/camera.h"
#include "../../native/src/network-state.h"
#include "../../native/src/audio.h"
#include "../../native/src/resource-usage.h"
#include <fstream>

namespace app {
int VoiceSelfTest(const std::filesystem::path& output, const std::string& base) {
  auto result = json::Value::Object(); result["passed"] = false;
  auto write = [&] {
    auto usage=resources::Read();auto usageJson=json::Value::Object();
    for(size_t i=0;i<3;++i){auto part=json::Value::Object();part["loaded"]=int(usage.parts[i].loaded);part["estimated_bytes"]=int64_t(usage.parts[i].loaded?usage.parts[i].estimate:0);usageJson[i==0?"voice":i==1?"dfn":"camera"]=std::move(part);}
    usageJson["private_bytes"]=int64_t(resources::PrivateBytes());usageJson["camera_child_private"]=int64_t(usage.cameraPrivate);result["resources"]=std::move(usageJson);
    auto pending = output; pending += L".pending";
    std::ofstream out(pending, std::ios::binary); out << result.Dump(); out.close();
    return out && MoveFileExW(pending.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); };
  auto fail = [&](const char* stage) { result["stage"] = stage; write(); return 1; };
  net::Client check(base);
  if (base.empty() || check.secure() || !check.allowed()) return fail("local_server_required");
  auto user = Environment(L"HARMONY_TEST_USER"), password = Environment(L"HARMONY_TEST_PASSWORD"), visitor = Environment(L"HARMONY_TEST_VISITOR");
  if (user.empty() || password.empty() || visitor.empty()) return fail("fixture_environment_missing");
  Service service(base, [] {}, false);
  auto wait = [&](auto predicate, int seconds = 30) {
    auto until = GetTickCount64() + seconds * 1000;
    while (GetTickCount64() < until) { if (predicate(service.Read())) return true; Sleep(10); } return false;
  };
  service.Restore(); if (!wait([](auto s) { return !s.busy; })) return fail("restore_timeout");
  service.Login(std::move(user), std::move(password));
  if (!wait([](auto s) { return s.signedIn && s.model.live; })) return fail("login_timeout");
  auto state = service.Read(); if (state.model.rooms.empty()) return fail("no_voice_room");
  auto room = state.model.rooms.front().id; result["room_id"] = room;
  // Select existing endpoints explicitly before joining; default/empty is not
  // evidence that a device operation happened.
  service.VoiceDevices();
  if(!wait([](auto s) { return s.voice.inputs.size()>1 && s.voice.outputs.size()>1; })) return fail("selected_device_not_found");
  state=service.Read();
  auto selectedInput=state.voice.inputs[1].id, selectedOutput=state.voice.outputs[1].id;
  service.VoiceDeviceChoice(true,selectedInput); service.VoiceDeviceChoice(false,selectedOutput);
  if(!wait([&](auto s) { return s.voice.input==selectedInput && s.voice.output==selectedOutput; })) return fail("selected_device_not_applied");
  service.VoiceNoise(true,50);
  auto started = GetTickCount64(); service.JoinVoice(room);
  if (!wait([&](auto s) { return s.voice.room == room && !s.voice.connecting; })) return fail("join_media_timeout");
  if(!wait([](auto s) { return s.voice.dfn && s.voice.dfnActive; })) return fail("dfn_not_active");
  result["dfn_active"]=true; result["dfn_fallbacks"]=int64_t(service.Read().voice.dfnFallbacks);
  result["join_ms"] = int64_t(GetTickCount64() - started); result["stage"] = "ready"; write();
  // Run the owner's web -> leave -> native sequence against real media, without
  // a window or a shared persisted session. Each command samples decoded audio.
  if(Environment(L"HARMONY_TEST_REJOIN")=="1") {
    // The simulated ADM sleeps for 10 ms. Measure whether Windows' default
    // 15.6 ms timer quantum is starving its real-time RTP source.
    struct TestClock {
      HMODULE dll=nullptr; using Period=unsigned(WINAPI*)(unsigned);Period end=nullptr;
      TestClock(){if(Environment(L"W8_CLOCK")=="1"){dll=LoadLibraryW(L"winmm.dll");if(dll){auto begin=reinterpret_cast<Period>(GetProcAddress(dll,"timeBeginPeriod"));end=reinterpret_cast<Period>(GetProcAddress(dll,"timeEndPeriod"));if(begin)begin(1);}}}
      ~TestClock(){if(end)end(1);if(dll)FreeLibrary(dll);}
    } testClock;
    struct Player {
      HANDLE process=nullptr;
      ~Player(){if(process){TerminateProcess(process,0);CloseHandle(process);}}
    } player;
    if(Environment(L"W8_REAL")=="1") {
      auto devices=service.Read().voice; std::string input,outputDevice;
      for(auto& d:devices.inputs) if(d.name.find("CABLE Output")!=std::string::npos) input=d.id;
      for(auto& d:devices.outputs) if(d.name.find("CABLE Input")!=std::string::npos) outputDevice=d.id;
      if(input.empty()||outputDevice.empty()) return fail("cable_devices_not_found");
      service.VoiceDeviceChoice(true,input);service.VoiceDeviceChoice(false,outputDevice);
      if(!wait([&](auto s){return s.voice.input==input&&s.voice.output==outputDevice&&s.voice.inputReady;}))return fail("cable_selection_timeout");
      if(!Environment(L"W8_TAP_DIR").empty())SetEnvironmentVariableW(L"W8_TAP_ACTIVE",L"1");
      result["input"]=input;result["output"]=outputDevice;write();
    }
    auto command=output; command+=L".command"; std::string previous;
    auto deadline=GetTickCount64()+180000;
    while(GetTickCount64()<deadline) {
      std::ifstream in(command); std::string value; std::getline(in,value);
      if(value.empty() || value==previous) { Sleep(20); continue; }
      previous=value;
      if(value=="camera-real" || value=="camera-virtual") {
        std::string id="__harmony_pipe_perf_test__";
        if(value=="camera-real") {
          auto cameras=camera::Devices();id.clear();
          for(auto& device:cameras)if(device.name.find("C505")!=std::string::npos)id=device.id;
          if(id.empty())return fail("physical_camera_not_found");
        }
        service.Camera(true,id);
        if(!wait([&](auto s){return s.voice.cameraID==id && s.voice.sharingCamera;}))return fail("camera_switch_timeout");
        Sleep(1200);auto v=service.Read().voice;
        result["stage"]=value;result["connection_identity"]=std::to_string(v.connectionIdentity);result["session_identity"]=std::to_string(v.sessionIdentity);
        auto sample=network::Latest(false).Read();auto network=json::Value::Object();
        network["rtt_ms"]=sample.rtt;network["jitter_ms"]=sample.jitter;network["loss_percent"]=sample.loss;network["send_kbps"]=sample.send;network["receive_kbps"]=sample.receive;network["available_kbps"]=sample.available;
        result["network"]=network;write();continue;
      }
      if(value=="play-mixture"||value=="play-clean"||value=="play-loop") {
        if(player.process){TerminateProcess(player.process,0);CloseHandle(player.process);player.process=nullptr;}
        auto exe=Environment(L"W8_PYTHON"),script=Environment(L"W8_PLAYER"),directory=Environment(L"W8_FIXTURE_DIR");
        if(exe.empty()||script.empty()||directory.empty())return fail("player_environment_missing");
        auto path=std::filesystem::path(net::Wide(directory))/(value=="play-clean"?L"clean.pcm":L"mixture.pcm");
        auto line=L"\""+net::Wide(exe)+L"\" \""+net::Wide(script)+L"\" \""+path.wstring()+L"\" "+(value=="play-loop"?L"100":L"1");
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        if(!CreateProcessW(net::Wide(exe).c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return fail("player_launch_failed");
        player.process=pi.hProcess;CloseHandle(pi.hThread);result["player_pid"]=int64_t(pi.dwProcessId);
        result["stage"]=value;write();continue;
      }
      // The Diagnóstico recording, armed as its switch arms it, into W8_RECORD_DIR.
      if(value.starts_with("record-")) {
        auto directory=Environment(L"W8_RECORD_DIR");if(directory.empty())return fail("record_directory_missing");
        audio::ArmRecording(std::filesystem::path(net::Wide(directory)),std::stoi(value.substr(7)));
        if(!wait([](auto){auto s=audio::ReadRecording();return s.state==audio::RecordingStatus::Saved||s.state==audio::RecordingStatus::Failed;},std::stoi(value.substr(7))+30))
          return fail("record_timeout");
        auto s=audio::ReadRecording();if(s.state!=audio::RecordingStatus::Saved)return fail("record_failed");
        result["record_folder"]=net::Narrow(s.folder.wstring());result["record_seconds"]=int64_t(s.seconds);result["stage"]=value;write();continue;
      }
      if(value=="mute"||value=="unmute") {
        service.MuteVoice(value=="mute",false);
        if(!wait([&](auto s){return s.voice.muted==(value=="mute");})) return fail("command_mute_timeout");
        result["stage"]=value;write();continue;
      }
      if(value=="leave") {
        service.LeaveVoice();if(!wait([](auto s){return s.voice.room.empty();}))return fail("command_leave_timeout");
        result["stage"]=value;write();continue;
      }
      if(value=="join") {
        service.JoinVoice(room);if(!wait([&](auto s){return s.voice.room==room&&!s.voice.connecting;}))return fail("command_join_timeout");
        result["stage"]=value;write();continue;
      }
      if(value=="finish") { service.LeaveVoice(); service.Stop(); result["passed"]=true; result["stage"]="complete"; write(); return 0; }
      // The friend's program in the server log of 22/10: in, out a second
      // later, back in seven seconds after that, all in one process.
      if(value=="bounce") {
        result["stage"]="bounce-first";write();
        double firstPeak=0;auto firstUntil=GetTickCount64()+1000;
        while(GetTickCount64()<firstUntil){firstPeak=std::max(firstPeak,service.Read().voice.renderLevel);Sleep(10);}
        result["bounce_first_render_peak"]=firstPeak;service.LeaveVoice();
        if(!wait([](auto s) { return s.voice.room.empty(); })) return fail("bounce_leave_timeout");
        result["stage"]="bounce-left";write();
        Sleep(6000); service.JoinVoice(room);
        if(!wait([&](auto s) { return s.voice.room == room && !s.voice.connecting; })) return fail("bounce_join_timeout");
        result["stage"]="bounce"; write(); continue;
      }
      double peak=0; auto until=GetTickCount64()+2500;
      while(GetTickCount64()<until) { peak=std::max(peak,service.Read().voice.renderLevel); Sleep(10); }
      auto voice=service.Read().voice;
      result["stage"]=value; result["render_peak"]=peak;
      result["capture_callbacks"]=int64_t(voice.captureCallbacks); result["render_callbacks"]=int64_t(voice.renderCallbacks);
      result["generation"]=int64_t(voice.sessionGeneration);
      result["muted"]=voice.muted;result["deafened"]=voice.deafened;result["mic_gain"]=voice.micGain;
      result["dfn_active"]=voice.dfnActive;result["noise_filter"]=voice.noiseFilter;
      result["dfn_enabled"]=voice.dfn;result["strength_db"]=voice.strength;
      result["echo"]=voice.echo;result["noise"]=voice.noise;result["agc"]=voice.agc;
      auto speaking=json::Value::Object(); for(auto& [who,on]:voice.speaking)speaking[who]=on;
      result["speaking"]=std::move(speaking);write();
    }
    return fail("rejoin_deadline");
  }
  if (!wait([&](auto s) { auto it = s.voice.speaking.find(visitor); return it != s.voice.speaking.end() && it->second; })) return fail("remote_speaking_timeout");
  result["remote_speaking"] = true; result["stage"] = "speaking"; write();
  if (!wait([](auto s) { return s.voice.renderLevel > 2.5; })) return fail("render_audio_unavailable");
  result["render_before"] = service.Read().voice.renderLevel;
  auto command = output; command += L".command";
  auto acknowledged = [&](std::string wanted) {
    return wait([&](auto) {
      std::ifstream in(command); std::string value; std::getline(in, value); return value == wanted;
    });
  };
  if (!acknowledged("own-speaking")) return fail("own_speaking_timeout");
  result["own_speaking"] = true;
  service.MuteVoice(true, false); result["stage"] = "muting"; write();
  if (!acknowledged("mute-propagated")) return fail("mute_propagation_timeout");
  result["mute_propagated"] = true; result["mute_media_silent"]=true;
  service.MuteVoice(false, false); result["stage"]="unmuted"; write();
  if(!acknowledged("unmute-flowing")) return fail("unmute_media_timeout");
  service.MuteVoice(false,true); result["stage"]="deafening"; write();
  if(!wait([](auto s) { return s.voice.renderActive && s.voice.renderLevel<0.01; })) return fail("deafen_render_failed");
  if(!acknowledged("deafen-silent")) return fail("deafen_media_timeout");
  result["deafen_media_silent"]=true;
  service.MuteVoice(false,false);
  service.VoiceVolume(visitor, 0); if (!wait([&](auto s) { return s.voice.Volume(visitor) == 0; })) return fail("volume_timeout");
  result["stage"] = "volume-zero"; write();
  Sleep(750);
  result["render_after_gain_zero_750_ms"]=service.Read().voice.renderLevel; write();
  result["render_active_at_zero"]=service.Read().voice.renderActive;
  result["render_callbacks_at_zero"]=int64_t(service.Read().voice.renderCallbacks); write();
  if (!wait([](auto s) { return s.voice.renderActive && s.voice.renderLevel < 0.01; })) return fail("volume_zero_render_failed");
  double zeroMax=0;
  for(int i=0;i<50;++i) { Sleep(10); zeroMax=std::max(zeroMax,service.Read().voice.renderLevel); }
  if(zeroMax>=0.01 || !service.Read().voice.renderActive) return fail("volume_zero_not_stable");
  result["render_zero_max_500_ms"] = zeroMax; result["stage"] = "volume-other"; write();
  if (!wait([](auto s) { return s.voice.renderLevel > 2.5; })) return fail("volume_other_render_failed");
  result["render_other"] = service.Read().voice.renderLevel;
  // First prove A opened and produced actual ADM callbacks. Then select a
  // vanished ID, forcing the production fallback opener to the Windows default.
  if(!wait([&](auto s) { return SelectedDeviceReady(s.voice,selectedInput,selectedOutput); })) return fail("selected_device_callbacks_missing");
  auto beforeDevice=service.Read().voice;
  if(beforeDevice.input!=selectedInput || beforeDevice.output!=selectedOutput) return fail("selected_device_open_failed");
  auto generation = service.Read().voice.sessionGeneration;
  service.VoiceDeviceChoice(true, "test-missing-endpoint");
  service.VoiceDeviceChoice(false, "test-missing-endpoint");
  if(!wait([&](auto s) { return DefaultDeviceRecovered(beforeDevice,s.voice); })) return fail("default_device_callbacks_missing");
  state = service.Read();
  result["device_capture_before"]=int64_t(beforeDevice.captureCallbacks); result["device_capture_after"]=int64_t(state.voice.captureCallbacks);
  result["device_render_before"]=int64_t(beforeDevice.renderCallbacks); result["device_render_after"]=int64_t(state.voice.renderCallbacks);
  result["device_default_opened"]=state.voice.inputReady && state.voice.outputReady;
  if (!state.voice.input.empty() || !state.voice.output.empty() || state.voice.sessionGeneration != generation) return fail("device_changed_session");
  result["device_same_session"] = true;
  result["device_same_peer_connection"]=state.voice.connectionIdentity==beforeDevice.connectionIdentity;
  result["device_same_server_session"]=state.voice.sessionIdentity==beforeDevice.sessionIdentity; result["device_fixture"]="controlled ADM A to default B; real Transport callbacks";
  for(auto reason:{"network","resume"}) {
    auto before=service.Read().voice.recoveryCount;
    service.RecoverVoice(reason);
    if(!wait([&](auto s) { return s.voice.recoveryCount>before && !s.voice.reconnecting && s.voice.recoveryMs>=0; })) return fail("simulated_recovery_timeout");
    state=service.Read(); if(state.voice.sessionGeneration!=generation || state.voice.room!=room) return fail("ice_restart_changed_session");
    result[std::string(reason)+"_simulated_ms"]=state.voice.recoveryMs;
  }
  result["ice_restart_same_session"]=true; result["stage"] = "restart-server"; write();
  started = GetTickCount64();
  if (!wait([&](auto s) { return s.voice.reconnecting; })) return fail("server_outage_not_observed");
  if (!wait([&](auto s) { return s.voice.room == room && !s.voice.reconnecting && s.voice.sessionGeneration > generation; }, 40)) return fail("server_recovery_timeout");
  result["server_recovery_ms"] = service.Read().voice.recoveryMs;
  result["server_total_ms"] = int64_t(GetTickCount64() - started);
  result["same_room"] = true; result["stage"] = "recovered"; write();
  if (!acknowledged("finish")) return fail("finish_timeout");
  service.LeaveVoice(); if (!wait([](auto s) { return s.voice.room.empty(); })) return fail("leave_timeout");
  service.Logout(); if (!wait([](auto s) { return !s.signedIn; })) return fail("logout_timeout");
  service.Stop(); result["stage"] = "complete"; result["passed"] = true; return write() ? 0 : 1;
}
}
