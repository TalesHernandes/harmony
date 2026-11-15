#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <regex>

namespace {
constexpr size_t kKeep = 20000;  // a whole call for the diagnostic report (29/10/2026)
std::mutex mu;
std::deque<std::string> ring;
FILE* file = nullptr;
bool file_checked = false;
}  // namespace

std::string FilterLog(const std::string& text) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  // Do not try to guess where a free-form secret ends. Drop the whole line,
  // including multiline bodies, and never retain the rejected original.
  for (const char* key : {"password", "senha", "token=", "token:", "\"token\"", "cookie", "authorization", "bearer", "challenge",
                           "totp", "secret", "email", "e-mail", "ice-pwd", "ice-ufrag", "code:", "\"code\"",
                           "body=", "body:", "\"body\"", "content=", "message=", "message:", "\"message\""})
    if (lower.find(key) != std::string::npos) return "[sensitive diagnostic omitted]";
  static const std::regex codeAssignment(R"(code\s*=\s*(?!0x[0-9a-f]+(?:\s|$))[^\s]+)");
  if (lower.find("code") != std::string::npos && lower.find('=') != std::string::npos &&
      std::regex_search(lower, codeAssignment)) return "[sensitive diagnostic omitted]";
  static const std::regex unsafe(R"(([^\s@]+@[^\s@]+\.[^\s@]+)|([A-Za-z0-9_-]{32,}))");
  size_t run=0; bool longIdentifier=false;
  for (unsigned char c : text) {
    bool word=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';
    run=word?run+1:0; if(run>=32){longIdentifier=true;break;}
  }
  bool unsafeTrigger=longIdentifier || text.find('@')!=std::string::npos;
  if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos ||
      (unsafeTrigger && std::regex_search(text, unsafe)))
    return "[sensitive diagnostic omitted]";
  return text;
}

void logf(const char* fmt, ...) {
  char body[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  std::string safe = FilterLog(body);
  SecureZeroMemory(body, sizeof body);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[2100];
  std::snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, safe.c_str());

  std::lock_guard<std::mutex> lock(mu);
  if (!file_checked) {
    file_checked = true;
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("HARMONY_LOG_FILE", path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) file = std::fopen(path, "a");
  }
  ring.emplace_back(line);
  if (ring.size() > kKeep) ring.pop_front();
  std::fprintf(stderr, "%s\n", line);
  if (file) {
    std::fprintf(file, "%s\n", line);
    std::fflush(file);
  }
}

std::vector<std::string> LogLines() {
  std::lock_guard<std::mutex> lock(mu);
  return {ring.begin(), ring.end()};
}
