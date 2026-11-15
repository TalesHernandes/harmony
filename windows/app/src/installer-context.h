#pragma once
#include "platform.h"
#include "../../native/src/net.h"
#include <stdexcept>
#include <fstream>
namespace app::installer {
inline std::filesystem::path TestRoot() {
  auto value = Environment(L"HARMONY_INSTALLER_TEST_ROOT");
  if (value.empty()) {
#ifdef HARMONY_INSTALLER_TEST_ONLY
    throw std::runtime_error("test-only executable requires isolated context");
#endif
    // An installed fixture must never fall back to the real user profile,
    // including launches by Explorer, where the harness environment is absent.
    for (auto p = Executable().parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
      if (std::filesystem::is_regular_file(p / L".harmony-installer-test"))
        throw std::runtime_error("fixture executable lost isolated context");
    return {};
  }
  std::filesystem::path root(net::Wide(value));
  if (!root.is_absolute() || !std::filesystem::is_regular_file(root / L".harmony-installer-test"))
    throw std::runtime_error("installer test marker missing");
  net::Client origin(Environment(L"HARMONY_URL"));
  if (origin.host() != L"127.0.0.1" && origin.host() != L"localhost")
    throw std::runtime_error("installer tests require loopback");
  return root;
}
inline std::string Origin() {
  auto origin = Environment(L"HARMONY_URL");
  if (origin.empty()) origin = "https://harmony.taleshernandes.com";
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  return origin;
}
inline std::filesystem::path InstallRoot() {
  auto exe = Executable();
  auto root = exe.parent_path().parent_path();
  return std::filesystem::is_regular_file(root / L".harmony-native-install") &&
         std::filesystem::is_regular_file(root.parent_path() / L".harmony-installer") ? root : std::filesystem::path{};
}
inline void Ready(bool authenticated = true) {
  auto event = Environment(L"HARMONY_INSTALLER_READY_EVENT");
  if (event.rfind("Local\\harmony-installer-ready-", 0) != 0 || InstallRoot().empty()) return;
  HANDLE handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, net::Wide(event).c_str());
  if (handle) { SetEvent(handle); CloseHandle(handle); }
  if (authenticated) {
    auto name=net::Wide(event)+L"-authenticated";
    handle=OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str());
    if(handle) {SetEvent(handle);CloseHandle(handle);}
  }
}
}
