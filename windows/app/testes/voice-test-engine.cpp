// Unit tests link the same service against an inert engine factory. Integration
// tests link voice-engine.cpp and the real W1 engine instead.
#include "../src/voice-engine.h"
#include "../src/direct-call.h"
namespace app {
std::vector<VoiceDevice> EnumerateVoiceDevices(bool) { return {}; }
std::function<std::shared_ptr<VoiceEngine>(const VoiceState&)> testVoiceFactory;
std::shared_ptr<VoiceEngine> CreateVoiceEngine(net::Client*, const VoiceState& state, std::function<void(VoiceUpdate)>) {
  return testVoiceFactory?testVoiceFactory(state):nullptr;
}
// A voice test that "opens" at once.
struct InertVoiceTest final : VoiceTest {
  bool Running() const override { return true; }
  void Filters(bool, int, bool, bool, bool, double) override {}
};
std::unique_ptr<VoiceTest> CreateVoiceTest(net::Client*, const VoiceState&) { return std::make_unique<InertVoiceTest>(); }
std::function<std::unique_ptr<DirectMedia>()> testDirectFactory;
std::unique_ptr<DirectMedia> CreateDirectMedia(net::Client*, const VoiceState&, std::function<void(std::string, json::Value)>, std::function<void(bool)>) { return testDirectFactory ? testDirectFactory() : nullptr; }
void DirectLevels(double* mine, double* theirs) { *mine = 0; *theirs = 0; }
}
