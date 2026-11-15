#pragma once
#include "upload.h"
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace app {
struct ChatPerson {
  std::string username, displayName, avatar, nickname;
  bool admin = false;
  bool operator==(const ChatPerson&) const = default;
};
struct ChatAttachment {
  std::string id, name, mime, url;
  int64_t size = 0, width = 0, height = 0;
  bool missing = false;
  bool operator==(const ChatAttachment&) const = default;
};
// The name a download is saved under. A profile photo is named after its
// person ("bruna") and pasted pictures may have no extension at all, which
// saved a file Windows could not open (the owner, 22/10/2026): the bytes
// decide the extension when the name has none, then the type, never nothing.
inline std::string ChatDownloadName(std::string name, std::string const& mime, std::string_view bytes) {
  if (name.empty()) name = "arquivo";
  auto dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < name.size() && name.size() - dot <= 6) return name;
  auto starts = [&](std::string_view head) { return bytes.substr(0, head.size()) == head; };
  std::string ext = starts("\x89PNG") ? ".png" : starts("\xff\xd8\xff") ? ".jpg" : starts("GIF8") ? ".gif" :
    (starts("RIFF") && bytes.size() >= 12 && bytes.substr(8, 4) == "WEBP") ? ".webp" : starts("%PDF") ? ".pdf" : "";
  if (ext.empty()) ext = mime == "image/png" ? ".png" : mime == "image/jpeg" ? ".jpg" : mime == "image/gif" ? ".gif" : mime == "image/webp" ? ".webp" : mime == "text/plain" ? ".txt" : ".bin";
  return name + ext;
}
// The card the server made from a link in the message (api/preview.go): it
// read the page, so nobody's address reached the site; image is its copy.
struct ChatEmbed {
  std::string url, kind, site, title, description, image;
  int64_t width = 0, height = 0;
  bool operator==(const ChatEmbed&) const = default;
};
struct ChatReaction {
  std::string emoji;
  std::vector<std::string> users;
  bool Includes(const std::string& username) const;
  bool operator==(const ChatReaction&) const = default;
};
struct ChatReply {
  int64_t id = 0; ChatPerson author; std::string body; bool deleted = false;
  bool operator==(const ChatReply&) const = default;
};
struct ChatMessage {
  int64_t id = 0;
  std::string channel, body, createdAt, editedAt, deletedAt, deletedBy, kind;
  ChatPerson author;
  bool deleted = false;
  bool pinned = false;
  std::vector<ChatAttachment> attachments;
  std::vector<ChatEmbed> embeds;  // empty until a message_changed brings them
  std::vector<std::string> mentions;
  std::vector<ChatReaction> reactions;
  std::optional<ChatReply> reply;
  bool Mentions(const std::string& username) const;
  bool operator==(const ChatMessage&) const = default;
};
struct ChatConversation {
  std::string id, kind, name, topic;
  ChatPerson peer;
  bool muted = false, hidden = false;
  int64_t unread = 0, mentions = 0, lastMessage = 0, lastRead = 0;
  std::string category; int64_t position = 0;
};
struct ChatCategory {
  std::string id, name; int64_t position = 0;
  bool operator==(const ChatCategory&) const = default;
};
// Text channels keep server order. Empty DMs lead, then most recently active.
void ChatOrderConversations(std::vector<ChatConversation>& conversations);
struct ChatPlace { std::optional<ChatCategory> folder; std::vector<ChatConversation> texts, voices; };
std::vector<ChatPlace> ChatColumn(const std::vector<ChatConversation>& conversations,
    const std::vector<ChatConversation>& rooms, const std::vector<ChatCategory>& categories);
struct ChatDrop {
  enum class Target { Room, Folder } target = Target::Room;
  std::string id;
  enum class Where { Before, After, Into } where = Where::Into;
};
struct ChatOrder {
  std::vector<std::string> categories;
  std::vector<std::pair<std::string, std::string>> channels;
};
std::optional<ChatOrder> ChatMoveRoom(const std::vector<ChatPlace>& places, const std::string& id, const ChatDrop& drop);
std::optional<ChatOrder> ChatMoveFolder(const std::vector<ChatPlace>& places, const std::string& id, const ChatDrop& drop);
enum class ChatChangeKind { InsertEnd, InsertStart, Update, Remove, Reset, Metadata, Typing, Draft };
struct ChatChange {
  uint64_t sequence = 0;
  std::string channel;
  ChatChangeKind kind = ChatChangeKind::Metadata;
  size_t index = 0, count = 0;
  int64_t id = 0;
};
struct ChatTimeline {
  // Stable model storage, indexed under Inspect's lock. No whole-list copy on reads.
  std::deque<ChatMessage> messages;
  bool loaded = false, hasMore = true, hasNewer = false, loading = false, sending = false;
  int64_t newestKnown = 0;
  std::string error, editText;
  std::string draft;
  int64_t replyTo = 0;
};
int64_t ChatUnreadAt(const ChatTimeline& timeline, int64_t openedRead, const std::string& owner);
struct ChatTyping { std::string name; int64_t until = 0; };
// Roles (D12c), the web's ranks.ts: the list the server sends whole (GET
// /api/roles and the `roles` event), highest first. The server decides; these
// only draw it and hide the buttons it would refuse.
struct ChatRole {
  std::string id, name, color;  // color: "" or "#rrggbb" from the palette
  int64_t position = 0;         // 0 is the highest
  bool hoist = false;           // "mostrar separado na lista de pessoas"
  std::vector<std::string> permissions, members;
  bool operator==(const ChatRole&) const = default;
};
inline constexpr std::array<const char*, 3> ChatPermissionIds{"manage_channels", "manage_messages", "manage_roles"};
// Somebody's roles, highest first.
std::vector<ChatRole> ChatRolesOf(const std::string& username, const std::vector<ChatRole>& roles);
// The colour of their highest role that has one, or "".
std::string ChatColorOf(const std::string& username, const std::vector<ChatRole>& roles);
// "#rrggbb" as 0xrrggbb; nullopt for "" or anything else.
std::optional<uint32_t> ChatColorValue(const std::string& color);
// The role whose group of the people list they sit in, if they are not offline.
std::optional<ChatRole> ChatHoistOf(const std::string& username, const std::vector<ChatRole>& roles);
// The owner has every permission; anybody else the union of their roles.
std::set<std::string> ChatPermissionsOf(const std::string& username, bool owner, const std::vector<ChatRole>& roles);
// The position of their highest role: -1 for the owner, INT64_MAX for none.
int64_t ChatTopOf(const std::string& username, bool owner, const std::vector<ChatRole>& roles);
// The whole order after dragging id before or after onto, or nullopt if nothing moves.
std::optional<std::vector<std::string>> ChatMoveRole(const std::vector<ChatRole>& roles, const std::string& id, const std::string& onto, bool before);
struct ChatState {
  std::map<std::string, std::pair<bool, bool>> personal; // muted, blocked; this account only
  bool personalReady = false;
  uint64_t personalVersion = 0;
  bool Blocked(std::string const& who) const { auto p = personal.find(who); return p != personal.end() && p->second.second; }
  bool Silenced(std::string const& who, bool screen = false) const {
    auto p = personal.find(who);
    return p != personal.end() && (p->second.second || (!screen && p->second.first));
  }
  std::string owner, active, error;
  // canModerate is "Gerenciar canais" (the server's can_manage); admin is
  // the server's owner, above every role.
  bool canModerate = false, live = false, admin = false;
  std::vector<ChatRole> roles;
  std::vector<std::string> palette;  // the fifty colours, ten columns by five rows
  uint64_t rolesVersion = 0;         // bumps whenever roles change: names repaint
  bool Allowed(const std::string& permission) const;
  // Deleting somebody else's message: the owner anywhere; Gerenciar
  // mensagens in a text channel (App.tsx canModerate).
  bool Moderates(const std::string& channel) const;
  // Pinning: either person in a DM; Gerenciar mensagens in a text channel.
  bool Pins(const std::string& channel) const;
  std::vector<ChatConversation> conversations;
  std::vector<ChatCategory> categories;
  std::vector<ChatConversation> rooms;
  std::vector<ChatPerson> members;
  std::map<std::string, ChatTimeline> timelines;
  std::map<std::pair<std::string, std::string>, ChatTyping> typing;
  // Bounded journal. Apply entries after the last consumed sequence. If the
  // sequence restarted, entries were missed, or Reset is present, rebuild once
  // from Inspect; otherwise only the listed ranges need screen updates.
  std::deque<ChatChange> changes;
  uint64_t sequence = 0;
};
std::string ChatError(const net::Response& response);
std::string ChatTrim(std::string text);
std::wstring ChatInitials(const std::wstring& name);
ChatMessage ParseChatMessage(const json::Value& value);
// Composer matches substrings of username + display name, includes @todos,
// and offers at most six entries, in member order. Prefix is case insensitive.
std::vector<ChatPerson> ChatAutocomplete(const std::vector<ChatPerson>& members, std::string prefix);
struct ChatMentionToken { size_t at = 0, length = 0; std::string username; };
// UTF-8 byte offsets; outgoing body stays plain text. Server expands mentions.
std::vector<ChatMentionToken> ChatMentions(const std::string& text);
// http(s) links, as UTF-8 byte ranges, ended the way chat programs end them
// (Messages.tsx trimLink, api/preview.go): closing punctuation belongs to the
// sentence, and a ")" to the link only when the link opened one.
struct ChatLinkToken { size_t at = 0, length = 0; };
std::vector<ChatLinkToken> ChatLinks(const std::string& text);
std::optional<ChatMentionToken> ChatMentionAt(const std::string& text, size_t caret);

class Chat {
 public:
  using Transport = std::function<net::Response(const std::string& method, const std::string& path, const json::Value* body)>;
  using Clock = std::function<int64_t()>;
  explicit Chat(Transport transport, std::function<void()> changed = {},
      std::filesystem::path draftFile = {}, Clock clock = {}, bool persist = true);
  // Synchronous operations: Service::ChatWork schedules these on its worker.
  // Event and Inspect are safe from other threads. Inspect callbacks must not
  // call back into Chat, block, or retain references after the callback ends.
  void Inspect(const std::function<void(const ChatState&)>& reader) const;
  void Resync();
  void RefreshPersonal();
  std::string Personal(std::string const& who, bool block, bool enabled);
  std::function<void()> personalChanged;
  void Disconnect();
  bool Event(const std::string& type, const json::Value& data);
  void Open(std::string channel, int64_t around = 0);
  net::Response Search(const std::string& query);
  void Older(const std::string& channel);
  void Newer(const std::string& channel);
  void Send(const std::string& channel, std::string text, std::vector<std::string> attachments = {}, int64_t replyTo = 0);
  void Edit(const std::string& channel, int64_t id, std::string text);
  void Remove(const std::string& channel, int64_t id);
  void React(const std::string& channel, int64_t id, std::string emoji, bool on);
  std::string RenameChannel(const std::string& channel, std::string name);
  // kind is "text" or "voice"; the new channel's id lands in *created.
  std::string CreateChannel(std::string name, const std::string& kind, std::string* created = nullptr);
  std::string RemoveChannel(const std::string& channel);
  std::string CreateCategory(std::string name);
  std::string RenameCategory(const std::string& id, std::string name);
  std::string RemoveCategory(const std::string& id);
  std::string SaveLayout(const ChatOrder& order);
  std::string Pin(const std::string& channel, int64_t id, bool on);
  // Roles (api/roles.go). Each returns "" or the error to show, and reads the
  // list back afterwards, so the screen does not wait for the event.
  std::string CreateRole(std::string* created = nullptr);
  std::string UpdateRole(const std::string& id, const json::Value& patch);
  std::string RemoveRole(const std::string& id);
  std::string OrderRoles(const std::vector<std::string>& ids);
  std::string RoleMember(const std::string& id, const std::string& username, bool on);
  std::vector<ChatMessage> Pins(const std::string& channel, std::string& error);
  bool CanPin(const ChatMessage& message) const;
  void OpenDM(std::string username);
  net::Response Nickname(const std::string& username, std::string name);
  net::Response RingRoom(const std::string& room, const std::string& username);
  // The "Enviar diagnóstico" report; the server keeps it for the owner.
  net::Response Diagnostic(std::string report);
  void Mute(const std::string& channel, bool muted);
  void Hide(const std::string& channel);
  void BottomVisible(const std::string& channel, bool visible);
  void Typing(const std::string& channel, const std::string& text);
  void Tick();
  std::optional<int64_t> NextTickDelay() const;
  void Draft(const std::string& channel, std::string text, int64_t replyTo = 0);
  void Foreground(bool visible);
  void Forget();
  bool CanEdit(const ChatMessage& message) const;
  bool CanDelete(const ChatMessage& message) const;
  // Transport-specific binary helpers run on the same worker. Data is shared
  // without copies; cache holds <=64MiB and <=4096 entries, and clears on logout.
  net::Response Upload(net::Client& client, const std::string& channel, const std::filesystem::path& file,
      const UploadProgress& progress, const std::atomic<bool>& cancel);
  std::shared_ptr<const std::string> Image(net::Client& client, const std::string& url);
  void PublishResources();
 private:
  friend class Service;
  bool BeginSend(const std::string& channel, const std::string& text, int64_t replyTo);
  void SendQueued(const std::string& channel, std::string text, std::vector<std::string> attachments, int64_t replyTo);
  void Page(const std::string& channel, const std::string& mode, int64_t anchor = 0);
  void Upsert(const ChatMessage& message, bool changedOnly, bool insertOnly = false);
  void Notice(std::string channel, ChatChangeKind kind, size_t index = 0, size_t count = 0, int64_t id = 0);
  void Bound(const std::string& channel, bool keepOlder = false);
  void Announce();
  void LoadDrafts();
  bool SaveDrafts();
  ChatConversation* Conversation(const std::string& channel);
  void ApplyRoles(const json::Value& list);  // under mu_
  std::string RolesWrite(const std::string& what, const std::string& method, const std::string& path, const json::Value* body, std::string* created = nullptr);
  Transport transport_;
  std::function<void()> changed_;
  Clock clock_;
  std::filesystem::path draftFile_;
  mutable std::mutex mu_;
  std::recursive_mutex operations_;
  ChatState state_;
  uint64_t resourceVersion_=0, publishedResourceVersion_=~uint64_t{0};
  std::vector<std::pair<size_t,uint64_t>> resourceChannels_;
  std::vector<std::string> resourceNames_;
  std::map<std::string, int64_t> lastTyping_;
  std::vector<std::string> draftOrder_;
  bool refreshMembers_ = false, refreshChannels_ = false;
  std::map<std::string, std::vector<std::pair<std::string, json::Value>>> inFlight_;
  std::vector<std::pair<std::string, json::Value>> syncEvents_;
  bool syncing_ = false, signedOut_ = false, visible_ = true;
  // Pictures have their own lock: a download never holds operations_, so a
  // photo arriving does not hold up a message being sent, and the screen's
  // picture workers fetch side by side. The epoch drops a download that
  // finishes after the account it was for has gone.
  struct CachedImage { std::shared_ptr<const std::string> bytes; uint64_t used; };
  std::mutex imagesMu_;
  std::map<std::string, CachedImage> images_;
  uint64_t imageSequence_ = 0, imageEpoch_ = 0;
  size_t imageBytes_ = 0;
  void ForgetImages();
};
}
