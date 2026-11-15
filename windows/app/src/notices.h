#pragma once
#include "chat.h"
#include "direct-call.h"
namespace app {
struct Notice {
  enum class Target { Conversation, Call, Room } target;
  std::string id, title, body, tag;
};
struct NoticeContext { std::string me; bool seen = false; bool enabled = true; };
bool MessageSound(const NoticeContext&, const ChatMessage&, const ChatConversation* where, const std::string& active);
std::optional<Notice> MessageNotice(const NoticeContext&, const ChatMessage&, const ChatConversation* where);
// CallPerson has no nickname field: pass nameOf's resolved name as displayName.
// Room events can carry from.nickname, resolved by the Service's member cache.
std::optional<Notice> CallNotice(const NoticeContext&, const std::string& callId, const CallPerson& from);
std::optional<Notice> RoomNotice(const NoticeContext&, const json::Value& ringEvent);
struct NoticePreferences {
  bool enabled = true, sound = true;
  bool Load(const std::filesystem::path&);
  bool Save(const std::filesystem::path&) const;
};
}
