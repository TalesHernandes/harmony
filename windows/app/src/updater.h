#pragma once
#include "installer-context.h"
#include "../../native/src/log.h"
#include <wincrypt.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <regex>
#include <functional>

namespace app::installer {
enum class UpdatePhase { Unavailable, Checking, Current, Downloading, Ready, Failed };
struct UpdateState {
  UpdatePhase phase=UpdatePhase::Unavailable;
  int64_t release=0, received=0, total=0;
  int Percent() const {return total>0?int(std::clamp<int64_t>(received*100/total,0,100)):0;}
};
using UpdateProgress=std::function<void(UpdateState)>;
// A separate process waits for this app to release its files before invoking
// the existing installer. No shell command, and no candidate is run directly.
inline bool ScheduleApply() {
  if(InstallRoot().empty()) return false;
  auto exe=Executable();auto command=L"\""+exe.wstring()+L"\" --apply-update-after "+std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{sizeof startup};PROCESS_INFORMATION child{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return false;
  CloseHandle(child.hThread);CloseHandle(child.hProcess);return true;
}
inline int ApplyAfter(DWORD pid) {
  auto root=InstallRoot();if(root.empty() || !pid || pid==GetCurrentProcessId())return 2;
  HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!parent)return 2;
  auto result=WaitForSingleObject(parent,60000);CloseHandle(parent);if(result!=WAIT_OBJECT_0)return 1;
  auto launcher=root.parent_path()/L"harmony.exe";auto command=L"\""+launcher.wstring()+L"\"";
  STARTUPINFOW startup{sizeof startup};PROCESS_INFORMATION child{};
  if(!CreateProcessW(launcher.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child))return 1;
  CloseHandle(child.hThread);CloseHandle(child.hProcess);return 0;
}
inline json::Value ReadJSON(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), {});
  auto value = json::Parse(bytes); return value ? *value : json::Value{};
}
inline std::string SHA256(const std::filesystem::path& file) {
  HCRYPTPROV provider{}; HCRYPTHASH hash{};
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
  std::string result;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    std::ifstream in(file, std::ios::binary); char buffer[65536]; bool ok = bool(in);
    while (ok && in) { in.read(buffer, sizeof(buffer)); auto n = in.gcount(); if (n) ok = !!CryptHashData(hash, reinterpret_cast<BYTE*>(buffer), DWORD(n), 0); }
    if (!in.eof()) ok = false;
    BYTE bytes[32]; DWORD n = sizeof(bytes);
    if (ok && CryptGetHashParam(hash, HP_HASHVAL, bytes, &n, 0)) {
      constexpr char hex[] = "0123456789abcdef";
      for (BYTE b : bytes) { result += hex[b >> 4]; result += hex[b & 15]; }
    }
    CryptDestroyHash(hash);
  }
  CryptReleaseContext(provider, 0); return result;
}
inline bool ReleaseValid(const json::Value& release, int current) {
  auto code = release["version_code"].Int(); auto size = release["size"].Int();
  return code > current && code <= 999999999 && size > 0 && size <= 512LL*1024*1024 &&
    std::regex_match(release["sha256"].Str(), std::regex("[0-9a-f]{64}")) &&
    std::regex_match(release["url"].Str(), std::regex("/api/app/windows/harmony-[1-9][0-9]{0,8}-[0-9A-Za-z._-]{1,32}\\.exe"));
}
// Streaming download owns no session beyond this call, never follows redirects,
// and never leaves an executable candidate until both size and hash match.
inline bool Download(net::Client& client, const json::Value& release,
                     const std::filesystem::path& target, const std::atomic<bool>& stop, UpdateProgress progress={}) {
  auto partial = target; partial += L".part";
  HINTERNET connection = WinHttpConnect(client.session(), client.host().c_str(), client.port(), 0);
  if (!connection) return false;
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", net::Wide(release["url"].Str()).c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, client.secure() ? WINHTTP_FLAG_SECURE : 0);
  bool ok = false;
  if (request) {
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    auto secret = client.cookie(); auto headers = L"Cookie: " + net::Wide(secret) + L"\r\n";
    ok = !!WinHttpSendRequest(request, headers.c_str(), DWORD(-1), nullptr, 0, 0, 0);
    SecureZeroMemory(secret.data(), secret.size()); SecureZeroMemory(headers.data(), headers.size()*sizeof(wchar_t));
    if (ok) ok = !!WinHttpReceiveResponse(request, nullptr);
    DWORD status{}, length = sizeof(status);
    if (ok) ok = !!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &length, nullptr) && status == 200;
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    auto started = GetTickCount64(); int64_t count = 0; char bytes[65536];
    if(progress) progress({UpdatePhase::Downloading,release["version_code"].Int(),0,release["size"].Int()});
    while (ok && !stop && GetTickCount64()-started < 300000) {
      DWORD got{};
      if (!WinHttpReadData(request, bytes, sizeof(bytes), &got)) { ok = false; break; }
      if (!got) break;
      count += got;
      if (count > release["size"].Int()) { ok = false; break; }
      out.write(bytes, got); if (!out) { ok = false; break; }
      if(progress) progress({UpdatePhase::Downloading,release["version_code"].Int(),count,release["size"].Int()});
    }
    out.close();
    // Which step failed, so a failed update is no longer a guess (24/10/2026:
    // the owner's first try of release 19 failed and nothing said why).
    if (ok && (!out || stop || count != release["size"].Int()))
      logf("update download incomplete: status=%lu bytes=%lld of %lld stopped=%d write=%d elapsed_ms=%llu",
           status, count, release["size"].Int(), int(stop.load()), int(bool(out)), GetTickCount64()-started);
    else if (!ok) logf("update download failed: status=%lu bytes=%lld windows_error=%lu", status, count, GetLastError());
    ok = ok && bool(out) && !stop && count == release["size"].Int();
    WinHttpCloseHandle(request);
  } else logf("update download failed: request not opened, windows_error=%lu", GetLastError());
  WinHttpCloseHandle(connection);
  if (ok && SHA256(partial) != release["sha256"].Str()) { ok = false; logf("update download failed: SHA-256 differs from the release"); }
  if (ok && !MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ok = false; logf("update download failed: candidate not moved into place, windows_error=%lu", GetLastError()); }
  if (!ok) DeleteFileW(partial.c_str());
  return ok;
}
inline bool CheckUpdate(const std::filesystem::path& root, const std::atomic<bool>& stop, UpdateProgress progress={}) {
  auto report=[&](UpdatePhase phase,int64_t code=0) {if(progress)progress({phase,code});};
  report(UpdatePhase::Checking);
  net::Client client(Origin()); auto secret = LoadSession(client.base());
  bool authenticated = client.RestoreCookie(secret); SecureZeroMemory(secret.data(), secret.size());
  if (!authenticated) {logf("update check skipped: no saved session");return false;}
  logf("update availability check");
  auto response = client.Get("/api/app/windows");
  if (response.status == 404 && response.Json()["error"].Str() == "no_app") {report(UpdatePhase::Current);return true;}
  if (response.status != 200) {logf("update check failed: release status %d",response.status);return false;}
  auto release = response.Json(); auto current = ReadJSON(root / L"current.json")["version"].Int();
  if (!ReleaseValid(release, 0)) {logf("update check failed: release description invalid");return false;}
  if (release["version_code"].Int() <= current) {report(UpdatePhase::Current);return true;}
  if (ReadJSON(root.parent_path() / L"failed-release.json")["version"].Int() == release["version_code"].Int()) {report(UpdatePhase::Failed);return true;}
  auto pending = root.parent_path() / L"harmony.next.exe";
  auto saved=ReadJSON(pending.wstring()+L".json");
  std::error_code error;
  if (saved["version_code"].Int()==release["version_code"].Int() && saved["sha256"].Str()==release["sha256"].Str() &&
      std::filesystem::file_size(pending,error)==release["size"].Int() && !error && SHA256(pending)==release["sha256"].Str()) {report(UpdatePhase::Ready,release["version_code"].Int());return true;}
  if (!Download(client, release, pending, stop,progress)) {logf("update download failed for release %lld",release["version_code"].Int());return false;}
  auto text = release.Dump(); auto meta = pending; meta += L".json.part";
  { std::ofstream out(meta, std::ios::binary); out << text; if (!out) return false; }
  auto final = pending; final += L".json";
  if (!MoveFileExW(meta.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
  logf("update verified; ready for next start, release %lld", release["version_code"].Int());
  report(UpdatePhase::Ready,release["version_code"].Int());return true;
}
class Updater {
 public:
  struct Timing {
    std::chrono::milliseconds initial{60000}, retry{15*60*1000}, success{6*60*60*1000}, login{2000}, cooldown{60000}, announce{30000};
  };
 private:
  std::atomic<bool> stop_{false}; std::mutex mutex_; std::condition_variable wake_; std::thread worker_;
  Timing timing_;
  mutable std::mutex stateMutex_;
  UpdateState state_;
  void Progress(UpdateState value) {std::lock_guard lock(stateMutex_);state_=value;}
  bool signedIn_=false, requested_=false, hasLogin_=false, announced_=false;
  std::chrono::steady_clock::time_point lastLogin_;
  using Probe=std::function<bool(const std::atomic<bool>&)>;
  void Start(Probe probe, Timing timing) {
    timing_=timing;
    worker_=std::thread([this,probe=std::move(probe)] {
      std::unique_lock lock(mutex_);
      auto delay=timing_.initial;
      while (!stop_) {
        bool interrupted=wake_.wait_for(lock,delay,[this] {return stop_.load() || requested_;});
        if(stop_) break;
        if(interrupted) {
          // An announced build waits a random 0-30 s (Timing::announce), so twenty programmes
          // do not pull the same 37 MB from the server in the same second.
          delay=announced_?std::chrono::milliseconds(GetTickCount64()%std::max<int64_t>(1,timing_.announce.count())):timing_.login;
          requested_=false;announced_=false;continue;
        }
        lock.unlock();
        bool ok=false;
        try {ok=probe(stop_);} catch (...) {}
        if(!ok && !stop_) {Progress({UpdatePhase::Failed});logf("update check deferred");}
        lock.lock();delay=ok?timing_.success:timing_.retry;
      }
    });
  }
 public:
  explicit Updater(bool enabled) {
    if (!enabled) return;
    auto root = InstallRoot(); if (root.empty()) return;
    Timing timing;
    if(!TestRoot().empty()) {timing.initial=std::chrono::seconds(2);timing.retry=std::chrono::seconds(3);timing.success=std::chrono::seconds(30);timing.login=std::chrono::milliseconds(250);timing.cooldown=std::chrono::seconds(5);}
    Start([this,root](auto const& stop) {return CheckUpdate(root,stop,[this](UpdateState state){Progress(state);});},timing);
  }
#ifdef HARMONY_UPDATER_TESTS
  Updater(Probe probe,Timing timing) {Start(std::move(probe),timing);}
#endif
  void SessionChanged(bool signedIn) {
    if(!worker_.joinable()) return;
    std::lock_guard lock(mutex_);
    auto now=std::chrono::steady_clock::now();
    if(signedIn && !signedIn_ && (!hasLogin_ || now-lastLogin_>=timing_.cooldown)) {
      requested_=true;hasLogin_=true;lastLogin_=now;wake_.notify_all();
    }
    signedIn_=signedIn;
  }
  // The server says a newer build was published (gateway "app_release").
  // The check that follows is the ordinary one: session, size and hash.
  void ReleaseAnnounced() {
    if(!worker_.joinable()) return;
    std::lock_guard lock(mutex_);
    requested_=true;announced_=true;wake_.notify_all();
  }
  UpdateState Read() const {std::lock_guard lock(stateMutex_);return state_;}
  ~Updater() { { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); if (worker_.joinable()) worker_.join(); }
};
}
