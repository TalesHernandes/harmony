#include "upload.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>

namespace app {
namespace {
struct Handle {
  HINTERNET value = nullptr;
  ~Handle() { if (value) WinHttpCloseHandle(value); }
};
struct Request {
  Handle session, connection, request;
  Request(net::Client& client, const std::string& method, const std::string& path, bool async = false) {
    if (!client.allowed() || !client.session() || path.empty() || path[0] != '/' ||
        path.starts_with("//") || path.find_first_of("\r\n\\") != std::string::npos) return;
    auto source = client.session();
    if (async) {
      session.value = WinHttpOpen(L"harmony-upload", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
      source = session.value;
      if (!source) return;
    }
    connection.value = WinHttpConnect(source, client.host().c_str(), client.port(), 0);
    if (!connection.value) return;
    request.value = WinHttpOpenRequest(connection.value, net::Wide(method).c_str(),
        net::Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        client.secure() ? WINHTTP_FLAG_SECURE : 0);
    if (!request.value) return;
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    WinHttpSetTimeouts(request.value, 5000, 5000, 5000, 5000);
    auto cookie = client.cookie();
    auto headers = L"Cookie: " + net::Wide(cookie) + L"\r\n";
    WinHttpAddRequestHeaders(request.value, headers.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
    SecureZeroMemory(cookie.data(), cookie.size());
  }
  HINTERNET get() const { return request.value; }
};
// Cancellation is observed on the owning thread. It closes an asynchronous
// request only after WinHTTP returned, then retains callback state/buffers until
// HANDLE_CLOSING. No synchronous operation is closed from another thread.
class UploadRequest {
 public:
  UploadRequest(net::Client& client, const std::string& path, const std::atomic<bool>& cancel)
      : request_(client, "POST", path, true), cancel_(cancel) {
    if (!get() || !done_.value || !closed_.value) return;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
    if (!WinHttpSetOption(get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context)) return;
    ready_ = WinHttpSetStatusCallback(get(), Status,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) != WINHTTP_INVALID_STATUS_CALLBACK;
  }
  ~UploadRequest() {
    if (!ready_) return;
    WinHttpCloseHandle(get()); request_.request.value = nullptr;
    WaitForSingleObject(closed_, INFINITE);
  }
  HINTERNET get() const { return request_.get(); }
  bool Wait(BOOL started) {
    if (!ready_ || !started) return false;
    while (!cancel_) {
      if (WaitForSingleObject(done_, 20) == WAIT_OBJECT_0) return !error_ && !cancel_;
    }
    return false;
  }
  bool Send(const std::wstring& headers, DWORD length) {
    return ready_ && !cancel_ && Wait(WinHttpSendRequest(get(), headers.c_str(), DWORD(-1),
        nullptr, 0, length, reinterpret_cast<DWORD_PTR>(this)));
  }
  bool Write(const char* bytes, DWORD count) {
    std::copy_n(bytes, count, buffer_.data());
    return !cancel_ && Wait(WinHttpWriteData(get(), buffer_.data(), count, nullptr)) && bytes_ == count;
  }
  net::Response Answer(size_t limit) {
    if (cancel_ || !Wait(WinHttpReceiveResponse(get(), nullptr))) return {};
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return {};
    net::Response result{int(status), {}};
    for (;;) {
      if (cancel_ || !Wait(WinHttpReadData(get(), buffer_.data(), DWORD(buffer_.size()), nullptr))) return {};
      if (!bytes_) return result;
      if (result.body.size() + bytes_ > limit) return {};
      result.body.append(buffer_.data(), bytes_);
    }
  }
 private:
  struct Event {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { CloseHandle(value); }
    operator HANDLE() const { return value; }
  } done_, closed_;
  static void CALLBACK Status(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length) {
    auto* self = reinterpret_cast<UploadRequest*>(context);
    if (!self) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(self->closed_); return; }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
      self->error_ = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
    else if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) self->bytes_ = *static_cast<DWORD*>(info);
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self->bytes_ = length;
    else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
    SetEvent(self->done_);
  }
  Request request_;
  const std::atomic<bool>& cancel_;
  bool ready_ = false;
  DWORD error_ = 0, bytes_ = 0;
  std::array<char,65536> buffer_{};
};
net::Response ReadAnswer(HINTERNET request, size_t limit) {
  net::Response result;
  if (!request || !WinHttpReceiveResponse(request, nullptr)) return result;
  DWORD status = 0, size = sizeof status;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return result;
  result.status = int(status);
  std::array<char, 65536> buffer;
  for (;;) {
    DWORD count = 0;
    if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &count)) return {};
    if (!count) return result;
    if (result.body.size() + count > limit) return {};
    result.body.append(buffer.data(), count);
  }
}
}
std::string ChatEscape(const std::string& value) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
net::Response ChatRequest(net::Client& client, const std::string& method,
    const std::string& path, const json::Value* body) {
  // Keep the existing authenticated client for the methods it already supports.
  if (method == "GET") return client.Get(path);
  if (method == "POST") return client.Post(path, body ? *body : json::Value::Object());
  Request request(client, method, path);
  if (!request.get()) return {};
  auto text = body ? body->Dump() : std::string{};
  BOOL sent = WinHttpSendRequest(request.get(), body ? L"Content-Type: application/json\r\n" : nullptr,
      body ? DWORD(-1) : 0, text.empty() ? nullptr : text.data(), DWORD(text.size()), DWORD(text.size()), 0);
  SecureZeroMemory(text.data(), text.size());
  return sent ? ReadAnswer(request.get(), 16 << 20) : net::Response{};
}
net::Response UploadFile(net::Client& client, const std::string& channel,
    const std::filesystem::path& file, const UploadProgress& progress, const std::atomic<bool>& cancel) {
  if (cancel) return {0, R"({"error":"canceled"})"};
  std::error_code error;
  auto length = std::filesystem::file_size(file, error);
  if (error) return {0, R"({"error":"offline"})"};
  if (length > (32u << 20)) return {413, R"({"error":"too_large"})"};
  std::ifstream input(file, std::ios::binary);
  if (!input) return {};
  // A non-constant boundary; no dependency is needed for a multipart delimiter.
  const auto boundary = "harmony-" + std::to_string(GetTickCount64()) + "-" + std::to_string(GetCurrentThreadId());
  auto filename = net::Narrow(file.filename().wstring());
  std::erase_if(filename, [](unsigned char c) { return c < 32 || c == 127 || c == '"' || c == '\\'; });
  const auto prefix = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" +
      filename + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
  const auto suffix = "\r\n--" + boundary + "--\r\n";
  UploadRequest request(client, "/api/channels/" + ChatEscape(channel) + "/uploads", cancel);
  if (!request.get()) return {};
  const auto headers = net::Wide("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  if (!request.Send(headers, DWORD(prefix.size() + length + suffix.size()))) return {};
  auto write = [&](const char* bytes, size_t count) {
    while (count) {
      if (cancel) return false;
      DWORD written = DWORD(std::min<size_t>(count, 65536));
      if (!request.Write(bytes, written)) return false;
      bytes += written; count -= written;
    }
    return true;
  };
  uint64_t sent = 0;
  bool ok = write(prefix.data(), prefix.size());
  std::array<char, 65536> buffer;
  if (progress) progress(0, length);
  while (ok && sent < length) {
    auto count = std::min<uint64_t>(buffer.size(), length - sent);
    input.read(buffer.data(), std::streamsize(count));
    if (uint64_t(input.gcount()) != count) { ok = false; break; }
    ok = write(buffer.data(), size_t(count));
    if (ok) { sent += count; if (progress) progress(sent, length); }
  }
  if (ok) ok = write(suffix.data(), suffix.size());
  if (cancel) return {0, R"({"error":"canceled"})"};
  auto result = ok ? request.Answer(1 << 20) : net::Response{};
  if (cancel) return {0, R"({"error":"canceled"})"};
  return result;
}
net::Response UploadAvatar(net::Client& client, const std::filesystem::path& file) {
  std::error_code error;
  auto size = std::filesystem::file_size(file, error);
  if (error) return {400, R"({"error":"bad_image"})"};
  if (size > (5u << 20)) return {413, R"({"error":"avatar_too_large"})"};
  std::ifstream input(file, std::ios::binary);
  std::string bytes(size_t(size), '\0');
  if (!input.read(bytes.data(), std::streamsize(size))) return {400, R"({"error":"bad_image"})"};
  // Same raw body as api.putAvatar. The server checks magic bytes, dimensions
  // and the hourly limit; file extensions and MIME labels are not authority.
  Request request(client, "PUT", "/api/me/avatar");
  if (!request.get()) return {};
  if (!WinHttpSendRequest(request.get(), L"Content-Type: application/octet-stream\r\n", DWORD(-1),
      bytes.data(), DWORD(bytes.size()), DWORD(bytes.size()), 0)) return {};
  return ReadAnswer(request.get(), 1 << 20);
}
net::Response DownloadBytes(net::Client& client, const std::string& url, size_t limit) {
  auto path = url;
  // Never forward the session to another origin or follow a redirect there.
  if (!path.starts_with('/')) {
    const auto prefix = client.base() + "/";
    if (!path.starts_with(prefix)) return {404, R"({"error":"not_found"})"};
    path = path.substr(client.base().size());
  }
  Request request(client, "GET", path);
  if (!request.get() || !WinHttpSendRequest(request.get(), nullptr, 0, nullptr, 0, 0, 0)) return {};
  return ReadAnswer(request.get(), limit);
}
}
