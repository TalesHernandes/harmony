#pragma once
#include <string>
namespace app {
// Invite links belong to the configured server. Only their code is sent to
// /api/signup; pasted URLs never become a request destination or a log line.
inline std::string InviteCode(std::string text, const std::string& base) {
  auto first=text.find_first_not_of(" \t\r\n");
  if (first==std::string::npos) return {};
  text=text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);
  auto prefix=base+"/signup?code=";
  if (text.starts_with(prefix)) text=text.substr(prefix.size(),text.find_first_of("&#",prefix.size())-prefix.size());
  if (text.empty() || text.size()>256 || text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos) return {};
  return text;
}
}
