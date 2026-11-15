#include "notices.h"
#include <algorithm>
#include <fstream>
namespace app {
namespace {
bool Allowed(const NoticeContext& c, const std::string& author) {
  return c.enabled && !c.seen && !author.empty() && author != c.me;
}
std::string Name(const ChatPerson& p) {
  return !p.nickname.empty() ? p.nickname : !p.displayName.empty() ? p.displayName : p.username;
}
}
bool MessageSound(const NoticeContext& c, const ChatMessage& m, const ChatConversation* where, const std::string& active) {
  if (!c.enabled || m.kind == "pin" || m.author.username.empty() || m.author.username == c.me || m.channel.empty() || m.deleted || (where && where->muted)) return false;
  return !c.seen || active != m.channel || std::find(m.mentions.begin(), m.mentions.end(), c.me) != m.mentions.end();
}
std::optional<Notice> MessageNotice(const NoticeContext& c, const ChatMessage& m, const ChatConversation* where) {
  if (!Allowed(c, m.author.username) || m.kind == "pin" || m.channel.empty() || m.deleted || (where && where->muted)) return {};
  const bool dm = where && where->kind == "dm";
  if (!dm && std::find(m.mentions.begin(), m.mentions.end(), c.me) == m.mentions.end()) return {};
  auto author = m.author;
  if (author.nickname.empty() && where && where->peer.username == author.username) author.nickname = where->peer.nickname;
  return Notice{Notice::Target::Conversation, m.channel,
      dm ? Name(author) : Name(author) + " em #" + (where ? where->name : ""),
      dm ? "Mensagem direta" : "Te mencionou", m.channel};
}
std::optional<Notice> CallNotice(const NoticeContext& c, const std::string& id, const CallPerson& from) {
  if (!Allowed(c, from.username) || id.empty()) return {};
  return Notice{Notice::Target::Call, id, (from.displayName.empty() ? from.username : from.displayName) + " está te ligando", "Chamada direta", "call-" + id};
}
std::optional<Notice> RoomNotice(const NoticeContext& c, const json::Value& event) {
  const auto& from = event["from"]; auto id = event["channel_id"].Str();
  if (!Allowed(c, from["username"].Str()) || id.empty()) return {};
  ChatPerson p; p.username = from["username"].Str(); p.displayName = from["display_name"].Str(); p.nickname = from["nickname"].Str();
  return Notice{Notice::Target::Room, id, Name(p) + " está te chamando", "para a sala " + event["channel_name"].Str(), "ring-" + id};
}
bool NoticePreferences::Load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary); if (!in) return false;
  std::string text(1025, '\0'); in.read(text.data(), std::streamsize(text.size())); text.resize(size_t(in.gcount()));
  if (text.size() > 1024) return false;
  auto value = json::Parse(text);
  if (!value || (*value)["enabled"].type() != json::Value::Type::Bool) return false;
  enabled = (*value)["enabled"].Bool();
  sound = (*value)["sound"].Bool(true); return true;
}
bool NoticePreferences::Save(const std::filesystem::path& path) const {
  auto pending = path; pending += L".pending";
  std::ofstream out(pending, std::ios::binary | std::ios::trunc); if (!out) return false;
  auto v = json::Value::Object(); v["enabled"] = enabled; v["sound"] = sound; out << v.Dump(); out.close();
  if (!out) return false;
  return MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
}
