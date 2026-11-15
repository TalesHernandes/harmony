#include "chat.h"
#include "../../native/src/log.h"
#include "../../native/src/resource-usage.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace app {
namespace {
ChatPerson Person(const json::Value& v) {
  return {v["username"].Str(), v["display_name"].Str(), v["avatar"].Str(), v["nickname"].Str(), v["is_admin"].Bool()};
}
ChatConversation Channel(const json::Value& v) {
  return {v["id"].Str(), v["kind"].Str(), v["name"].Str(), v["topic"].Str(), Person(v["peer"]),
      v["muted"].Bool(), false, v["unread"].Int(), v["mentions"].Int(), v["last_message_id"].Int(), v["last_read_id"].Int(),
      v["category_id"].Str(), v["position"].Int()};
}
ChatCategory Category(const json::Value& v) {
  return {v["id"].Str(), v["name"].Str(), v["position"].Int()};
}
ChatRole Role(const json::Value& v) {
  ChatRole r{v["id"].Str(), v["name"].Str(), v["color"].Str(), v["position"].Int(), v["hoist"].Bool()};
  for (const auto& p : v["permissions"].Elements()) r.permissions.push_back(p.Str());
  for (const auto& m : v["members"].Elements()) r.members.push_back(m.Str());
  return r;
}
void ApplyLayout(ChatState& state, const json::Value& layout) {
  state.categories.clear();
  for (const auto& c : layout["categories"].Elements()) state.categories.push_back(Category(c));
  for (auto* list : {&state.conversations, &state.rooms}) for (auto& c : *list)
    for (const auto& p : layout["channels"].Elements()) if (p["id"].Str() == c.id) {
      c.category = p["category_id"].Str(); c.position = p["position"].Int(); break;
    }
}
bool NameByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}
std::string Lower(std::string s) {
  auto wide = net::Wide(s);
  if (!wide.empty()) CharLowerBuffW(wide.data(), DWORD(wide.size()));
  return net::Narrow(wide);
}
}
std::string ChatTrim(std::string s) {
  // JS trim's whitespace set, operating on Windows UTF-16 to retain UTF-8 text.
  auto w = net::Wide(s);
  auto space = [](wchar_t c) { return (c >= 9 && c <= 13) || c == 32 || c == 0xa0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff; };
  size_t first = 0, last = w.size();
  while (first < last && space(w[first])) ++first;
  while (last > first && space(w[last - 1])) --last;
  return net::Narrow(w.substr(first, last - first));
}
std::wstring ChatInitials(const std::wstring& name) {
  auto space = [](wchar_t c) { return (c >= 9 && c <= 13) || c == 32 || c == 0xa0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff; };
  std::wstring first, last;
  for (size_t i = 0; i < name.size(); ++i) if ((i == 0 || space(name[i - 1])) && !space(name[i])) {
    WORD type = 0; GetStringTypeW(CT_CTYPE1, &name[i], 1, &type);
    if (!(type & (C1_ALPHA | C1_DIGIT))) continue;
    if (first.empty()) first = name[i]; else last = name[i];
  }
  auto result = first.empty() ? std::wstring(L"?") : first + last;
  CharUpperBuffW(result.data(), DWORD(result.size())); return result;
}
void ChatOrderConversations(std::vector<ChatConversation>& conversations) {
  std::stable_sort(conversations.begin(), conversations.end(), [](const auto& a, const auto& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.kind != "dm") return false;
    const auto rank = [](int64_t id) { return id ? id : (std::numeric_limits<int64_t>::max)(); };
    return rank(a.lastMessage) > rank(b.lastMessage);
  });
}
namespace {
// JS localeCompare uses linguistic order, including accented names. Windows'
// collation handles UTF-16 rather than comparing UTF-8 bytes.
int CompareNames(const std::string& a, const std::string& b) {
  auto x = net::Wide(a), y = net::Wide(b);
  auto compared = CompareStringEx(LOCALE_NAME_USER_DEFAULT, SORT_STRINGSORT,
      x.c_str(), -1, y.c_str(), -1, nullptr, nullptr, 0);
  return compared ? compared - CSTR_EQUAL : a.compare(b);
}
struct PlainPlace {
  std::string id;
  std::vector<std::string> texts, voices;
  bool operator==(const PlainPlace&) const = default;
};
std::vector<PlainPlace> Plain(const std::vector<ChatPlace>& places) {
  std::vector<PlainPlace> result;
  for (const auto& p : places) {
    PlainPlace plain; plain.id = p.folder ? p.folder->id : "";
    for (const auto& c : p.texts) plain.texts.push_back(c.id);
    for (const auto& c : p.voices) plain.voices.push_back(c.id);
    result.push_back(std::move(plain));
  }
  return result;
}
bool Includes(const std::vector<std::string>& list, const std::string& id) {
  return std::find(list.begin(), list.end(), id) != list.end();
}
ChatOrder Flatten(const std::vector<PlainPlace>& places) {
  ChatOrder order;
  for (const auto& p : places) {
    if (!p.id.empty()) order.categories.push_back(p.id);
    for (const auto& id : p.texts) order.channels.emplace_back(id, p.id);
    for (const auto& id : p.voices) order.channels.emplace_back(id, p.id);
  }
  return order;
}
}
std::vector<ChatPlace> ChatColumn(const std::vector<ChatConversation>& conversations,
    const std::vector<ChatConversation>& rooms, const std::vector<ChatCategory>& categories) {
  auto folders = categories;
  std::stable_sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) {
    return a.position != b.position ? a.position < b.position : CompareNames(a.name, b.name) < 0;
  });
  std::vector<ChatPlace> places(1);
  for (const auto& f : folders) places.push_back({f, {}, {}});
  for (const auto* list : {&conversations, &rooms}) for (const auto& c : *list) {
    if (c.kind == "dm") continue;
    auto at = std::find_if(places.begin(), places.end(), [&](const auto& p) { return p.folder && p.folder->id == c.category; });
    if (at == places.end()) at = places.begin();
    (c.kind == "voice" ? at->voices : at->texts).push_back(c);
  }
  auto less = [](const auto& a, const auto& b) {
    if (a.position != b.position) return a.position < b.position;
    auto name = CompareNames(a.name, b.name);
    return name ? name < 0 : CompareNames(a.id, b.id) < 0;
  };
  for (auto& p : places) {
    std::stable_sort(p.texts.begin(), p.texts.end(), less);
    std::stable_sort(p.voices.begin(), p.voices.end(), less);
  }
  return places;
}
std::optional<ChatOrder> ChatMoveRoom(const std::vector<ChatPlace>& places, const std::string& id, const ChatDrop& drop) {
  auto plain = Plain(places), before = plain;
  auto from = std::find_if(plain.begin(), plain.end(), [&](const auto& p) { return Includes(p.texts, id) || Includes(p.voices, id); });
  if (from == plain.end()) return {};
  const bool text = Includes(from->texts, id);
  std::erase(text ? from->texts : from->voices, id);
  if (drop.target == ChatDrop::Target::Room) {
    if (drop.id == id) return {};
    auto to = std::find_if(plain.begin(), plain.end(), [&](const auto& p) { return Includes(p.texts, drop.id) || Includes(p.voices, drop.id); });
    if (to == plain.end()) return {};
    auto& list = text ? to->texts : to->voices;
    auto i = std::find(list.begin(), list.end(), drop.id);
    if (i == list.end()) list.insert(text ? list.end() : list.begin(), id);
    else list.insert(drop.where == ChatDrop::Where::Before ? i : i + 1, id);
  } else if (drop.where == ChatDrop::Where::Into) {
    auto to = std::find_if(plain.begin(), plain.end(), [&](const auto& p) { return p.id == drop.id; });
    if (to == plain.end()) return {};
    (text ? to->texts : to->voices).push_back(id);
  } else return {};
  return plain == before ? std::nullopt : std::optional<ChatOrder>(Flatten(plain));
}
std::optional<ChatOrder> ChatMoveFolder(const std::vector<ChatPlace>& places, const std::string& id, const ChatDrop& drop) {
  if (drop.target == ChatDrop::Target::Room || drop.where == ChatDrop::Where::Into || drop.id == id || places.empty()) return {};
  auto plain = Plain(places);
  auto from = std::find_if(plain.begin() + 1, plain.end(), [&](const auto& p) { return p.id == id; });
  if (from == plain.end()) return {};
  auto moving = *from;
  auto before = Flatten(plain).categories;
  plain.erase(from);
  auto to = std::find_if(plain.begin() + 1, plain.end(), [&](const auto& p) { return p.id == drop.id; });
  if (to == plain.end()) return {};
  plain.insert(drop.where == ChatDrop::Where::Before ? to : to + 1, moving);
  auto order = Flatten(plain);
  return order.categories == before ? std::nullopt : std::optional<ChatOrder>(order);
}
int64_t ChatUnreadAt(const ChatTimeline& timeline, int64_t openedRead, const std::string& owner) {
  if (openedRead > 0) for (const auto& m : timeline.messages)
    if (m.id > openedRead && m.author.username != owner) return m.id;
  return 0;
}
namespace {
std::string LimitRunes(const std::string& s, size_t limit) {
  size_t i = 0, count = 0;
  for (; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80 && ++count > limit) break;
  }
  return s.substr(0, i);
}
bool Good(const net::Response& r) { return r.status >= 200 && r.status < 300; }
std::filesystem::path DefaultDraftFile() {
  wchar_t root[32768];
  auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, DWORD(std::size(root)));
  return n && n < std::size(root) ? std::filesystem::path(root) / L"harmony-nativo" / L"chat-drafts.json" : std::filesystem::path{};
}
}
bool ChatReaction::Includes(const std::string& username) const {
  return std::find(users.begin(), users.end(), username) != users.end();
}
bool ChatMessage::Mentions(const std::string& username) const {
  return std::find(mentions.begin(), mentions.end(), username) != mentions.end();
}
ChatMessage ParseChatMessage(const json::Value& v) {
  ChatMessage m;
  m.id = v["id"].Int(); m.channel = v["channel_id"].Str(); m.author = Person(v["author"]);
  m.body = v["body"].Str(); m.kind = v["kind"].Str(); m.createdAt = v["created_at"].Str();
  m.editedAt = v["edited_at"].Str(); m.deletedAt = v["deleted_at"].Str(); m.deletedBy = v["deleted_by"].Str();
  m.deleted = v["deleted"].Bool(); m.pinned = v["pinned"].Bool();
  for (const auto& a : v["attachments"].Elements()) m.attachments.push_back({a["id"].Str(), a["name"].Str(),
      a["mime"].Str(), a["url"].Str(), a["size"].Int(), a["width"].Int(), a["height"].Int(), a["missing"].Bool()});
  for (const auto& e : v["embeds"].Elements()) m.embeds.push_back({e["url"].Str(), e["kind"].Str(), e["site"].Str(),
      e["title"].Str(), e["description"].Str(), e["image"].Str(), e["width"].Int(), e["height"].Int()});
  for (const auto& u : v["mentions"].Elements()) m.mentions.push_back(u.Str());
  for (const auto& r : v["reactions"].Elements()) {
    ChatReaction reaction; reaction.emoji = r["emoji"].Str();
    for (const auto& u : r["users"].Elements())
      if (!reaction.Includes(u.Str())) reaction.users.push_back(u.Str());
    m.reactions.push_back(std::move(reaction));
  }
  if (v["reply_to"]["id"].Int() > 0) {
    const auto& r = v["reply_to"];
    m.reply = ChatReply{r["id"].Int(), Person(r["author"]), r["body"].Str(), r["deleted"].Bool()};
  }
  return m;
}
std::vector<ChatRole> ChatRolesOf(const std::string& username, const std::vector<ChatRole>& roles) {
  std::vector<ChatRole> out;
  for (const auto& r : roles) if (std::find(r.members.begin(), r.members.end(), username) != r.members.end()) out.push_back(r);
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
  return out;
}
std::string ChatColorOf(const std::string& username, const std::vector<ChatRole>& roles) {
  for (const auto& r : ChatRolesOf(username, roles)) if (!r.color.empty()) return r.color;
  return {};
}
std::optional<uint32_t> ChatColorValue(const std::string& color) {
  if (color.size() != 7 || color[0] != '#') return std::nullopt;
  uint32_t out = 0;
  for (size_t i = 1; i < 7; ++i) {
    char c = color[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return std::nullopt;
    out = out << 4 | uint32_t(d);
  }
  return out;
}
std::optional<ChatRole> ChatHoistOf(const std::string& username, const std::vector<ChatRole>& roles) {
  for (const auto& r : ChatRolesOf(username, roles)) if (r.hoist) return r;
  return std::nullopt;
}
std::set<std::string> ChatPermissionsOf(const std::string& username, bool owner, const std::vector<ChatRole>& roles) {
  std::set<std::string> out;
  if (owner) { for (auto p : ChatPermissionIds) out.insert(p); return out; }
  for (const auto& r : ChatRolesOf(username, roles)) out.insert(r.permissions.begin(), r.permissions.end());
  return out;
}
int64_t ChatTopOf(const std::string& username, bool owner, const std::vector<ChatRole>& roles) {
  if (owner) return -1;
  auto mine = ChatRolesOf(username, roles);
  return mine.empty() ? std::numeric_limits<int64_t>::max() : mine.front().position;
}
std::optional<std::vector<std::string>> ChatMoveRole(const std::vector<ChatRole>& roles, const std::string& id, const std::string& onto, bool before) {
  if (id == onto) return std::nullopt;
  auto sorted = roles;
  std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
  std::vector<std::string> ids, rest;
  for (const auto& r : sorted) { ids.push_back(r.id); if (r.id != id) rest.push_back(r.id); }
  auto at = std::find(rest.begin(), rest.end(), onto);
  if (at == rest.end() || rest.size() == ids.size()) return std::nullopt;
  rest.insert(before ? at : at + 1, id);
  if (rest == ids) return std::nullopt;
  return rest;
}
bool ChatState::Allowed(const std::string& permission) const {
  return ChatPermissionsOf(owner, admin, roles).contains(permission);
}
bool ChatState::Moderates(const std::string& channel) const {
  if (admin) return true;
  for (const auto& c : conversations) if (c.id == channel) return c.kind == "text" && Allowed("manage_messages");
  return false;
}
bool ChatState::Pins(const std::string& channel) const {
  for (const auto& c : conversations) if (c.id == channel) return c.kind == "dm" || (c.kind == "text" && Allowed("manage_messages"));
  return false;
}
std::string ChatError(const net::Response& r) {
  auto code = r.Json()["error"].Str();
  if (code == "canceled") return {};
  if (!r.status || code == "offline") return "Sem conexão com o servidor.";
  static const std::map<std::string, std::string> errors = {
    {"delivery_failed", "Não foi possível entregar."},
    {"message_limit", "Você mandou mensagens rápido demais. Espere um minuto."},
    {"upload_limit", "Limite de arquivos por dia atingido."},
    {"upload_bytes_limit", "Você já mandou muita coisa hoje. Amanhã dá de novo."},
    {"too_large", "O arquivo é grande demais (o limite é 32 MB)."},
    {"bad_file_type", "Esse tipo de arquivo não passa: imagem, PDF, texto, som, vídeo ou zip."},
    {"bad_channel_name", "Nome de canal inválido: letras minúsculas, números e traço."},
    {"name_taken", "Já existe um com esse nome."},
    {"bad_category_name", "Dê um nome à categoria, com até 32 caracteres."},
    {"stale_layout", "A lista mudou enquanto você arrastava. Ela foi recarregada; tente de novo."},
    {"pin_limit", "Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra."},
    {"bad_message", "Mensagem vazia ou comprida demais."},
    {"bad_reaction", "Só dá para reagir com emoji, e com no máximo seis por mensagem."},
    {"last_channel", "Esse é o último canal de texto: o app precisa de um lugar para abrir."},
    {"room_busy", "Tem gente na sala. Espere ela esvaziar para apagar."},
    {"not_allowed", "Você não tem permissão para isso."}, {"not_found", "Não encontrado."},
    {"bad_role_name", "Dê um nome ao cargo, com até 32 caracteres."},
    {"bad_color", "Escolha uma das cores da lista."},
    {"role_limit", "O servidor já tem 50 cargos. Apague um para criar outro."},
    {"stale_roles", "A lista de cargos mudou enquanto você arrastava. Ela foi recarregada; tente de novo."},
    {"unauthenticated", "Sua sessão terminou. Entre de novo."},
    {"email_required", "Confirme seu e-mail para continuar."},
    {"rate_limited", "Tentativas demais. Espere quinze minutos."}
  };
  auto found = errors.find(code);
  return found == errors.end() ? "Deu algo errado. Tente de novo." : found->second;
}
std::vector<ChatPerson> ChatAutocomplete(const std::vector<ChatPerson>& members, std::string prefix) {
  std::vector<ChatPerson> all{{"todos", "Todo mundo daqui", "", "", false}}, out;
  all.insert(all.end(), members.begin(), members.end()); prefix = Lower(prefix);
  for (const auto& person : all) {
    if (Lower(person.username + " " + person.displayName).find(prefix) != std::string::npos) out.push_back(person);
    if (out.size() == 6) break;
  }
  return out;
}
std::optional<ChatMentionToken> ChatMentionAt(const std::string& text, size_t caret) {
  caret = std::min(caret, text.size());
  size_t at = caret;
  while (at && NameByte(static_cast<unsigned char>(text[at - 1]))) --at;
  if (!at || text[at - 1] != '@') return {};
  return ChatMentionToken{at - 1, caret - at + 1, Lower(text.substr(at, caret - at))};
}
std::vector<ChatMentionToken> ChatMentions(const std::string& text) {
  // Mirrors Messages' plain @name tokens; no client-provided mention ids.
  std::vector<ChatMentionToken> out;
  for (size_t at = 0; at < text.size(); ++at) if (text[at] == '@') {
    size_t end = at + 1;
    while (end < text.size() && end < at + 33 && NameByte(static_cast<unsigned char>(text[end]))) ++end;
    auto alnum = [](unsigned char c) { return NameByte(c) && c != '_' && c != '.' && c != '-'; };
    while (end > at + 1 && !alnum(static_cast<unsigned char>(text[end-1]))) --end;
    if (end >= at + 4 && alnum(static_cast<unsigned char>(text[at+1])))
      out.push_back({at, end - at, text.substr(at + 1, end - at - 1)});
    at = end - 1;
  }
  return out;
}
std::vector<ChatLinkToken> ChatLinks(const std::string& text) {
  std::vector<ChatLinkToken> out;
  auto lower = Lower(text);
  for (size_t at = 0; at < text.size();) {
    size_t http = lower.find("http://", at), https = lower.find("https://", at);
    size_t start = std::min(http, https);
    if (start == std::string::npos) break;
    size_t end = start;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '<' && text[end] != '>') ++end;
    while (end > start) {
      char c = text[end - 1];
      if (std::string_view(".,;:!?'\"").find(c) != std::string_view::npos) { --end; continue; }
      if (c == ')') {
        auto piece = std::string_view(text).substr(start, end - start);
        if (std::count(piece.begin(), piece.end(), '(') < std::count(piece.begin(), piece.end(), ')')) { --end; continue; }
      }
      break;
    }
    auto scheme = lower.compare(start, 8, "https://") == 0 ? 8u : 7u;
    if (end > start + scheme) out.push_back({start, end - start});
    at = std::max(end, start + scheme);
  }
  return out;
}
Chat::Chat(Transport transport, std::function<void()> changed, std::filesystem::path draftFile, Clock clock, bool persist)
    : transport_(std::move(transport)), changed_(std::move(changed)), clock_(std::move(clock)),
      draftFile_(persist ? (draftFile.empty() ? DefaultDraftFile() : std::move(draftFile)) : std::filesystem::path{}) {
  if (!clock_) clock_ = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count(); };
}
void Chat::Inspect(const std::function<void(const ChatState&)>& reader) const { std::lock_guard lock(mu_); reader(state_); }
void Chat::Announce() { if (changed_) changed_(); }
void Chat::Notice(std::string channel, ChatChangeKind kind, size_t index, size_t count, int64_t id) {
  ++resourceVersion_;
  state_.changes.push_back({++state_.sequence, std::move(channel), kind, index, count, id});
  if (state_.changes.size() > 1024) state_.changes.pop_front();
}
ChatConversation* Chat::Conversation(const std::string& channel) {
  for (auto& c : state_.conversations) if (c.id == channel) return &c;
  return nullptr;
}
void Chat::ApplyRoles(const json::Value& list) {
  std::vector<ChatRole> roles;
  for (const auto& r : list.Elements()) if (!r["id"].Str().empty()) roles.push_back(Role(r));
  std::stable_sort(roles.begin(), roles.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
  if (roles == state_.roles) return;
  state_.roles = std::move(roles); ++state_.rolesVersion;
  // can_manage is "Gerenciar canais" (api/channels.go); the roles carry it
  // live, so the column's tools come and go without a new GET.
  state_.canModerate = state_.Allowed("manage_channels");
  Notice({}, ChatChangeKind::Metadata);
}
// One write to the roles, then the list read back: the `roles` event brings
// it too, but this screen should not wait on the socket. Logged without
// names, as the web's run() does.
std::string Chat::RolesWrite(const std::string& what, const std::string& method, const std::string& path, const json::Value* body, std::string* created) {
  std::lock_guard operation(operations_);
  auto r = transport_(method, path, body);
  if (Good(r) && created) *created = r.Json()["id"].Str();
  auto error = Good(r) ? std::string{} : ChatError(r);
  if (Good(r)) logf("roles: %s", what.c_str());
  else logf("roles: %s refused (%d %s)", what.c_str(), r.status, r.Json()["error"].Str().c_str());
  auto fresh = transport_("GET", "/api/roles", nullptr);
  if (Good(fresh) && fresh.Json()["roles"].IsArray()) { std::lock_guard lock(mu_); ApplyRoles(fresh.Json()["roles"]); }
  Announce();
  return error;
}
std::string Chat::CreateRole(std::string* created) {
  std::string name = "Novo cargo";
  {
    std::lock_guard lock(mu_);
    auto taken = [&](const std::string& n) { return std::any_of(state_.roles.begin(), state_.roles.end(), [&](const auto& r) { return Lower(r.name) == Lower(n); }); };
    for (int i = 2; taken(name); ++i) name = "Novo cargo " + std::to_string(i);
  }
  auto body = json::Value::Object(); body["name"] = name; body["color"] = ""; body["hoist"] = false; body["permissions"] = json::Value::Array();
  return RolesWrite("create", "POST", "/api/roles", &body, created);
}
std::string Chat::UpdateRole(const std::string& id, const json::Value& patch) {
  std::string what = patch.Has("name") ? "rename" : patch.Has("color") ? "color" : patch.Has("hoist") ? "hoist" : "permissions";
  return RolesWrite(what, "PATCH", "/api/roles/" + ChatEscape(id), &patch);
}
std::string Chat::RemoveRole(const std::string& id) {
  return RolesWrite("delete", "DELETE", "/api/roles/" + ChatEscape(id), nullptr);
}
std::string Chat::OrderRoles(const std::vector<std::string>& ids) {
  auto body = json::Value::Object(); body["roles"] = json::Value::Array();
  for (const auto& id : ids) body["roles"].Push(id);
  return RolesWrite("order", "PUT", "/api/roles/order", &body);
}
std::string Chat::RoleMember(const std::string& id, const std::string& username, bool on) {
  return RolesWrite(on ? "give" : "take", on ? "PUT" : "DELETE", "/api/roles/" + ChatEscape(id) + "/members/" + ChatEscape(username), nullptr);
}
void Chat::Bound(const std::string& channel, bool keepOlder) {
  auto& t = state_.timelines[channel];
  // Only the visible history may grow. Its hard ceiling still permits paging
  // all 500 integration messages; crossing it drops the opposite boundary.
  size_t limit = channel == state_.active ? 2000 : 50;
  if (t.messages.size() <= limit) return;
  auto count = t.messages.size() - limit;
  if (keepOlder && channel == state_.active) {
    t.messages.erase(t.messages.end() - count, t.messages.end()); t.hasNewer = true;
    Notice(channel, ChatChangeKind::Remove, limit, count); return;
  }
  t.messages.erase(t.messages.begin(), t.messages.begin() + count);
  t.hasMore = true;
  Notice(channel, ChatChangeKind::Remove, 0, count);
}
void Chat::Upsert(const ChatMessage& m, bool changedOnly, bool insertOnly) {
  if (m.id <= 0 || m.channel.empty()) return;
  auto found = state_.timelines.find(m.channel);
  if (found == state_.timelines.end() || !found->second.loaded) return;
  auto& t = found->second;
  auto at = std::lower_bound(t.messages.begin(), t.messages.end(), m.id, [](const auto& a, int64_t id) { return a.id < id; });
  auto index = size_t(at - t.messages.begin());
  if (at != t.messages.end() && at->id == m.id) {
    if (*at == m || insertOnly) return;
    *at = m; Notice(m.channel, ChatChangeKind::Update, index, 1, m.id); return;
  }
  if (changedOnly) return;
  if (t.hasNewer) { t.newestKnown = std::max(t.newestKnown, m.id); return; }
  auto kind = index == 0 && !t.messages.empty() ? ChatChangeKind::InsertStart :
      index == t.messages.size() ? ChatChangeKind::InsertEnd : ChatChangeKind::Reset;
  t.messages.insert(at, m); Notice(m.channel, kind, index, 1, m.id); Bound(m.channel);
}
void Chat::RefreshPersonal() {
  std::lock_guard operation(operations_);
  auto response = transport_("GET", "/api/me/blocks", nullptr);
  auto document = response.Json();
  {
    std::lock_guard lock(mu_);
    auto previous = state_.personal; bool wasReady = state_.personalReady;
    const bool fresh = Good(response) && document["items"].IsArray();
    // Once known, stays known: a failed refresh keeps the last list and its choices.
    state_.personalReady = fresh || wasReady;
    // A failed refresh must not unfold a known blocked person's history.
    if (fresh) state_.personal.clear();
    if (fresh) for (auto const& item : document["items"].Elements()) {
      auto who = item["username"].Str();
      if (!who.empty() && who != state_.owner) state_.personal[who] = {item["muted"].Bool(), item["blocked"].Bool()};
    }
    if (previous != state_.personal || wasReady != state_.personalReady) {
      ++state_.personalVersion;
      for (auto const& [id, timeline] : state_.timelines) Notice(id, ChatChangeKind::Reset);
      Notice({}, ChatChangeKind::Metadata);
    }
  }
  if (personalChanged) personalChanged();
  Announce();
}
std::string Chat::Personal(std::string const& who, bool block, bool enabled) {
  std::lock_guard operation(operations_);
  auto response = transport_(enabled ? "PUT" : "DELETE", std::string("/api/me/") + (block ? "blocks/" : "mutes/") + ChatEscape(who), nullptr);
  if (!Good(response)) return ChatError(response);
  logf("personal preference changed");
  Resync();
  return {};
}
void Chat::Resync() {
  std::lock_guard operation(operations_);
  { std::lock_guard lock(mu_); syncing_ = true; syncEvents_.clear(); }
  auto me = transport_("GET", "/api/me", nullptr);
  auto channels = transport_("GET", "/api/channels", nullptr);
  auto members = transport_("GET", "/api/members", nullptr);
  // No roles is no colours and no role buttons; the server decides anyway.
  auto roles = transport_("GET", "/api/roles", nullptr);
  const auto channelsJSON = channels.Json(), membersJSON = members.Json(), rolesJSON = roles.Json();
  std::vector<std::pair<std::string, json::Value>> events;
  std::string active;
  {
    std::lock_guard lock(mu_);
    auto user = me.Json()["user"]["username"].Str();
    bool valid = Good(me) && !user.empty() && Good(channels) && channels.Json()["channels"].IsArray() &&
        Good(members) && members.Json()["members"].IsArray();
    if (valid) {
      if (state_.owner != user) {
        state_ = {}; state_.owner = user; signedOut_ = false; lastTyping_.clear(); ForgetImages();
        draftOrder_.clear(); LoadDrafts();
      }
      state_.canModerate = channels.Json()["can_manage"].Bool();
      state_.admin = me.Json()["user"]["is_admin"].Bool();
      if (Good(roles) && rolesJSON["roles"].IsArray()) {
        state_.palette.clear();
        for (const auto& c : rolesJSON["colors"].Elements()) state_.palette.push_back(c.Str());
        ApplyRoles(rolesJSON["roles"]);
      }
      state_.conversations.clear(); state_.rooms.clear();
      state_.categories.clear();
      for (const auto& c : channelsJSON["categories"].Elements()) state_.categories.push_back(Category(c));
      for (const auto& c : channelsJSON["channels"].Elements()) {
        auto ch = Channel(c);
        if (!ch.id.empty() && ch.kind == "voice") state_.rooms.push_back(ch);
        if (!ch.id.empty() && (ch.kind == "text" || ch.kind == "dm")) state_.conversations.push_back(std::move(ch));
      }
      state_.members.clear();
      for (const auto& p : membersJSON["members"].Elements()) state_.members.push_back(Person(p));
      // Drop data for conversations whose membership has been revoked.
      for (auto& [id, timeline] : state_.timelines) if (!Conversation(id)) {
        // Hidden conversations are absent too. Keep only our own unsent text;
        // never retain somebody else's history after membership disappears.
        timeline.messages.clear(); timeline.loaded = false; timeline.replyTo = 0;
        Notice(id, ChatChangeKind::Reset);
      }
      state_.live = true; state_.error.clear(); active = state_.active;
      if (!active.empty() && !Conversation(active)) { state_.active.clear(); active.clear(); }
      Notice({}, ChatChangeKind::Metadata);
    } else {
      state_.live = false;
      state_.error = ChatError(!Good(me) ? me : !Good(channels) ? channels : members);
    }
    syncing_ = false; events.swap(syncEvents_);
  }
  for (const auto& [type, data] : events) Event(type, data);
  RefreshPersonal();
  if (!active.empty()) Page(active, "latest");
  Announce();
}
void Chat::Disconnect() {
  { std::lock_guard lock(mu_); state_.live = false; state_.typing.clear(); Notice({}, ChatChangeKind::Metadata); }
  Announce();
}
bool Chat::Event(const std::string& type, const json::Value& data) {
  bool handled = true;
  {
    std::lock_guard lock(mu_);
    if (signedOut_) return false;
    if (syncing_) { syncEvents_.emplace_back(type, data); return true; }
    const auto channel = data["channel_id"].Str();
    if ((type == "message" || type == "message_changed") && inFlight_.contains(channel))
      inFlight_[channel].emplace_back(type, data);
    if (type == "message" || type == "message_changed") {
      const auto m = ParseChatMessage(data);
      if (m.id <= 0 || m.channel.empty()) return false;
      auto* c = Conversation(m.channel);
      if (!c && type == "message") refreshChannels_ = true;
      bool fresh = c && m.id > c->lastMessage;
      Upsert(m, type == "message_changed");
      if (type == "message" && c) {
        if (fresh) {
          const bool quiet = !state_.personalReady || state_.Blocked(m.author.username) || c->muted || m.channel == state_.active || m.author.username == state_.owner;
          if (!quiet) ++c->unread;
          if (state_.personalReady && !state_.Blocked(m.author.username) && !c->muted && m.channel != state_.active && m.Mentions(state_.owner)) ++c->mentions;
        }
        c->lastMessage = std::max(c->lastMessage, m.id); c->hidden = false;
        state_.typing.erase({m.channel, m.author.username});
        Notice(m.channel, ChatChangeKind::Metadata);
      }
    } else if (type == "typing") {
      auto who = data["username"].Str();
      if (!channel.empty() && !who.empty() && who != state_.owner) {
        state_.typing[{channel, who}] = {data["display_name"].Str().empty() ? who : data["display_name"].Str(), clock_() + 5000};
        Notice(channel, ChatChangeKind::Typing);
      }
    } else if (type == "read") {
      if (auto* c = Conversation(channel)) {
        auto read = data["last_read_id"].Int();
        if (read >= c->lastRead) { c->lastRead = read; c->unread = 0; c->mentions = 0; }
        Notice(channel, ChatChangeKind::Metadata);
      }
    } else if (type == "layout") {
      ApplyLayout(state_, data); Notice({}, ChatChangeKind::Metadata);
    } else if (type == "channel") {
      auto c = Channel(data);
      if (c.id.empty() || (c.kind != "text" && c.kind != "dm" && c.kind != "voice")) return false;
      if (c.kind == "voice") {
        auto at = std::find_if(state_.rooms.begin(), state_.rooms.end(), [&](const auto& r) { return r.id == c.id; });
        if (at == state_.rooms.end()) state_.rooms.push_back(c); else *at = c;
      } else if (auto* old = Conversation(c.id)) { c.unread = old->unread; c.mentions = old->mentions; *old = c; }
      else state_.conversations.push_back(c);
      Notice(c.id, ChatChangeKind::Metadata);
    } else if (type == "channel_removed") {
      auto id = data["id"].Str();
      std::erase_if(state_.conversations, [&](const auto& c) { return c.id == id; });
      std::erase_if(state_.rooms, [&](const auto& c) { return c.id == id; });
      state_.timelines.erase(id);
      inFlight_.erase(id);
      if (state_.active == id) state_.active.clear();
      Notice(id, ChatChangeKind::Reset);
    } else if (type == "profile") {
      auto who = data["username"].Str(), avatar = data["avatar"].Str();
      for (auto& person : state_.members) if (person.username == who) person.avatar = avatar;
      for (auto& c : state_.conversations) if (c.peer.username == who) c.peer.avatar = avatar;
      for (auto& [id, t] : state_.timelines) for (size_t i = 0; i < t.messages.size(); ++i) {
        auto& m = t.messages[i]; bool changed = false;
        if (m.author.username == who) { m.author.avatar = avatar; changed = true; }
        if (m.reply && m.reply->author.username == who) { m.reply->author.avatar = avatar; changed = true; }
        if (changed) Notice(id, ChatChangeKind::Update, i, 1, m.id);
      }
      Notice({}, ChatChangeKind::Metadata);
    } else if (type == "roles") {
      // A role was made, changed, moved, deleted, given or taken: the whole
      // list comes every time, like the column's layout.
      if (!data["roles"].IsArray()) return false;
      ApplyRoles(data["roles"]);
    } else if (type == "presence") {
      auto who = data["username"].Str();
      if (!who.empty() && std::none_of(state_.members.begin(), state_.members.end(), [&](const auto& p) { return p.username == who; })) refreshMembers_ = true;
    } else handled = false;
  }
  if (handled) Announce();
  return handled;
}
net::Response Chat::Search(const std::string& query) {
  std::lock_guard operation(operations_);
  return transport_("GET", "/api/search?q=" + ChatEscape(ChatTrim(query)), nullptr);
}
void Chat::Open(std::string channel, int64_t around) {
  std::lock_guard operation(operations_);
  {
    std::lock_guard lock(mu_);
    auto previous = state_.active;
    if (previous != channel) {
      if (!previous.empty()) { state_.timelines[previous].replyTo = 0; }
      state_.active = channel;
      if (!previous.empty()) {
        auto& old = state_.timelines[previous];
        if (old.hasNewer) { old.messages.clear(); old.loaded = false; Notice(previous, ChatChangeKind::Reset); }
        else Bound(previous);
      }
      Notice(channel, ChatChangeKind::Metadata);
    }
  }
  Page(channel, around > 0 ? "around" : "latest", around);
}
void Chat::Older(const std::string& channel) {
  std::lock_guard operation(operations_);
  int64_t anchor;
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel];
    if (!t.hasMore || t.messages.empty()) return; anchor = t.messages.front().id; }
  Page(channel, "before", anchor);
}
void Chat::Newer(const std::string& channel) {
  std::lock_guard operation(operations_);
  int64_t anchor;
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel];
    if (!t.hasNewer || t.messages.empty()) return; anchor = t.messages.back().id; }
  Page(channel, "after", anchor);
}
void Chat::Page(const std::string& channel, const std::string& mode, int64_t anchor) {
  { std::lock_guard lock(mu_); state_.timelines[channel].loading = true; inFlight_[channel].clear(); }
  Announce();
  auto path = "/api/channels/" + ChatEscape(channel) + "/messages";
  if (mode != "latest") path += "?" + mode + "=" + std::to_string(anchor);
  auto response = transport_("GET", path, nullptr);
  {
    std::lock_guard lock(mu_);
    if (!inFlight_.contains(channel)) return; // Channel removed while GET was in flight.
    auto buffered = std::move(inFlight_[channel]); inFlight_.erase(channel);
    auto& t = state_.timelines[channel]; t.loading = false;
    auto page = response.Json();
    if (!Good(response) || !page["messages"].IsArray()) t.error = ChatError(response);
    else {
      t.error.clear();
      std::map<int64_t, ChatMessage> combined;
      bool replace = mode == "around" || !t.loaded;
      int64_t pageFirst = 0;
      for (const auto& m : page["messages"].Elements()) if (!pageFirst || m["id"].Int() < pageFirst) pageFirst = m["id"].Int();
      // Latest can preserve contiguous older pages, but must not bridge a gap.
      if (mode == "latest" && t.loaded) {
        int64_t first = 0;
        for (const auto& m : page["messages"].Elements()) if (!first || m["id"].Int() < first) first = m["id"].Int();
        replace = t.hasNewer || t.messages.empty() || !first || first > t.messages.back().id;
      }
      if (!replace) for (const auto& m : t.messages) combined[m.id] = m;
      for (const auto& value : page["messages"].Elements()) {
        auto m = ParseChatMessage(value);
        if (m.id > 0 && m.channel == channel) combined[m.id] = std::move(m);
      }
      // A gateway edit received during GET wins over its older snapshot.
      for (const auto& [type, value] : buffered) {
        auto m = ParseChatMessage(value);
        const bool gap = mode == "around" ? page["has_newer"].Bool() : mode == "before" ? t.hasNewer : false;
        if (m.id > 0 && m.channel == channel && (combined.contains(m.id) || (type == "message" && !gap))) combined[m.id] = std::move(m);
        else if (type == "message" && gap) t.newestKnown = std::max(t.newestKnown, m.id);
      }
      std::deque<ChatMessage> next;
      for (auto& [id, m] : combined) next.push_back(std::move(m));
      const auto oldCount = t.messages.size();
      // Range notices are derived from ids, including overlaps and empty pages.
      size_t prefix = 0, suffix = 0;
      if (!replace && !t.messages.empty()) {
        while (prefix < next.size() && next[prefix].id < t.messages.front().id) ++prefix;
        while (suffix < next.size() && next[next.size()-suffix-1].id > t.messages.back().id) ++suffix;
      }
      bool contiguous = !replace && next.size() == oldCount + prefix + suffix;
      if (contiguous) {
        for (size_t i = 0; i < oldCount; ++i) if (next[prefix+i].id != t.messages[i].id) contiguous = false;
      }
      if (contiguous) {
        if (prefix) Notice(channel, ChatChangeKind::InsertStart, 0, prefix);
        for (size_t i = 0; i < oldCount; ++i) if (next[prefix+i] != t.messages[i])
          Notice(channel, ChatChangeKind::Update, prefix+i, 1, next[prefix+i].id);
        if (suffix) Notice(channel, ChatChangeKind::InsertEnd, prefix+oldCount, suffix);
      } else Notice(channel, ChatChangeKind::Reset, 0, next.size());
      const auto oldFirst = t.messages.empty() ? 0 : t.messages.front().id;
      t.messages = std::move(next); t.loaded = true;
      if (mode != "after" && !(mode == "latest" && !replace && oldFirst && oldFirst < pageFirst))
        t.hasMore = page["has_more"].Bool();
      if (mode == "around" || mode == "after" || mode == "latest") t.hasNewer = page["has_newer"].Bool();
      if (mode == "after" && !t.messages.empty()) t.hasNewer = t.hasNewer || t.newestKnown > t.messages.back().id;
      if (!t.hasNewer) t.newestKnown = 0;
      Bound(channel, mode == "before");
    }
    Notice(channel, ChatChangeKind::Metadata);
  }
  Announce();
}
void Chat::Send(const std::string& channel, std::string text, std::vector<std::string> attachments, int64_t replyTo) {
  if (!BeginSend(channel, text, replyTo)) return;
  SendQueued(channel, std::move(text), std::move(attachments), replyTo);
}
bool Chat::BeginSend(const std::string& channel, const std::string& text, int64_t replyTo) {
  {
    std::lock_guard lock(mu_); auto& t = state_.timelines[channel];
    if (signedOut_ || state_.owner.empty()) return false;
    if (t.sending) return false;
    if (t.draft.empty()) { t.draft = text; t.replyTo = replyTo; }
    t.sending = true; t.error.clear(); Notice(channel, ChatChangeKind::Metadata);
  }
  Announce(); return true;
}
void Chat::SendQueued(const std::string& channel, std::string text, std::vector<std::string> attachments, int64_t replyTo) {
  std::lock_guard operation(operations_);
  { std::lock_guard lock(mu_); auto at = state_.timelines.find(channel);
    if (signedOut_ || at == state_.timelines.end() || !at->second.sending) return; }
  const auto sentBody = text; text = ChatTrim(text);
  if (text.empty() && attachments.empty()) {
    { std::lock_guard lock(mu_); state_.timelines[channel].sending = false; Notice(channel, ChatChangeKind::Metadata); }
    Announce(); return;
  }
  auto body = json::Value::Object(); body["body"] = text; body["reply_to"] = replyTo;
  body["attachments"] = json::Value::Array(); for (const auto& id : attachments) body["attachments"].Push(id);
  auto r = transport_("POST", "/api/channels/" + ChatEscape(channel) + "/messages", &body);
  {
    std::lock_guard lock(mu_); auto& t = state_.timelines[channel]; t.sending = false;
    if (Good(r)) {
      // Only a row the gateway has not brought yet: the gateway's own copy
      // is at least as new (a link card may already be on it).
      auto m = ParseChatMessage(r.Json()); Upsert(m, false, true);
      if (t.draft == sentBody) t.draft.clear();
      t.replyTo = 0; // Composer's onSent closes the reply after success.
      if (auto* c = Conversation(channel)) c->lastMessage = std::max(c->lastMessage, m.id);
      t.error.clear();
    } else t.error = ChatError(r);
    Notice(channel, ChatChangeKind::Draft); Notice(channel, ChatChangeKind::Metadata);
  }
  Announce();
}
bool Chat::CanEdit(const ChatMessage& m) const {
  std::lock_guard lock(mu_); return !m.deleted && m.kind.empty() && m.author.username == state_.owner;
}
bool Chat::CanDelete(const ChatMessage& m) const {
  std::lock_guard lock(mu_); return !m.deleted && m.kind.empty() && (m.author.username == state_.owner || state_.Moderates(m.channel));
}
void Chat::Edit(const std::string& channel, int64_t id, std::string text) {
  std::lock_guard operation(operations_);
  if (ChatTrim(text).empty()) {
    { std::lock_guard lock(mu_); auto& t = state_.timelines[channel]; t.editText.clear(); t.error.clear(); Notice(channel, ChatChangeKind::Metadata); }
    Announce(); return; // Messages.commit closes an empty edit without PATCH.
  }
  bool allowed = false;
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel]; t.editText = text;
    for (const auto& m : t.messages) if (m.id == id) allowed = !m.deleted && m.kind.empty() && m.author.username == state_.owner; }
  auto body = json::Value::Object(); body["body"] = ChatTrim(text);
  auto r = allowed ? transport_("PATCH", "/api/messages/" + std::to_string(id), &body) : net::Response{404, R"({"error":"not_found"})"};
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel];
    if (Good(r)) { Upsert(ParseChatMessage(r.Json()), true); t.editText.clear(); t.error.clear(); }
    else t.error = ChatError(r); Notice(channel, ChatChangeKind::Metadata); }
  Announce();
}
void Chat::Remove(const std::string& channel, int64_t id) {
  std::lock_guard operation(operations_);
  bool allowed = false;
  { std::lock_guard lock(mu_); for (const auto& m : state_.timelines[channel].messages) if (m.id == id)
      allowed = !m.deleted && m.kind.empty() && (m.author.username == state_.owner || state_.Moderates(channel)); }
  auto r = allowed ? transport_("DELETE", "/api/messages/" + std::to_string(id), nullptr) : net::Response{404, R"({"error":"not_found"})"};
  { std::lock_guard lock(mu_); if (Good(r)) Upsert(ParseChatMessage(r.Json()), true);
    state_.timelines[channel].error = Good(r) ? "" : ChatError(r); Notice(channel, ChatChangeKind::Metadata); }
  Announce();
}
void Chat::React(const std::string& channel, int64_t id, std::string emoji, bool on) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["emoji"] = std::move(emoji); body["on"] = on;
  auto r = transport_("POST", "/api/messages/" + std::to_string(id) + "/reactions", &body);
  { std::lock_guard lock(mu_); if (Good(r)) Upsert(ParseChatMessage(r.Json()), true);
    state_.timelines[channel].error = Good(r) ? "" : ChatError(r); Notice(channel, ChatChangeKind::Metadata); }
  Announce();
}
bool Chat::CanPin(const ChatMessage& message) const {
  std::lock_guard lock(mu_);
  if (message.deleted || !message.kind.empty()) return false;
  return state_.Pins(message.channel);
}
std::string Chat::Pin(const std::string& channel, int64_t id, bool on) {
  std::lock_guard operation(operations_);
  auto r = transport_(on ? "POST" : "DELETE", "/api/messages/" + std::to_string(id) + "/pin", nullptr);
  auto error = Good(r) ? std::string{} : ChatError(r);
  { std::lock_guard lock(mu_);
    if (Good(r)) Upsert(ParseChatMessage(r.Json()), true);
    state_.timelines[channel].error = error; Notice(channel, ChatChangeKind::Metadata);
  }
  Announce(); return error;
}
std::vector<ChatMessage> Chat::Pins(const std::string& channel, std::string& error) {
  std::lock_guard operation(operations_);
  auto r = transport_("GET", "/api/channels/" + ChatEscape(channel) + "/pins", nullptr);
  error = Good(r) ? std::string{} : ChatError(r);
  std::vector<ChatMessage> messages;
  if (Good(r)) {
    auto data = r.Json();
    for (const auto& m : data["messages"].Elements()) messages.push_back(ParseChatMessage(m));
  }
  return messages;
}
std::string Chat::RenameChannel(const std::string& channel, std::string name) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["name"] = ChatTrim(std::move(name));
  auto r = transport_("PATCH", "/api/channels/" + ChatEscape(channel), &body);
  if (!Good(r)) return ChatError(r);
  Event("channel", r.Json()); return {};
}
std::string Chat::CreateChannel(std::string name, const std::string& kind, std::string* created) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["name"] = ChatTrim(std::move(name)); body["topic"] = ""; body["kind"] = kind;
  auto r = transport_("POST", "/api/channels", &body);
  if (!Good(r)) return ChatError(r);
  auto data = r.Json();
  if (created) *created = data["id"].Str();
  Event("channel", data); return {};
}
// The server keeps the messages (store.DeleteRoom); the channel leaves every
// column. The event does the same here when the socket is between lives.
std::string Chat::RemoveChannel(const std::string& channel) {
  std::lock_guard operation(operations_);
  auto r = transport_("DELETE", "/api/channels/" + ChatEscape(channel), nullptr);
  if (!Good(r)) return ChatError(r);
  auto gone = json::Value::Object(); gone["id"] = channel;
  Event("channel_removed", gone); return {};
}
std::string Chat::CreateCategory(std::string name) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["name"] = ChatTrim(std::move(name));
  auto r = transport_("POST", "/api/categories", &body);
  if (!Good(r)) return ChatError(r);
  Resync(); return {};
}
std::string Chat::RenameCategory(const std::string& id, std::string name) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["name"] = ChatTrim(std::move(name));
  auto r = transport_("PATCH", "/api/categories/" + ChatEscape(id), &body);
  if (!Good(r)) return ChatError(r);
  Resync(); return {};
}
std::string Chat::RemoveCategory(const std::string& id) {
  std::lock_guard operation(operations_);
  auto r = transport_("DELETE", "/api/categories/" + ChatEscape(id), nullptr);
  if (!Good(r)) return ChatError(r);
  Resync(); return {};
}
std::string Chat::SaveLayout(const ChatOrder& order) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(), layout = json::Value::Object();
  body["categories"] = json::Value::Array(); body["channels"] = json::Value::Array();
  layout["categories"] = json::Value::Array(); layout["channels"] = json::Value::Array();
  {
    std::lock_guard lock(mu_);
    for (size_t i = 0; i < order.categories.size(); ++i) {
      const auto& id = order.categories[i]; body["categories"].Push(id);
      for (const auto& f : state_.categories) if (f.id == id) {
        auto c = json::Value::Object(); c["id"] = id; c["name"] = f.name; c["position"] = int64_t(i);
        layout["categories"].Push(c); break;
      }
    }
    std::map<std::pair<std::string, std::string>, int64_t> next;
    for (const auto& [id, folder] : order.channels) {
      auto c = json::Value::Object(); c["id"] = id; c["category_id"] = folder; body["channels"].Push(c);
      std::string kind;
      for (const auto* list : {&state_.conversations, &state_.rooms}) for (const auto& room : *list)
        if (room.id == id) kind = room.kind;
      c["position"] = next[{folder, kind}]++; layout["channels"].Push(c);
    }
    ApplyLayout(state_, layout); Notice({}, ChatChangeKind::Metadata);
  }
  Announce();
  auto r = transport_("PUT", "/api/layout", &body);
  if (!Good(r)) { auto error = ChatError(r); Resync(); return error; }
  Event("layout", r.Json()); return {};
}
void Chat::OpenDM(std::string username) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["username"] = std::move(username);
  auto r = transport_("POST", "/api/dms", &body);
  if (Good(r)) { Event("channel", r.Json()); Open(r.Json()["id"].Str()); }
  else { { std::lock_guard lock(mu_); state_.error = ChatError(r); Notice({}, ChatChangeKind::Metadata); } Announce(); }
}
net::Response Chat::Nickname(const std::string& username, std::string name) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["name"] = ChatTrim(std::move(name));
  auto r = transport_("PUT", "/api/people/" + ChatEscape(username) + "/nickname", &body);
  if (Good(r)) {
    // Refresh the canonical list like the web: a newly joined person may not
    // have been present in the last member snapshot on this device.
    auto members = transport_("GET", "/api/members", nullptr);
    if (!Good(members)) return members;
    const auto people = members.Json();
    const auto saved = r.Json()["name"].Str();
    { std::lock_guard lock(mu_);
      if (people["members"].IsArray()) {
        state_.members.clear();
        for (const auto& person : people["members"].Elements()) state_.members.push_back(Person(person));
      }
      auto apply = [&](ChatPerson& p) { if (p.username == username) p.nickname = saved; };
      for (auto& p : state_.members) apply(p);
      for (auto& c : state_.conversations) apply(c.peer);
      for (auto& [id, t] : state_.timelines) {
        for (auto& m : t.messages) { apply(m.author); if (m.reply) apply(m.reply->author); }
        Notice(id, ChatChangeKind::Reset);
      }
      Notice({}, ChatChangeKind::Metadata);
    }
    Announce();
  }
  return r;
}
net::Response Chat::Diagnostic(std::string report) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["report"] = std::move(report);
  return transport_("POST", "/api/diagnostics", &body);
}
net::Response Chat::RingRoom(const std::string& room, const std::string& username) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["username"] = username;
  return transport_("POST", "/api/voice/" + ChatEscape(room) + "/ring", &body);
}
void Chat::Mute(const std::string& channel, bool muted) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object(); body["muted"] = muted;
  auto r = transport_("POST", "/api/channels/" + ChatEscape(channel) + "/mute", &body);
  { std::lock_guard lock(mu_); if (Good(r)) { if (auto* c = Conversation(channel)) c->muted = r.Json()["muted"].Bool(); }
    state_.error = Good(r) ? "" : ChatError(r); Notice(channel, ChatChangeKind::Metadata); }
  if (Good(r)) Resync(); else Announce();
}
void Chat::Hide(const std::string& channel) {
  std::lock_guard operation(operations_);
  auto body = json::Value::Object();
  auto r = transport_("POST", "/api/channels/" + ChatEscape(channel) + "/hide", &body);
  { std::lock_guard lock(mu_); if (Good(r)) {
      if (auto* c = Conversation(channel)) c->hidden = true;
      auto& t = state_.timelines[channel]; t.messages.clear(); t.loaded = false; t.replyTo = 0;
      Notice(channel, ChatChangeKind::Reset);
      if (state_.active == channel) state_.active.clear();
    }
    state_.error = Good(r) ? "" : ChatError(r); Notice(channel, ChatChangeKind::Metadata); }
  Announce();
}
void Chat::BottomVisible(const std::string& channel, bool visible) {
  std::lock_guard operation(operations_);
  int64_t newest;
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel]; auto* c = Conversation(channel);
    if (!visible || channel != state_.active || !c || t.hasNewer || t.messages.empty()) return;
    newest = t.messages.back().id; if (newest <= c->lastRead) return; }
  auto body = json::Value::Object(); body["last_read_id"] = newest;
  auto r = transport_("POST", "/api/channels/" + ChatEscape(channel) + "/read", &body);
  if (Good(r)) { auto ev = json::Value::Object(); ev["channel_id"] = channel; ev["last_read_id"] = newest; Event("read", ev); }
  else { { std::lock_guard lock(mu_); state_.timelines[channel].error = ChatError(r); Notice(channel, ChatChangeKind::Metadata); } Announce(); }
}
void Chat::Typing(const std::string& channel, const std::string& text) {
  std::lock_guard operation(operations_);
  if (ChatTrim(text).empty()) return;
  auto now = clock_();
  { std::lock_guard lock(mu_); auto previous = lastTyping_.find(channel);
    if (previous != lastTyping_.end() && now - previous->second <= 3000) return;
    lastTyping_[channel] = now; }
  auto body = json::Value::Object(); transport_("POST", "/api/channels/" + ChatEscape(channel) + "/typing", &body);
}
std::optional<int64_t> Chat::NextTickDelay() const {
  std::lock_guard lock(mu_);
  std::optional<int64_t> delay;
  if(refreshMembers_ || refreshChannels_) delay=500; // failed refresh retry
  auto now=clock_();
  for(auto const& [key,typing]:state_.typing) {
    auto remaining=std::max<int64_t>(0,typing.until-now);
    if(!delay || remaining<*delay) delay=remaining;
  }
  return delay;
}
void Chat::Tick() {
  std::lock_guard operation(operations_);
  bool changed = false;
  bool members = false, channels = false;
  { std::lock_guard lock(mu_); const auto now = clock_();
    members = std::exchange(refreshMembers_, false); channels = std::exchange(refreshChannels_, false);
    for (auto at = state_.typing.begin(); at != state_.typing.end();) {
      if (at->second.until <= now) { Notice(at->first.first, ChatChangeKind::Typing); at = state_.typing.erase(at); changed = true; }
      else ++at;
    }
  }
  if (channels) { Resync(); return; }
  if (members) {
    auto r = transport_("GET", "/api/members", nullptr); auto data = r.Json();
    std::lock_guard lock(mu_);
    if (Good(r) && data["members"].IsArray()) {
      state_.members.clear(); for (const auto& p : data["members"].Elements()) state_.members.push_back(Person(p));
      Notice({}, ChatChangeKind::Metadata); changed = true;
    } else refreshMembers_ = true;
  }
  if (changed) Announce();
}
void Chat::Draft(const std::string& channel, std::string text, int64_t replyTo) {
  { std::lock_guard lock(mu_); auto& t = state_.timelines[channel]; t.draft = std::move(text); t.replyTo = replyTo;
    if (std::find(draftOrder_.begin(), draftOrder_.end(), channel) == draftOrder_.end()) draftOrder_.push_back(channel);
    Notice(channel, ChatChangeKind::Draft); }
  Announce();
}
void Chat::LoadDrafts() {
  if (draftFile_.empty()) return;
  std::error_code ec;
  auto size = std::filesystem::file_size(draftFile_, ec);
  if (!ec && size <= (512u << 10)) {
    std::ifstream input(draftFile_, std::ios::binary);
    auto value = json::Parse(std::string(std::istreambuf_iterator<char>(input), {}));
    if (value && (*value)["owner"].Str() == state_.owner && clock_() - (*value)["at"].Int() <= 7ll*24*60*60*1000) {
      const auto& order = (*value)["order"];
      for (const auto& key : order.Elements()) {
        if (draftOrder_.size() == 20) break;
        auto id = key.Str(); auto limited = LimitRunes((*value)["items"][id].Str(), 4000);
        draftOrder_.push_back(id);
        if (!ChatTrim(limited).empty()) state_.timelines[id].draft = std::move(limited);
      }
    }
  }
  std::filesystem::remove(draftFile_, ec);
}
bool Chat::SaveDrafts() {
  if (draftFile_.empty() || signedOut_ || state_.owner.empty()) return true;
  auto data = json::Value::Object(), items = json::Value::Object(), order = json::Value::Array();
  // Stable bounded iteration. All conversations' text, never attachments/reply.
  size_t count = 0;
  std::vector<std::string> selected;
  for (auto at = draftOrder_.rbegin(); at != draftOrder_.rend() && count < 20; ++at) {
    auto found = state_.timelines.find(*at); if (found == state_.timelines.end()) continue;
    auto text = LimitRunes(found->second.draft, 4000);
    if (!ChatTrim(text).empty()) { items[*at] = text; selected.push_back(*at); ++count; }
  }
  for (auto at = selected.rbegin(); at != selected.rend(); ++at) order.Push(*at);
  std::error_code ec;
  if (!count) { std::filesystem::remove(draftFile_, ec); return !ec; }
  data["at"] = clock_(); data["owner"] = state_.owner; data["items"] = std::move(items); data["order"] = std::move(order);
  std::filesystem::create_directories(draftFile_.parent_path(), ec);
  if (ec) return false;
  auto temp = draftFile_; temp += L".tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << data.Dump(); out.close();
  if (!out || !MoveFileExW(temp.c_str(), draftFile_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temp, ec); return false;
  }
  return true;
}
void Chat::Foreground(bool visible) {
  std::lock_guard operation(operations_);
  { std::lock_guard lock(mu_); visible_ = visible;
    if (visible && !draftFile_.empty()) { std::error_code ec; std::filesystem::remove(draftFile_, ec); }
    else if (!SaveDrafts()) state_.error = "Não foi possível guardar o rascunho neste Windows.";
    Notice({}, ChatChangeKind::Metadata); }
  Announce();
}
void Chat::Forget() {
  std::lock_guard operation(operations_);
  { std::lock_guard lock(mu_); signedOut_ = true; state_ = {}; ++resourceVersion_; lastTyping_.clear(); inFlight_.clear();
    ForgetImages(); draftOrder_.clear(); refreshMembers_ = refreshChannels_ = false;
    if (!draftFile_.empty()) {
      std::error_code ec; std::filesystem::remove(draftFile_, ec); auto temp = draftFile_; temp += L".tmp"; std::filesystem::remove(temp, ec);
    } }
  Announce();
}
net::Response Chat::Upload(net::Client& client, const std::string& channel, const std::filesystem::path& file,
    const UploadProgress& progress, const std::atomic<bool>& cancel) {
  // No operations_ lock: a 32 MB file going up for minutes must not hold up
  // the chat, and the upload touches no chat state. The screen says what
  // went wrong next to the file's own name (Composer.tsx put), so the
  // conversation's error line is not written here.
  return UploadFile(client, channel, file, progress, cancel);
}
void Chat::PublishResources() {
  std::vector<std::pair<size_t,uint64_t>> channels;
  std::vector<std::string> channelNames;
  {std::lock_guard lock(mu_);if(publishedResourceVersion_!=resourceVersion_) {for(auto& [id,timeline]:state_.timelines){
    uint64_t bytes=0;
    for(auto& m:timeline.messages){
      bytes+=sizeof m+m.body.capacity()+m.channel.capacity()+m.createdAt.capacity()+m.editedAt.capacity()+m.deletedAt.capacity()+m.deletedBy.capacity()+m.kind.capacity();
      bytes+=m.author.username.capacity()+m.author.displayName.capacity()+m.author.nickname.capacity()+m.author.avatar.capacity();
      bytes+=m.attachments.capacity()*sizeof(ChatAttachment)+m.mentions.capacity()*sizeof(std::string)+m.reactions.capacity()*sizeof(ChatReaction);
      for(auto& a:m.attachments)bytes+=a.id.capacity()+a.name.capacity()+a.mime.capacity()+a.url.capacity();
      for(auto& mention:m.mentions)bytes+=mention.capacity();
      for(auto& reaction:m.reactions){bytes+=reaction.emoji.capacity()+reaction.users.capacity()*sizeof(std::string);for(auto& user:reaction.users)bytes+=user.capacity();}
      if(m.reply)bytes+=sizeof(ChatReply)+m.reply->body.capacity()+m.reply->author.username.capacity()+m.reply->author.displayName.capacity()+m.reply->author.nickname.capacity()+m.reply->author.avatar.capacity();
    }
    channels.push_back({timeline.messages.size(),bytes});
    auto conversation=std::find_if(state_.conversations.begin(),state_.conversations.end(),[&](auto& c){return c.id==id;});
    channelNames.push_back(conversation==state_.conversations.end()?"—":conversation->name);
  }
  resourceChannels_=std::move(channels);resourceNames_=std::move(channelNames);publishedResourceVersion_=resourceVersion_;
  }
  channels=resourceChannels_;channelNames=resourceNames_;
  }
  uint64_t count=0,bytes=0,emojiCount=0,emojiBytes=0,avatarCount=0,avatarBytes=0;
  {std::lock_guard lock(imagesMu_);count=images_.size();bytes=imageBytes_;for(auto& [url,image]:images_){
    if(url.find("/twemoji-")!=std::string::npos){++emojiCount;emojiBytes+=image.bytes->size();}
    else if(url.find("avatar")!=std::string::npos){++avatarCount;avatarBytes+=image.bytes->size();}
  }}
  auto& s=resources::State();std::lock_guard lock(s.mutex);s.channels=std::move(channels);s.channelNames=std::move(channelNames);s.encodedCount=count;s.encodedBytes=bytes;s.emojiCount=emojiCount;s.emojiBytes=emojiBytes;s.avatarCount=avatarCount;s.avatarBytes=avatarBytes;
}
void Chat::ForgetImages() {
  std::lock_guard lock(imagesMu_); images_.clear(); imageBytes_ = 0; ++imageEpoch_;
}
std::shared_ptr<const std::string> Chat::Image(net::Client& client, const std::string& url) {
  uint64_t epoch;
  { std::lock_guard lock(imagesMu_);
    auto found = images_.find(url);
    if (found != images_.end()) { found->second.used = ++imageSequence_; return found->second.bytes; }
    epoch = imageEpoch_; }
  auto r = DownloadBytes(client, url);
  if (!Good(r)) return {};
  auto bytes = std::make_shared<const std::string>(std::move(r.body));
  std::lock_guard lock(imagesMu_);
  if (epoch != imageEpoch_) return {};
  if (auto found = images_.find(url); found != images_.end()) { imageBytes_ -= found->second.bytes->size(); images_.erase(found); }
  // The emoji panel is ~1900 small pictures; the byte limit is what binds.
  constexpr size_t limit = 64 << 20;
  while (!images_.empty() && (imageBytes_ + bytes->size() > limit || images_.size() >= 4096)) {
    auto oldest = std::min_element(images_.begin(), images_.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    imageBytes_ -= oldest->second.bytes->size(); images_.erase(oldest);
  }
  imageBytes_ += bytes->size(); images_[url] = {bytes, ++imageSequence_}; return bytes;
}
}
