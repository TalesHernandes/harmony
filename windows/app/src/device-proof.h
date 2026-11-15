#pragma once
#include "voice-state.h"
namespace app {
inline bool SelectedDeviceReady(const VoiceState& v,const std::string& input,const std::string& output) {
  return !input.empty() && !output.empty() && v.input==input && v.output==output &&
    v.inputReady && v.outputReady && v.captureCallbacks>0 && v.renderCallbacks>0;
}
inline bool DefaultDeviceRecovered(const VoiceState& before,const VoiceState& after) {
  return !before.input.empty() && !before.output.empty() && after.input.empty() && after.output.empty() &&
    after.inputReady && after.outputReady && after.sessionGeneration==before.sessionGeneration &&
    after.connectionIdentity==before.connectionIdentity && after.sessionIdentity==before.sessionIdentity &&
    after.captureCallbacks>before.captureCallbacks && after.renderCallbacks>before.renderCallbacks;
}
}
