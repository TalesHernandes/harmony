// Standalone: compile with /DW6_SELFTEST_MAIN; no WinUI or Service required.
// Fixture invites are supplied by the local server harness, never persisted.
#include "direct-call.h"
#include <bcrypt.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace app {
namespace {
std::string Env(const wchar_t* key) {
  wchar_t value[4096]; auto n = GetEnvironmentVariableW(key, value, DWORD(std::size(value)));
  return n && n < std::size(value) ? net::Narrow(std::wstring(value,n)) : "";
}
void Require(bool value, const char* stage) { if (!value) throw std::runtime_error(stage); }
std::string Random() {
  unsigned char bytes[16]; Require(BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "random_failed");
  std::string text; for (auto c : bytes) { text += "0123456789abcdef"[c >> 4]; text += "0123456789abcdef"[c & 15]; } return text;
}
struct BenchMedia : DirectMedia {
  std::function<void(std::string,json::Value)> send;
  std::vector<std::string>* received;
  explicit BenchMedia(decltype(send) s, std::vector<std::string>* r) : send(std::move(s)), received(r) {}
  bool Open(bool polite, const json::Value&, std::string*) override {
    if (!polite) {
      auto offer = json::Value::Object(); offer["type"] = "offer"; offer["sdp"] = "synthetic-w6-description"; send("offer",offer);
      auto ice = json::Value::Object(); ice["candidate"] = "synthetic-w6-candidate"; ice["sdpMid"] = "0"; ice["sdpMLineIndex"] = 0; send("ice",ice);
    }
    return true;
  }
  void Remote(const std::string& kind, const json::Value&) override {
    received->push_back(kind);
    if (kind == "offer") { auto answer = json::Value::Object(); answer["type"] = "answer"; answer["sdp"] = "synthetic-w6-answer"; send("answer",answer); }
  }
  void Mute(bool) override {}
  void Close() override {}
};
struct Account {
  struct Event { std::string type; json::Value data; };
  net::Client client;
  std::mutex mu; std::deque<Event> pending;
  std::atomic<bool> opened = false;
  std::vector<Event> history;
  std::vector<std::string> received;
  std::unique_ptr<DirectCall> call;
  std::unique_ptr<net::Socket> socket;
  std::string username;
  int64_t offset = 0;
  explicit Account(const std::string& base) : client(base) {
    call = std::make_unique<DirectCall>([this](const auto& method, const auto& path, const auto* body) {
      return method == "GET" ? client.Get(path) : client.Post(path, body ? *body : json::Value::Object());
    }, [this](auto send, auto) { return std::make_unique<BenchMedia>(send,&received); }, nullptr, [this] { return int64_t(GetTickCount64()) + offset; });
  }
  ~Account() { if (socket) socket->Stop(); call->Disconnect(); if (!client.cookie().empty()) client.Post("/api/logout", json::Value::Object()); }
  void Signup(const std::string& invite, const std::string& password) {
    username = "w6-" + Random().substr(0,16);
    auto body = json::Value::Object(); body["code"] = invite; body["username"] = username;
    body["display_name"] = username; body["password"] = password; body["email"] = username + "@example.invalid";
    auto r = client.Post("/api/signup", body);
    Require(r.status == 201 && !client.cookie().empty(), "signup_failed");
    socket = std::make_unique<net::Socket>(&client, [this](const auto& type, const auto& data) {
      std::lock_guard lock(mu); pending.push_back({type,data});
    }, [this] { opened = true; }); socket->Start();
  }
  void Pump() {
    std::deque<Event> events;
    { std::lock_guard lock(mu); events.swap(pending); }
    for (auto& e : events) { history.push_back(e); call->Event(e.type,e.data); }
  }
  bool Heard(const std::string& type, const std::string& id) const {
    for (const auto& e : history) if (e.type == type && e.data["id"].Str() == id) return true;
    return false;
  }
};
}
int DirectSelfTest(const std::filesystem::path& output, const std::string& base) {
  auto result = json::Value::Object(); result["passed"] = false;
  try {
    net::Client check(base); Require(!check.secure() && check.allowed(), "local_server_required");
    auto ia = Env(L"HARMONY_TEST_INVITE_A"), ib = Env(L"HARMONY_TEST_INVITE_B"), ic = Env(L"HARMONY_TEST_INVITE_C");
    Require(!ia.empty() && !ib.empty() && !ic.empty(), "fixture_invites_missing");
    Account a(base), b(base), c(base); auto password = Random();
    a.Signup(ia,password); b.Signup(ib,password); c.Signup(ic,password); SecureZeroMemory(password.data(),password.size());
    auto wait = [&](const auto& condition, int ms = 10000) {
      const auto end = GetTickCount64() + ms;
      do { a.Pump(); b.Pump(); c.Pump(); if (condition()) return true; Sleep(10); } while (GetTickCount64() < end);
      return false;
    };
    Require(wait([&] { return a.opened && b.opened && c.opened; }), "socket_open_timeout");
    a.call->Ring({b.username,b.username}); auto id = a.call->View().id; Require(!id.empty(), "start_failed");
    Require(wait([&] { return b.call->View().phase == CallPhase::Ringing; }), "incoming_timeout");
    Require(b.received.empty(), "signal_before_accept");
    b.call->Accept(); Require(b.call->View().phase == CallPhase::Connecting, "accept_failed");
    Require(wait([&] { return b.received.size() >= 2 && !a.received.empty(); }), "signal_timeout");
    Require(a.Heard("call_accepted",id) && b.received == std::vector<std::string>{"offer","ice"} && a.received == std::vector<std::string>{"answer"}, "signal_order_failed");
    result["ordered_offer_answer_ice"] = true;
    a.call->Mute(true); Require(wait([&] { return b.call->View().peerMuted; }), "mute_timeout"); result["peer_state"] = true;
    c.call->Ring({a.username,a.username}); Require(c.call->View().notice == "Essa pessoa já está em outra chamada.", "busy_failed"); result["busy"] = true;
    a.call->Hangup(); Require(wait([&] { return a.Heard("call_ended",id) && b.Heard("call_ended",id); }), "ended_timeout"); result["both_ended"] = true;
    a.call->Ring({b.username,b.username}); id = a.call->View().id;
    Require(!id.empty() && wait([&] { return b.call->View().phase == CallPhase::Ringing; }), "reject_ring_timeout");
    b.call->Decline(); Require(wait([&] { return a.call->View().notice == "Chamada recusada." && a.Heard("call_ended",id) && b.Heard("call_ended",id); }), "reject_timeout"); result["rejected"] = true;
    // Reverse direction avoids the unanswered-call cooldown; this is a real
    // 45-second wait, so GET also checks the server's expiration, not just Tick.
    b.call->Ring({a.username,a.username}); id = b.call->View().id;
    Require(!id.empty() && wait([&] { return a.call->View().phase == CallPhase::Ringing; }), "timeout_ring_failed");
    Require(wait([&] { a.call->Tick(); b.call->Tick(); return a.call->View().phase == CallPhase::Idle && b.call->View().phase == CallPhase::Idle; }, 47000), "ring_deadline_failed");
    Require(b.client.Get("/api/calls/" + id).status == 404, "server_expiration_failed"); result["real_45s_expiration"] = true;
    // B and C have their own call. A has neither read nor write access.
    b.call->Ring({c.username,c.username}); id = b.call->View().id;
    Require(!id.empty() && wait([&] { return c.call->View().phase == CallPhase::Ringing; }), "private_ring_failed");
    c.call->Accept(); Require(wait([&] { return b.Heard("call_accepted",id); }), "private_accept_failed");
    Require(a.client.Get("/api/calls/" + id).status == 404, "foreign_read_allowed");
    auto signal = json::Value::Object(); signal["kind"] = "offer"; signal["payload"]["type"] = "offer"; signal["payload"]["sdp"] = "synthetic";
    Require(a.client.Post("/api/calls/" + id + "/signal",signal).status == 404, "foreign_signal_allowed");
    result["foreign_read_and_signal_404"] = true;
    b.call->Hangup(); Require(wait([&] { return c.Heard("call_ended",id); }), "private_end_timeout");
    result["passed"] = true; result["stage"] = "complete";
  } catch (const std::exception& e) {
    // Only constant stage names raised above, never HTTP contents or credentials.
    result["stage"] = e.what();
  }
  std::ofstream out(output,std::ios::binary | std::ios::trunc); out << result.Dump(); out.close();
  return out && result["passed"].Bool() ? 0 : 1;
}
}
#ifdef W6_SELFTEST_MAIN
int main(int argc, char** argv) {
  if (argc != 3) { std::cerr << "usage: harmony-nativo.exe resultado.json http://127.0.0.1:PORT\n"; return 2; }
  return app::DirectSelfTest(std::filesystem::path(net::Wide(argv[1])),argv[2]);
}
#endif
