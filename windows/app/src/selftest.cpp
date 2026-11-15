#include "service.h"
#include "platform.h"
#include "../../native/src/log.h"
#include <fstream>

namespace app {
namespace {
int64_t EpochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool WriteResult(const std::filesystem::path& path, const json::Value& value) {
  auto pending = path; pending += L".pending";
  std::ofstream out(pending, std::ios::binary | std::ios::trunc); out << value.Dump(); out.close();
  return out && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}
int SelfTest(const std::filesystem::path& output, const std::string& base) {
  json::Value result = json::Value::Object(); result["passed"] = false;
  auto fail = [&](const char* reason) { result["stage"] = reason; WriteResult(output, result); return 1; };
  net::Client check(base);
  if (check.secure() || !check.allowed()) return fail("local_server_required");
  auto user = Environment(L"HARMONY_TEST_USER"), password = Environment(L"HARMONY_TEST_PASSWORD");
  auto visitor = Environment(L"HARMONY_TEST_VISITOR");
  if (user.empty() || password.empty() || visitor.empty()) return fail("fixture_environment_missing");
  std::mutex mutex; std::condition_variable changed;
  State observed;
  std::string roomID;
  int64_t entered = 0, left = 0;
  std::unique_ptr<Service> service;
  service = std::make_unique<Service>(base, [&] {
    auto state = service->Read();
    std::lock_guard lock(mutex);
    observed = std::move(state);
    if (!roomID.empty()) {
      bool present = false;
      for (auto const& room : observed.model.rooms) if (room.id == roomID)
        for (auto const& person : room.people) if (person.username == visitor) present = true;
      if (present && !entered) entered = EpochMs();
      if (!present && entered && !left && observed.model.live) left = EpochMs();
    }
    changed.notify_all();
  }, false); // The real UI service, with test-only persistence disabled.
  service->Restore();
  std::unique_lock lock(mutex);
  bool ok = changed.wait_for(lock,std::chrono::seconds(10),[&] { return !observed.busy; });
  if (!ok) { lock.unlock(); service->Stop(); return fail("initial_state_timeout"); }
  lock.unlock(); service->Login(std::move(user), std::move(password)); lock.lock();
  ok = changed.wait_for(lock,std::chrono::seconds(20),[&] {
    return observed.needsCode || (observed.signedIn && observed.model.live) || (!observed.busy && !observed.error.empty());
  });
  if (ok && observed.needsCode) {
    auto code = Environment(L"HARMONY_TEST_CODE");
    if (code.empty()) { lock.unlock(); service->Stop(); return fail("factor_environment_missing"); }
    lock.unlock(); service->Code(std::move(code)); lock.lock();
    ok = changed.wait_for(lock,std::chrono::seconds(20),[&] {
      return (observed.signedIn && observed.model.live) || (!observed.busy && !observed.error.empty());
    });
    result["twofa_verified"] = ok && observed.signedIn;
  }
  if (!ok || !observed.signedIn || observed.model.rooms.empty()) { lock.unlock(); service->Stop(); return fail("login_or_snapshot_failed"); }
  roomID = observed.model.rooms.front().id;
  lock.unlock(); service->Reconnect(); lock.lock();
  ok = changed.wait_for(lock,std::chrono::seconds(20),[&] { return observed.gatewayConnections >= 2; });
  if (!ok) { lock.unlock(); service->Stop(); return fail("reconnect_failed"); }
  result["connections"] = observed.gatewayConnections;
  result["uses_ui_service"] = true;
  result["test_session_persisted"] = false;
  result["stage"] = "ready"; result["room_id"] = roomID; result["room_count"] = int(observed.model.rooms.size());
  WriteResult(output,result);
  ok = changed.wait_for(lock,std::chrono::seconds(20),[&] { return entered != 0; });
  if (!ok) { lock.unlock(); service->Stop(); return fail("join_event_timeout"); }
  result["stage"] = "joined"; result["join_observed_epoch_ms"] = entered; WriteResult(output,result);
  ok = changed.wait_for(lock,std::chrono::seconds(20),[&] { return left != 0; });
  if (!ok) { lock.unlock(); service->Stop(); return fail("leave_event_timeout"); }
  lock.unlock(); service->Logout(); lock.lock();
  ok = changed.wait_for(lock,std::chrono::seconds(10),[&] { return !observed.signedIn; });
  lock.unlock(); service->Stop();
  if (!ok) return fail("logout_failed");
  result["stage"] = "left"; result["leave_observed_epoch_ms"] = left; result["passed"] = true;
  result["logout_verified"] = true;
  return WriteResult(output,result) ? 0 : 1;
}
}
