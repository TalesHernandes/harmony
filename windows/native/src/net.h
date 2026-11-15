// HTTP and the live socket to the harmony server, over WinHTTP (part of
// Windows, the same thing the D8 shell uses for its updates).
//
// The session is the `harmony_session` cookie (PROTOCOL-HARMONY §1). It lives
// only in memory here: the W1 program is a proof, not the app, and the password
// comes from the environment for each run.
#pragma once
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "json.h"

namespace net {

struct Response {
  int status = 0;  // 0 = the request never got an answer
  std::string body;
  json::Value Json() const;
};

class Client {
 public:
  // base: "http://localhost:8091" or "https://harmony.taleshernandes.com".
  explicit Client(std::string base);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Response Get(const std::string& path);
  Response Post(const std::string& path, const json::Value& body);

  // POST /api/login and keep the cookie. False with the reason on failure.
  // ask_code is called when the account has two-step verification (D10):
  // it gets "email" or "totp" and returns the code the person typed.
  bool Login(const std::string& user, const std::string& password, std::string* error,
             const std::function<std::string(const std::string& kind)>& ask_code = nullptr);

  const std::string& base() const { return base_; }
  std::string cookie() const;
  // Restored only from the native app's user-bound encrypted session store.
  bool RestoreCookie(const std::string& cookie);
  void ClearCookie();
  HINTERNET session() const { return session_; }
  const std::wstring& host() const { return host_; }
  INTERNET_PORT port() const { return port_; }
  bool secure() const { return secure_; }
  // HTTPS, or plain HTTP to this machine only (the bench's test server): the
  // password and the session never cross a network in the clear.
  bool allowed() const { return secure_ || local_; }

 private:
  Response Send(const wchar_t* verb, const std::string& path, const std::string* body);

  std::string base_;
  std::wstring host_;
  INTERNET_PORT port_ = 0;
  bool secure_ = false;
  bool local_ = false;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  mutable std::mutex mu_;
  std::string cookie_;  // "harmony_session=<value>"
};

// The live socket, /ws. It only receives (the server ignores what is sent);
// every event is {"type", "data"}. Reconnects with a doubling wait from 0.5 s
// to 5 s, like the web client, and calls on_open after every (re)connection,
// because the server sends no snapshot: whoever is listening has to fetch
// again (PROTOCOL-HARMONY §2).
class Socket {
 public:
  using Handler = std::function<void(const std::string& type, const json::Value& data)>;
  Socket(Client* client, Handler on_event, std::function<void()> on_open,
         std::function<void()> on_close = nullptr);
  ~Socket();
  void Start();
  void Stop();
  // Request a new handshake without joining the receive thread (also safe
  // from on_open when a snapshot request failed).
  void Reconnect();

 private:
  void Run();
  bool ConnectOnce();

  Client* client_;
  Handler on_event_;
  std::function<void()> on_open_;
  std::function<void()> on_close_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> reconnect_{false};
  HANDLE interrupt_=nullptr;
  std::thread thread_;

};

std::wstring Wide(const std::string& s);
std::string Narrow(const std::wstring& w);

}  // namespace net
