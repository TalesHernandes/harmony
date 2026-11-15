#pragma once
#include <windows.h>
#include "../../native/src/json.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace app {
struct VoiceDevice { std::string id, name; bool operator==(const VoiceDevice&) const = default; };
struct VoiceState {
  std::string room, roomName, input, output, self;
  bool sharingScreen=false,sharingCamera=false;
  std::string mediaNotice,cameraID;
  std::string connectionStep="Conectando ao servidor?";
  bool muted = false, deafened = false, connecting = false, reconnecting = false;
  bool echo=true, noise=true, agc=true;
  bool dfn=true, dfnActive=false; uint64_t dfnFallbacks=0; std::string noiseFilter="libwebrtc NS", noiseReason="aguardando captura";
  double micGain=1;
  int strength = 50;
  // "Ouvir minha voz" is on (Service::TestVoice); voiceTestError says why it
  // could not open or stopped. Never saved: it starts off every time.
  bool voiceTest = false; std::string voiceTestError;
  int64_t recoveryStarted = 0, recoveryMs = -1;
  unsigned sessionGeneration = 0;
  unsigned recoveryCount = 0;
  double renderLevel = 0;
  unsigned renderCallbacks=0;
  uint64_t connectionIdentity=0, sessionIdentity=0;
  uint64_t captureCallbacks=0, referenceMissing=0, referenceDropped=0, referenceAge=0;
  bool inputReady=false, outputReady=false;
  std::string inputStatus, outputStatus;
  bool renderActive=false;
  std::map<std::string, double> volumes;
  std::map<std::string, int64_t> lastLoud;
  std::map<std::string, bool> speaking;
  std::vector<VoiceDevice> inputs{{"", "Padrão do Windows"}}, outputs{{"", "Padrão do Windows"}};
  void Join(std::string id, std::string name) {
    room = std::move(id); roomName = std::move(name); connecting = true; connectionStep="Conectando ao servidor?";
    reconnecting = false; lastLoud.clear(); speaking.clear();
  }
  void Leave() {
    sharingScreen=sharingCamera=false; mediaNotice.clear();
    room.clear(); roomName.clear(); connecting = reconnecting = false;
    recoveryStarted = 0; lastLoud.clear(); speaking.clear();
  }
  bool Sending() const { return !muted && !deafened; }
  double Volume(const std::string& who) const {
    auto it = volumes.find(who); return it == volumes.end() ? 1.0 : it->second;
  }
  void Volume(const std::string& who, double value) {
    if (!who.empty() && std::isfinite(value)) volumes[who] = std::clamp(value, 0.0, 2.0);
  }
  // The web's speaking.ts: linear RMS above -60 dBFS lights at once, and the
  // ring holds 300 ms past the last sound so syllables do not blink it.
  static constexpr double kSpeakingFloor = 0.001;
  static constexpr int64_t kSpeakingHoldMs = 300;
  bool Level(const std::string& who, double amplitude, int64_t now, bool own = false) {
    if (who.empty() || room.empty()) return false;
    bool changed = false;
    if (own && !Sending()) {
      lastLoud.erase(who);
      changed = speaking[who]; speaking[who] = false;
    }
    if (std::isfinite(amplitude) && amplitude > kSpeakingFloor && (!own || Sending())) lastLoud[who] = now;
    for (auto& [person, when] : lastLoud) {
      bool on = now - when < kSpeakingHoldMs;
      changed = changed || speaking[person] != on; speaking[person] = on;
    }
    return changed;
  }
  void Recover(int64_t now) {
    if (room.empty()) return;
    if (!reconnecting) { ++recoveryCount; recoveryStarted = now; recoveryMs = -1; }
    reconnecting = true;
  }
  // Connected ICE alone is not proof of recovered voice; RTP must grow.
  bool Recovered(int64_t now, bool connected, bool mediaGrowing) {
    if (!connected || !mediaGrowing || room.empty()) return false;
    connecting = false;
    if (reconnecting) { recoveryMs = now - recoveryStarted; reconnecting = false; }
    return true;
  }
  bool Devices(bool capture, std::vector<VoiceDevice> list) {
    list.insert(list.begin(), {"", "Padrão do Windows"});
    auto& selected = capture ? input : output;
    auto& current = capture ? inputs : outputs;
    bool missing = !selected.empty() && std::none_of(list.begin(), list.end(), [&](auto& d) { return d.id == selected; });
    if (missing) selected.clear();
    bool changed = current != list || missing; current = std::move(list); return changed;
  }
  bool LoadNoise(const std::filesystem::path& path) {
    std::error_code ec; auto size=std::filesystem::file_size(path,ec); if(ec || size>1024) return false;
    std::ifstream in(path,std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)),{});
    auto value=json::Parse(bytes); if(!value || !value->IsObject()) return false;
    cameraID=(*value)["cameraID"].Str();
    auto gain=(*value)["micGain"].Number(1); micGain=std::isfinite(gain)?std::clamp(gain,0.,2.):1;
    echo=(*value)["echo"].Bool(true); noise=(*value)["noise"].Bool(true); agc=(*value)["agc"].Bool(true);
    dfn=(*value)["enabled"].Bool(true); auto level=(*value)["strength"].Number(50); strength=std::isfinite(level)?int(std::clamp(level,10.,100.)):50; return true;
  }
  bool SaveNoise(const std::filesystem::path& path) const {
    auto value=json::Value::Object(); value["enabled"]=dfn; value["strength"]=strength; value["echo"]=echo; value["noise"]=noise; value["agc"]=agc; value["micGain"]=micGain; value["cameraID"]=cameraID;
    auto pending=path; pending+=L".pending"; std::ofstream out(pending,std::ios::binary|std::ios::trunc); out<<value.Dump(); out.close();
    return out && MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
  }
  bool LoadVolumes(const std::filesystem::path& path) {
    std::error_code ec; auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 65536) return false;
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    auto value = json::Parse(bytes); if (!value || !value->IsObject()) return false;
    volumes.clear();
    for (auto& [who, gain] : value->Items()) if (gain.type() == json::Value::Type::Number) Volume(who, gain.Number());
    return true;
  }
  bool SaveVolumes(const std::filesystem::path& path) const {
    auto value = json::Value::Object(); for (auto& [who, gain] : volumes) value[who] = gain;
    auto pending = path; pending += L".pending";
    std::ofstream out(pending, std::ios::binary | std::ios::trunc); out << value.Dump(); out.close();
    return out && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }
};
}
