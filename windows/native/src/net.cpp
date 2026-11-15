#include "net.h"

#include <chrono>
#include <vector>

#include "log.h"
#include "perf.h"

namespace net {

namespace {
// Bounds on what the server may make us hold: an API answer, and one
// WebSocket message (the largest real ones are a few KB).
constexpr size_t kMaxBody = 16 << 20;
constexpr size_t kMaxMessage = 4 << 20;
}  // namespace

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

json::Value Response::Json() const {
  auto v = json::Parse(body);
  return v ? *v : json::Value();
}

Client::Client(std::string base) : base_(std::move(base)) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
  std::wstring wbase = Wide(base_);
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.dwSchemeLength = 1;
  uc.dwUrlPathLength = 1;
  if (WinHttpCrackUrl(wbase.c_str(), 0, 0, &uc)) {
    host_ = host;
    port_ = uc.nPort;
    secure_ = uc.nScheme == INTERNET_SCHEME_HTTPS;
    local_ = host_ == L"localhost" || host_ == L"127.0.0.1" || host_ == L"::1" || host_ == L"[::1]";
  }
  if (!allowed()) {
    logf("refusing server: plain HTTP only to this machine");
    return;
  }
  session_ = WinHttpOpen(L"harmony-native/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_) WinHttpSetTimeouts(session_, 5000, 5000, 5000, 5000);
  if (session_) connect_ = WinHttpConnect(session_, host_.c_str(), port_, 0);
}

Client::~Client() {
  ClearCookie();
  if (connect_) WinHttpCloseHandle(connect_);
  if (session_) WinHttpCloseHandle(session_);
}

std::string Client::cookie() const {
  std::lock_guard<std::mutex> lock(mu_);
  return cookie_;
}

bool Client::RestoreCookie(const std::string& cookie) {
  const std::string prefix = "harmony_session=";
  if (cookie.rfind(prefix, 0) != 0 || cookie.size() != prefix.size() + 43) return false;
  for (size_t i = prefix.size(); i < cookie.size(); ++i)
    if (!((cookie[i] >= '0' && cookie[i] <= '9') || (cookie[i] >= 'a' && cookie[i] <= 'z') ||
          (cookie[i] >= 'A' && cookie[i] <= 'Z') || cookie[i] == '-' || cookie[i] == '_')) return false;
  std::lock_guard<std::mutex> lock(mu_);
  SecureZeroMemory(cookie_.data(), cookie_.size());
  cookie_ = cookie;
  return true;
}

void Client::ClearCookie() {
  std::lock_guard<std::mutex> lock(mu_);
  SecureZeroMemory(cookie_.data(), cookie_.size());
  cookie_.clear();
}

Response Client::Get(const std::string& path) { return Send(L"GET", path, nullptr); }

Response Client::Post(const std::string& path, const json::Value& body) {
  std::string text = body.Dump();
  auto response = Send(L"POST", path, &text);
  SecureZeroMemory(text.data(), text.size());
  return response;
}

Response Client::Send(const wchar_t* verb, const std::string& path, const std::string* body) {
  static perf::Stage& stage = perf::Get("http");
  auto timer = stage.Begin();
  Response r;
  if (!connect_) return r;
  HINTERNET req = WinHttpOpenRequest(connect_, verb, Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
  if (!req) return r;
  // The cookie is ours to send: WinHTTP's own cookie handling is switched off
  // so that the session is exactly the one this object holds.
  DWORD flags = WINHTTP_DISABLE_COOKIES;
  WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
  std::wstring headers;
  std::string c = cookie();
  if (!c.empty()) headers += L"Cookie: " + Wide(c) + L"\r\n";
  if (body) headers += L"Content-Type: application/json\r\n";
  BOOL ok = WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                               DWORD(-1L), body ? (void*)body->data() : WINHTTP_NO_REQUEST_DATA,
                               body ? DWORD(body->size()) : 0, body ? DWORD(body->size()) : 0, 0);
  if (ok) ok = WinHttpReceiveResponse(req, nullptr);
  if (ok) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = int(status);
    // Every Set-Cookie header, looking for the session.
    for (DWORD index = 0;;) {
      DWORD len = 0;
      WinHttpQueryHeaders(req, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, &index);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
      std::wstring value(len / sizeof(wchar_t), L'\0');
      DWORD before = index;
      if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &len,
                               &index))
        break;
      value.resize(len / sizeof(wchar_t));
      std::string v = Narrow(value);
      if (v.rfind("harmony_session=", 0) == 0) {
        std::lock_guard<std::mutex> lock(mu_);
        SecureZeroMemory(cookie_.data(), cookie_.size());
        cookie_ = v.substr(0, v.find(';'));
      }
      if (index == before) break;
    }
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(req, &avail)) { r.status = 0; r.body.clear(); break; }
      if (avail == 0) break;
      size_t at = r.body.size();
      if (at + avail > kMaxBody) {
        logf("http: answer over %zu bytes, dropped", kMaxBody);
        r.status = 0;
        r.body.clear();
        break;
      }
      r.body.resize(at + avail);
      DWORD read = 0;
      if (!WinHttpReadData(req, r.body.data() + at, avail, &read)) { r.status = 0; r.body.clear(); break; }
      r.body.resize(at + read);
    }
  }
  WinHttpCloseHandle(req);
  timer.Done();
  return r;
}

bool Client::Login(const std::string& user, const std::string& password, std::string* error,
                   const std::function<std::string(const std::string& kind)>& ask_code) {
  // Built by hand so every copy of the password is ours to wipe.
  std::string quoted = json::Value(password).Dump();
  std::string text = "{\"username\":" + json::Value(user).Dump() + ",\"password\":" + quoted + "}";
  Response r = Send(L"POST", "/api/login", &text);
  SecureZeroMemory(quoted.data(), quoted.size());
  SecureZeroMemory(text.data(), text.size());
  if (r.status == 200 && !cookie().empty()) return true;
  // Two-step verification: the password was right, and the server wants the
  // code before it opens a session (api.go handleLogin, /api/login/2fa).
  json::Value answer = r.Json();
  if (r.status == 200 && !answer["challenge"].Str().empty() && ask_code) {
    std::string code = ask_code(answer["twofa"].Str());
    json::Value body = json::Value::Object();
    body["challenge"] = answer["challenge"].Str();
    body["code"] = code;
    r = Post("/api/login/2fa", body);
    if (r.status == 200 && !cookie().empty()) return true;
  }
  if (error) {
    std::string reason = r.Json()["error"].Str();
    *error = "login " + std::to_string(r.status) + (reason.empty() ? "" : " " + reason);
  }
  return false;
}

Socket::Socket(Client* client, Handler on_event, std::function<void()> on_open,
               std::function<void()> on_close)
    : client_(client), on_event_(std::move(on_event)), on_open_(std::move(on_open)),
      on_close_(std::move(on_close)), interrupt_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

Socket::~Socket() { Stop(); if(interrupt_) CloseHandle(interrupt_); }

void Socket::Start() {
  stop_ = false;
  if(!interrupt_) return;
  ResetEvent(interrupt_);
  thread_ = std::thread([this] { Run(); });
}

void Socket::Stop() {
  stop_ = true;
  if(interrupt_) SetEvent(interrupt_);
  if (thread_.joinable()) thread_.join();
}

void Socket::Reconnect() { reconnect_ = true; if(interrupt_) SetEvent(interrupt_); }

void Socket::Run() {
  perf::NameThread(L"harmony-socket");
  int wait_ms = 500;
  while (!stop_) {
    if (ConnectOnce()) wait_ms = 500;
    if (on_close_ && !stop_) on_close_();
    if (stop_) break;
    logf("socket closed; reconnecting in %d ms", wait_ms);
    WaitForSingleObject(interrupt_,wait_ms);
    wait_ms = wait_ms * 2 > 5000 ? 5000 : wait_ms * 2;
  }
}

namespace {
// Each asynchronous handle owns its callback context until HANDLE_CLOSING,
// the final callback. Only the receive thread starts operations or closes it.
struct SocketOperation {
  HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD error = 0, bytes = 0;
  WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
  ~SocketOperation() { CloseHandle(done); CloseHandle(closed); }
  static void CALLBACK Status(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD) {
    auto* op = reinterpret_cast<SocketOperation*>(context);
    if (!op) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(op->closed); return; }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
      op->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
    } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
      auto* result = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(info);
      op->bytes = result->dwBytesTransferred; op->type = result->eBufferType;
    } else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
               status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
    SetEvent(op->done);
  }
  bool Wait(HANDLE interrupt) {
    HANDLE events[]{interrupt,done};
    return WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1 && error==0;
  }
  void Close(HINTERNET handle) {
    if (!handle) return;
    // An async operation is pending, but no WinHTTP call is executing here.
    // Close requests cancellation; wait for the final callback before freeing
    // its buffers/context. Never close a synchronous Receive concurrently.
    WinHttpCloseHandle(handle);
    WaitForSingleObject(closed, INFINITE);
  }
};
}

bool Socket::ConnectOnce() {
  ResetEvent(interrupt_);
  reconnect_ = false;
  if (!client_->allowed() || stop_) return false;
  HINTERNET session = WinHttpOpen(L"harmony-native/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
  if (!session) return false;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  WinHttpSetStatusCallback(session, SocketOperation::Status,
      WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0);
  HINTERNET connect = WinHttpConnect(session, client_->host().c_str(), client_->port(), 0);
  HINTERNET req = connect ? WinHttpOpenRequest(connect, L"GET", L"/ws", nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, client_->secure() ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  bool opened = false;
  SocketOperation http, receive;
  if (req) {
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&http);
    WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));
    DWORD flags = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    std::wstring headers = L"Cookie: " + Wide(client_->cookie()) + L"\r\nOrigin: " + Wide(client_->base()) + L"\r\n";
    if (WinHttpSendRequest(req, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, context) &&
        http.Wait(interrupt_) && WinHttpReceiveResponse(req, nullptr) && http.Wait(interrupt_)) {
      HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, reinterpret_cast<DWORD_PTR>(&receive));
      if (ws) {
        opened = true;
        logf("socket open");
        if (on_open_ && !stop_) on_open_();
        std::string message;
        std::vector<char> buf(64 * 1024);
        while (!stop_ && !reconnect_) {
          receive.error = 0;
          DWORD err = WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), nullptr, nullptr);
          if (err != NO_ERROR || !receive.Wait(interrupt_)) break;
          if (receive.type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
          if (message.size() + receive.bytes > kMaxMessage) {
            logf("socket: message over %zu bytes, closing", kMaxMessage); break;
          }
          message.append(buf.data(), receive.bytes);
          if (receive.type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
              receive.type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
          if (auto v = json::Parse(message)) {
            if (on_event_) on_event_((*v)["type"].Str(), (*v)["data"]);
          }
          message.clear();
        }
        receive.Close(ws);
      }
    }
    http.Close(req);
  }
  if (connect) WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return opened;
}

}  // namespace net
