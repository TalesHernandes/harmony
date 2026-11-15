#include "service.h"
#include "platform.h"
#include "test-audio-device.h"
#include "capture-notice.h"
#include "../../native/src/perf.h"
#include "../../native/src/sync-probe.h"
#include "../../native/src/log.h"
#include "../../native/gpu/gpu.h"
#include <fstream>
#include <set>
#include "w13-camera-selftest.h"
namespace app {
int ScreenSelfTest(const std::filesystem::path& output,const std::string& base) {
  auto result=json::Value::Object();result["passed"]=false;result["stage"]="starting";
  std::set<std::string> mappings;
  auto write=[&] {for(auto& line:LogLines())if(line.find("InitEncode index=")!=std::string::npos||line.find("screen sender index=")!=std::string::npos)mappings.insert(line);result["layer_mapping"]=json::Value::Array();for(auto& line:mappings)result["layer_mapping"].Push(line);result["screen_diagnostics"]=json::Value::Array();for(auto& line:LogLines())if(line.find("screen encoder ")!=std::string::npos||line.find("screen feedback ")!=std::string::npos||line.find("screen key request ")!=std::string::npos||line.find("screen open failed ")!=std::string::npos||line.find("screen capture fallback=")!=std::string::npos||line.find("voice bandwidth ")!=std::string::npos||line.find("webrtc ")!=std::string::npos)result["screen_diagnostics"].Push(line);auto pending=output;pending+=L".pending";std::ofstream file(pending,std::ios::binary);file<<result.Dump();file.close();return file&&MoveFileExW(pending.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);};
  auto fail=[&](const char* stage){result["stage"]=stage;write();return 1;};
  net::Client guard(base);if(base.empty()||guard.secure()||!guard.allowed())return fail("local_server_required");
  gpu::ConfigureCaptureSelftest(Environment(L"HARMONY_TEST_DUPLICATION")=="1");
  gpu::ConfigureNVEncSelftest(Environment(L"HARMONY_TEST_NVENC_SYNC")=="1");
  HWND window=reinterpret_cast<HWND>(_wcstoui64(net::Wide(Environment(L"HARMONY_TEST_WINDOW")).c_str(),nullptr,10));
  if(!IsWindow(window))return fail("fixture_window_missing");
  if(!GetForegroundWindow())return fail("interactive_desktop_unavailable");
  auto seconds=std::stoi(Environment(L"HARMONY_TEST_SECONDS"));if(seconds<1||seconds>3600)return fail("invalid_duration");
  if(!SetEnvironmentVariableW(L"HARMONY_TEST_REQUIRE_CABLE",L"1"))return fail("CABLE isolation required");
  W13VideoSink screenSink,cameraSink;
  Service service(base,[]{},false);auto wait=[&](auto condition,int timeout=30){auto until=GetTickCount64()+timeout*1000,nextLog=GetTickCount64();while(GetTickCount64()<until){auto current=service.Read();if(condition(current))return true;if(GetTickCount64()>=nextLog){logf("screen selftest waiting state=%s",current.voice.mediaNotice.c_str());result["opening_state"]=current.voice.mediaNotice;write();nextLog=GetTickCount64()+1000;}MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(25);}return false;};
  service.Restore();if(!wait([](auto state){return !state.busy;}))return fail("restore_timeout");
  service.Login(Environment(L"HARMONY_TEST_USER"),Environment(L"HARMONY_TEST_PASSWORD"));if(!wait([](auto state){return state.signedIn&&state.model.live;}))return fail("login_timeout");
  auto state=service.Read();if(state.model.rooms.empty())return fail("voice_room_missing");
  auto outputDevice=TestAudioDevice();if(outputDevice.empty())return fail("CABLE Input unavailable");
  service.VoiceDeviceChoice(false,outputDevice);
  if(!wait([&](auto state){return state.voice.output==outputDevice;}))return fail("CABLE Input selection timeout");
  service.JoinVoice(state.model.rooms.front().id);if(!wait([](auto state){return !state.voice.connecting&&!state.voice.room.empty();}))return fail("voice_media_timeout");
  auto notice=TestTransmissionNotice(window);if(!notice)return fail("notice_exclusion_failed");
  struct NoticeOwner {HWND window;~NoticeOwner(){if(window)DestroyWindow(window);}} noticeOwner{notice};
  result["notice_visible"]=IsWindowVisible(notice)!=FALSE;result["notice_excluded"]=true;
  const bool monitor=Environment(L"HARMONY_TEST_MONITOR")=="1";
  const auto requestedFps=Environment(L"HARMONY_TEST_FPS");
  const int fps=requestedFps.empty()?60:std::stoi(requestedFps);
  if(fps!=15&&fps!=30&&fps!=60)return fail("invalid_frame_rate");
  result["capture_kind"]=monitor?"monitor":"window";result["requested_fps"]=fps;
  const bool voiceOnly=Environment(L"W13_VOICE_ONLY")=="1";
  if(!voiceOnly) service.ShareScreen(monitor?nullptr:window,monitor?MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST):nullptr,1080,fps,true);
  if(!voiceOnly&&!wait([](auto state){return state.voice.mediaNotice=="transmitting"||state.voice.mediaNotice=="capture_unavailable"||state.voice.mediaNotice=="screen_offer_failed"||state.voice.mediaNotice=="screen_audio_unavailable";})) {logf("screen selftest timeout step=offer_response last_state=%s",service.Read().voice.mediaNotice.c_str());return fail("screen_offer_timeout");}
  if(!voiceOnly&&service.Read().voice.mediaNotice!="transmitting")return fail(service.Read().voice.mediaNotice.c_str());
  bool camera=Environment(L"HARMONY_TEST_CAMERA")=="1";if(camera)service.Camera(true,"");
  auto watchUser=Environment(L"W13_WATCH");if(!watchUser.empty()){service.Video(watchUser,"screen",[&](auto* track){screenSink.Attach(track);});service.VideoSize(watchUser,1920,1,true);service.Video(watchUser,"cam",[&](auto* track){cameraSink.Attach(track);});}
  result["stage"]="transmitting";result["connection_identity"]=int64_t(service.Read().voice.connectionIdentity);write();
  auto until=GetTickCount64()+seconds*1000;auto readings=json::Value::Array();
  std::string previousCommand;
  while(GetTickCount64()<until) {
    auto commandPath=Environment(L"W13_COMMAND");
    if(!commandPath.empty()) {std::ifstream file(commandPath);std::string command;std::getline(file,command);if(command!=previousCommand){previousCommand=command;if(command=="on"||command=="on-again"){camera=true;service.Camera(true,"__harmony_pipe_perf_test__");}else if(command=="off"){camera=false;service.Camera(false,"");}result["command"]=command;}}
    MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    if(!IsWindowVisible(notice))return fail("notice_not_visible");
    auto s=service.Read();if(s.voice.outputStatus=="failed")return fail("CABLE Input playout unavailable");if(!voiceOnly&&!s.voice.sharingScreen)return fail("capture_lost");if(camera&&s.voice.mediaNotice=="camera_unavailable")return fail("camera_unavailable");
    auto snapshot=perf::TakeSnapshot();auto reading=json::Value::Object();reading["s"]=snapshot.seconds;reading["cpu"]=snapshot.process_cpu_percent;reading["memory_bytes"]=int64_t(snapshot.working_set_bytes);reading["codificador"]=gpu::ActiveEncoderName();
    auto visitor=Environment(L"HARMONY_TEST_VISITOR");auto speaking=s.voice.speaking.find(visitor);reading["call_playout_confirmed"]=s.voice.renderLevel>2.5&&speaking!=s.voice.speaking.end()&&speaking->second;
    reading["render_level"]=s.voice.renderLevel;reading["render_callbacks"]=int64_t(s.voice.renderCallbacks);reading["memory"]=W13Memory();reading["screen_decoded"]=int64_t(screenSink.frames.load());reading["camera_decoded"]=int64_t(cameraSink.frames.load());
    reading["etapas"]=json::Value::Object();for(auto& stage:snapshot.stages){auto value=json::Value::Object();value["in"]=int64_t(stage.in);value["out"]=int64_t(stage.out);value["drop"]=int64_t(stage.dropped);value["avg"]=stage.avg_ms;value["max"]=stage.max_ms;reading["etapas"][stage.name]=value;}
    reading["tarefas"]=json::Value::Object();for(auto& thread:snapshot.threads)reading["tarefas"][thread.name]=thread.cpu_percent;
    if(sync_probe::Enabled()) reading["sync_probe"]=sync_probe::Drain();
    if(Environment(L"W13_COMMAND").empty()){readings.Push(reading);result["native_measurements"]=readings;}result["latest_native_measurement"]=reading;write();Sleep(1000);
  }
  service.StopScreen();
  if(sync_probe::Enabled()) result["sync_probe_tail"]=sync_probe::Drain();
  result["native_measurements"]=readings;result["stage"]="done";result["passed"]=true;write();service.StopScreen();service.LeaveVoice();service.Stop();return 0;
}
}
