// Silent deterministic tests and an opt-in, isolated loopback integration test.
// No voice engine, audio device, production account or persisted session is used.
#include "../src/chat.h"
#include "../../native/src/resource-usage.h"
#include "../src/chat-format.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
int checks = 0;
void Check(bool condition, const char* name) {
  ++checks; if (!condition) throw std::runtime_error(name);
}
json::Value Message(int64_t id, std::string channel = "t", std::string author = "alice") {
  auto v = json::Value::Object(); v["id"] = id; v["channel_id"] = channel; v["body"] = "synthetic";
  v["author"]["username"] = author; v["author"]["display_name"] = author;
  v["attachments"] = json::Value::Array(); v["mentions"] = json::Value::Array(); v["reactions"] = json::Value::Array();
  return v;
}
json::Value Page(std::initializer_list<int64_t> ids, bool more = false, bool newer = false) {
  auto v = json::Value::Object(); v["messages"] = json::Value::Array(); v["has_more"] = more; v["has_newer"] = newer;
  for (auto id : ids) v["messages"].Push(Message(id)); return v;
}
struct Fake {
  int announcements = 0;
  int64_t now = 10000;
  std::string owner = "alice";
  json::Value page = Page({3, 4}, true);
  std::vector<std::string> paths;
  std::function<std::optional<net::Response>(const std::string&, const std::string&, const json::Value*)> intercept;
  std::unique_ptr<app::Chat> chat;
  explicit Fake(std::filesystem::path file) {
    chat = std::make_unique<app::Chat>([this](const auto& method, const auto& path, const auto* body) {
      paths.push_back(path);
      if (intercept) { auto r = intercept(method, path, body); if (r) return *r; }
      if (path == "/api/me") { auto v = json::Value::Object(); v["user"]["username"] = owner; return net::Response{200, v.Dump()}; }
      if (path == "/api/me/blocks") return net::Response{200, R"({"items":[]})"};
      if (path == "/api/channels") return net::Response{200, R"({"can_manage":false,"channels":[{"id":"t","kind":"text","last_message_id":4,"last_read_id":0},{"id":"d","kind":"dm"}]})"};
      if (path == "/api/members") return net::Response{200, R"({"members":[{"username":"alice","display_name":"Alice"},{"username":"bob","display_name":"Bob"}]})"};
      if (method == "GET") return net::Response{200, page.Dump()};
      return net::Response{204, ""};
    }, [this] { ++announcements; }, std::move(file), [this] { return now; });
    chat->Resync();
  }
  app::ChatTimeline Timeline(std::string channel = "t") {
    app::ChatTimeline result; chat->Inspect([&](const auto& s) { if (s.timelines.contains(channel)) result = s.timelines.at(channel); }); return result;
  }
};
void Paging(const std::filesystem::path& root) {
  Fake f(root / "paging.json"); f.chat->Open("t");
  f.chat->Search("  a&b?  ");
  Check(f.paths.back() == "/api/search?q=a%26b%3F", "search escapes query and uses the authenticated API without an owner parameter");
  f.page = Page({2, 1, 3}, false);
  f.intercept = [&](const auto& method, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path.find("before=") != std::string::npos) f.chat->Event("message", Message(5));
    return {};
  };
  f.chat->Older("t"); auto t = f.Timeline();
  Check(t.messages.size() == 5 && t.messages.front().id == 1 && t.messages.back().id == 5, "paging sorts and merges overlapping gateway/page ids");
  Check(!t.hasMore && !t.loading, "paging applies oldest boundary");
  bool insertStart = false;
  f.chat->Inspect([&](const auto& s) { for (const auto& n : s.changes)
    if (n.channel == "t" && n.kind == app::ChatChangeKind::InsertStart && n.index == 0 && n.count == 2) insertStart = true; });
  Check(insertStart, "older page reports exact insert-at-start range");
  Check(f.paths.back() == "/api/channels/t/messages?before=3", "before cursor is oldest retained id");
  auto edited = Message(3); edited["body"] = "newer fixture";
  f.intercept = [&](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path.find("messages") != std::string::npos) f.chat->Event("message_changed", edited); return {};
  };
  f.page = Page({3, 4, 5}); f.chat->Open("t");
  Check(f.Timeline().messages[2].body == "newer fixture", "gateway edit during GET beats stale page");
}
void Sending(const std::filesystem::path& root) {
  Fake f(root / "sending.json"); f.chat->Open("t"); f.chat->Draft("t", "first", 3);
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (method != "POST" || path != "/api/channels/t/messages") return {};
    Check(f.Timeline().sending, "send exposes busy state before network completes");
    f.chat->Send("t", "duplicate Enter");
    Check((*body)["reply_to"].Int() == 3 && !body->Has("nonce"), "send uses web reply and no invented nonce");
    auto m = Message(5); auto reply = Message(3); reply["channel_id"] = nullptr; m["reply_to"] = reply;
    f.chat->Event("message", m);
    f.chat->Draft("t", "typed during send", 4);
    return net::Response{201, m.Dump()};
  };
  f.chat->Send("t", "first", {}, 3);
  auto t = f.Timeline();
  Check(!t.sending && t.messages.size() == 3 && t.messages.back().id == 5, "HTTP acknowledgement and earlier gateway echo produce one row");
  Check(t.messages.back().reply && t.messages.back().reply->id == 3, "incoming reply reference preserved");
  Check(t.draft == "typed during send" && t.replyTo == 0, "send preserves new text and closes reply after success like Composer");
  f.chat->Draft("t", "typed during send", 4);
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{429, R"({"error":"message_limit"})"}; };
  f.chat->Send("t", "typed during send", {}, 4); t = f.Timeline();
  Check(t.draft == "typed during send" && t.replyTo == 4 && !t.sending && t.error == "Você mandou mensagens rápido demais. Espere um minuto.", "failed send retains composer and web error");
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{0, ""}; };
  auto messagesBeforeFailure = t.messages.size();
  f.chat->Send("t", "typed during send", {}, 4); t = f.Timeline();
  Check(t.draft == "typed during send" && t.replyTo == 4 && !t.sending && !t.error.empty(), "invalid HTTP response retains draft, reply and retry state");
  Check(t.messages.size() == messagesBeforeFailure, "invalid HTTP response never inserts a partial confirmation");
  // The link card lands while the send's answer is still on its way (the
  // owner, 30/10/2026: sent at once, no card; waited, card): the answer,
  // written before the card existed, must not take it away.
  f.intercept = [&](const auto& method, const auto& path, const auto*) -> std::optional<net::Response> {
    if (method != "POST" || path != "/api/channels/t/messages") return {};
    auto plain = Message(6); plain["body"] = "https://vimeo.com/1";
    auto carded = plain; carded["embeds"] = json::Value::Array();
    auto e = json::Value::Object(); e["url"] = "https://vimeo.com/1"; e["kind"] = "video"; e["title"] = "A video"; carded["embeds"].Push(e);
    f.chat->Event("message", plain); f.chat->Event("message_changed", carded);
    return net::Response{201, plain.Dump()};
  };
  f.chat->Send("t", "https://vimeo.com/1"); t = f.Timeline();
  Check(t.messages.back().id == 6 && t.messages.back().embeds.size() == 1, "the send's answer never erases a card that arrived before it");
}
void Changes(const std::filesystem::path& root) {
  Fake f(root / "changes.json"); f.chat->Open("t");
  auto m = Message(3); m["body"] = "edited fixture";
  f.chat->Event("message_changed", m);
  Check(f.Timeline().messages[0].body == "edited fixture" && f.Timeline().messages[1].id == 4, "edit updates only addressed row");
  m["deleted"] = true; m["deleted_by"] = "alice"; m["body"] = "";
  f.chat->Event("message_changed", m);
  auto t = f.Timeline();
  Check(t.messages.size() == 2 && t.messages[0].deleted && t.messages[0].deletedBy == "alice", "delete retains tombstone in correct row");
  bool update = false;
  f.chat->Inspect([&](const auto& s) { auto n = s.changes.back(); update = n.channel == "t" && n.kind == app::ChatChangeKind::Update && n.id == 3 && n.index == 0 && n.count == 1; });
  Check(update, "changed message notice identifies row and id");
  f.chat->Event("message_changed", Message(99));
  Check(f.Timeline().messages.size() == 2, "edit outside retained page never inserts a row");
  m = Message(4); auto reaction = json::Value::Object(); reaction["emoji"] = "👍"; reaction["users"] = json::Value::Array();
  reaction["users"].Push("alice"); reaction["users"].Push("bob"); m["reactions"].Push(reaction);
  f.chat->Event("message_changed", m); t = f.Timeline();
  Check(t.messages[1].reactions.size() == 1 && t.messages[1].reactions[0].users.size() == 2 && t.messages[1].reactions[0].Includes("alice"), "reaction aggregation includes self");
  m["reactions"] = json::Value::Array(); f.chat->Event("message_changed", m);
  Check(f.Timeline().messages[1].reactions.empty(), "reaction removal replaces aggregate");
  Check(!f.chat->CanEdit(t.messages[0]) && !f.chat->CanDelete(t.messages[0]), "deleted rows have no edit/delete actions");
  auto other = app::ParseChatMessage(Message(8, "t", "bob"));
  Check(!f.chat->CanEdit(other) && !f.chat->CanDelete(other), "regular user cannot edit or delete another author");
  f.chat->Edit("t", 4, "failed edit");
  Check(f.Timeline().editText.empty(), "successful edit closes edit draft");
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{0, ""}; };
  f.chat->Edit("t", 4, "kept edit");
  Check(f.Timeline().editText == "kept edit" && !f.Timeline().error.empty(), "failed edit keeps edit text");
}
void TypingAndRead(const std::filesystem::path& root) {
  Fake f(root / "typing.json"); f.chat->Open("t");
  auto ev = json::Value::Object(); ev["channel_id"] = "t"; ev["username"] = "bob"; ev["display_name"] = "Bob";
  Check(!f.chat->NextTickDelay(),"idle chat has no periodic deadline");
  f.chat->Event("typing", ev);
  Check(f.chat->NextTickDelay()==5000,"typing schedules its actual expiration");
  f.now += 4999; f.chat->Tick();
  f.chat->Inspect([&](const auto& s) { Check(s.typing.size() == 1, "typing remains before five seconds"); });
  Check(f.chat->NextTickDelay()==1,"typing deadline follows the clock");
  ++f.now; f.chat->Tick();
  f.chat->Inspect([&](const auto& s) { Check(s.typing.empty(), "typing expires at exactly five seconds"); });
  Check(!f.chat->NextTickDelay(),"expired typing leaves no idle wakeup");
  auto start = f.paths.size(); f.chat->Typing("t", " "); f.chat->Typing("t", "a");
  f.now += 3000; f.chat->Typing("t", "ab"); ++f.now; f.chat->Typing("t", "abc");
  Check(f.paths.size() == start + 2, "typing sender observes web's strict greater-than 3000ms rule");
  f.intercept = [](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path.ends_with("/read")) return net::Response{503, ""}; return {};
  };
  f.chat->BottomVisible("t", false);
  f.chat->BottomVisible("t", true);
  f.chat->Inspect([&](const auto& s) { Check(s.conversations[0].lastRead == 0, "read failure does not advance marker"); });
  f.intercept = {}; f.chat->BottomVisible("t", true);
  f.chat->Inspect([&](const auto& s) { Check(s.conversations[0].lastRead == 4 && s.conversations[0].unread == 0, "read retries and advances only after success"); });
  f.page = Page({1, 2}, true, true); f.chat->Open("t", 2);
  start = f.paths.size(); f.chat->BottomVisible("t", true);
  Check(f.paths.size() == start, "jump window with newer pages never marks unseen end read");
  f.chat->Event("message", Message(9));
  Check(f.Timeline().messages.back().id == 2 && f.Timeline().newestKnown == 9, "live message never bridges jump-window gap");
  f.page = Page({3, 4}, false, true); f.chat->Newer("t");
  Check(f.paths.back().ends_with("?after=2") && f.Timeline().messages.back().id == 4 && f.Timeline().hasNewer, "forward page keeps gap until latest known message");
}
void Drafts(const std::filesystem::path& root) {
  auto file = root / "drafts.json";
  { Fake f(file); f.chat->Draft("t", "unsent fixture", 4);
    Check(!std::filesystem::exists(file), "foreground draft never writes disk");
    f.chat->Foreground(false); Check(std::filesystem::exists(file) && !std::filesystem::exists(file.wstring()+L".tmp"), "background draft atomically replaces one file"); }
  { Fake f(file); auto t = f.Timeline();
    Check(t.draft == "unsent fixture" && t.replyTo == 0, "restart restores only own text without reply");
    Check(!std::filesystem::exists(file), "taking restored draft removes disk copy");
    f.chat->Draft("t", " "); f.chat->Foreground(false);
    Check(!std::filesystem::exists(file), "empty draft is dropped");
    f.chat->Draft("t", "fixture"); f.chat->Foreground(false); f.chat->Foreground(true);
    Check(!std::filesystem::exists(file), "foreground drops persisted drafts");
    f.chat->Foreground(false); f.chat->Forget(); f.chat->Foreground(false);
    Check(!std::filesystem::exists(file), "logout latch prevents departure from recreating drafts"); }
  { Fake f(file); f.chat->Draft("t", std::string(4100, 'x')); f.chat->Foreground(false); }
  { Fake f(file); Check(f.Timeline().draft.size() == 4000, "disk draft limits text to server's 4000 runes"); }
  { Fake f(file); f.chat->Draft("t", "expired fixture"); f.chat->Foreground(false); }
  { Fake f(root / "other.json"); f.owner = "bob";
    auto disk = json::Value::Object(); disk["owner"] = "alice"; disk["at"] = f.now; disk["items"]["t"] = "other owner";
    std::ofstream(file) << disk.Dump();
    // Switch file via a second real Chat with Bob's fake identity.
    app::Chat bob([&](const auto&, const auto& p, const auto*) {
      return p == "/api/me" ? net::Response{200, R"({"user":{"username":"bob"}})"} :
       p == "/api/channels" ? net::Response{200, R"({"channels":[{"id":"t","kind":"text"}]})"} : net::Response{200,R"({"members":[]})"}; }, {}, file, [&] { return f.now; });
    bob.Resync(); bob.Inspect([&](const auto& s) { Check(!s.timelines.contains("t"), "another account never receives persisted text"); });
    Check(!std::filesystem::exists(file), "another owner's file is discarded"); }
  { auto disk = json::Value::Object(); disk["owner"] = "alice"; disk["at"] = int64_t(-8ll*24*60*60*1000);
    disk["items"]["t"] = "old"; disk["order"] = json::Value::Array(); disk["order"].Push("t"); std::ofstream(file) << disk.Dump();
    Fake f(file); Check(f.Timeline().draft.empty(), "draft older than seven days expires"); }
  { Fake f(file);
    for (int i = 0; i < 21; ++i) f.chat->Draft("c" + std::to_string(i), "own text fixture");
    f.chat->Foreground(false); }
  { Fake f(file); f.chat->Inspect([&](const auto& s) {
      Check(!s.timelines.contains("c0") && s.timelines.at("c20").draft == "own text fixture" && s.timelines.size() == 20,
          "disk draft drops oldest conversation beyond twenty"); }); }
  { Fake f(file); std::string text;
    for (int i = 0; i < 4100; ++i) text += "🙂";
    f.chat->Draft("t", text); f.chat->Foreground(false); }
  { Fake f(file); Check(f.Timeline().draft.size() == 16000, "draft rune limit preserves complete UTF-8 emoji"); }
}
void PureFunctions() {
  std::vector<app::ChatPerson> people{{"alpha", "Pessoa", "", "", false}, {"beta", "ALPHA display", "", "", false}};
  auto matches = app::ChatAutocomplete(people, "ALPHA");
  Check(matches.size() == 2 && matches[0].username == "alpha", "autocomplete matches username and display substrings case-insensitively");
  Check(app::ChatAutocomplete(people, "tod")[0].username == "todos", "autocomplete includes @todos");
  auto at = app::ChatMentionAt("prefix @al", 10);
  Check(at && at->at == 7 && at->username == "al", "caret mention completion mirrors Composer regex");
  Check(!app::ChatMentionAt("@al space", 9), "completion stops at whitespace");
  {
    // Links: the sentence keeps its punctuation, a link keeps its own
    // parenthesis, and an "@" inside an address is the address's.
    std::string text = "veja https://a.com/x. e (https://w.org/A_(b)) <https://c.com> HTTP://d.com/@bob, https:// fim";
    auto links = app::ChatLinks(text);
    std::vector<std::string> got; for (auto l : links) got.push_back(text.substr(l.at, l.length));
    Check(got == std::vector<std::string>{"https://a.com/x", "https://w.org/A_(b)", "https://c.com", "HTTP://d.com/@bob"}, "links end where chat programs end them");
  }
  auto mentions = app::ChatMentions("@ab @alice @bob. @_bad @todos");
  Check(mentions.size() == 3 && mentions[1].username == "bob", "render mention tokens use 3-32 bytes and alphanumeric ends");
  Check(app::ChatEscape("a/b ?") == "a%2Fb%20%3F", "channel ids are escaped as URL segments");
}
void Memory(const std::filesystem::path& root) {
  Fake f(root / "memory.json"); f.chat->Open("t");
  for (int id = 5; id <= 150; ++id) f.chat->Event("message", Message(id));
  f.chat->Open("d");
  Check(f.Timeline().messages.size() == 50 && f.Timeline().messages.back().id == 150 && f.Timeline().hasMore, "leaving conversation retains newest fifty rows");
  auto m = Message(151, "t", "bob"); m["mentions"].Push("alice");
  f.chat->Event("message", m); f.chat->Event("message", m);
  f.chat->Inspect([&](const auto& s) { Check(s.conversations[0].unread == 1 && s.conversations[0].mentions == 1, "duplicate gateway event does not duplicate unread or mention count"); });
  f.chat->Event("channel_removed", [] { auto v = json::Value::Object(); v["id"] = "t"; return v; }());
  f.chat->Inspect([&](const auto& s) { Check(!s.timelines.contains("t"), "removed channel discards its retained content"); });
}
void Conversations(const std::filesystem::path& root) {
  Fake f(root / "conversations.json");
  bool muted = false, admin = false;
  f.intercept = [&](const auto&, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (path.ends_with("/mute")) {
      muted = (*body)["muted"].Bool(); auto r = json::Value::Object(); r["muted"] = muted; return net::Response{200, r.Dump()};
    }
    if (path == "/api/me" && admin) return net::Response{200, R"({"user":{"username":"alice","is_admin":true}})"};
    if (path == "/api/channels") {
      auto r = json::Value::Object(); r["can_manage"] = admin; r["channels"] = json::Value::Array();
      auto c = json::Value::Object(); c["id"] = "t"; c["kind"] = "text"; c["muted"] = muted; r["channels"].Push(c);
      c["id"] = "d"; c["kind"] = "dm"; r["channels"].Push(c); return net::Response{200, r.Dump()};
    }
    return {};
  };
  f.chat->Mute("t", true); auto m = Message(10, "t", "bob"); m["mentions"].Push("alice"); f.chat->Event("message", m);
  f.chat->Inspect([&](const auto& s) { Check(s.conversations[0].muted && s.conversations[0].unread == 0 && s.conversations[0].mentions == 0,
      "muted conversation suppresses unread and mention counters"); });
  f.chat->Open("d"); f.chat->Draft("d", "own DM draft", 4); f.chat->Hide("d");
  f.chat->Inspect([&](const auto& s) { Check(s.active.empty() && s.conversations[1].hidden && s.timelines.at("d").draft == "own DM draft",
      "hiding DM closes it and preserves only own draft"); });
  auto channel = json::Value::Object(); channel["id"] = "d"; channel["kind"] = "dm";
  f.chat->Event("channel", channel);
  f.chat->Inspect([&](const auto& s) { Check(!s.conversations[1].hidden, "channel event reopens hidden DM without duplicating it"); });
  admin = true; f.chat->Resync(); auto other = app::ParseChatMessage(m);
  Check(f.chat->CanDelete(other) && !f.chat->CanEdit(other), "admin can delete another author's message but cannot edit it");
}
void Reorder() {
  using D = app::ChatDrop;
  auto room = [](std::string id, std::string kind, std::string folder, int64_t pos, std::string name = "") {
    app::ChatConversation c; c.id = id; c.kind = kind; c.category = folder; c.position = pos; c.name = name; return c;
  };
  auto a = room("a", "text", "", 2), b = room("b", "text", "missing", 1);
  b.hidden = true;
  auto p = app::ChatColumn({a, b, room("d", "dm", "", 0), room("c", "text", "f", 0)},
      {room("v", "voice", "f", 1), room("w", "voice", "f", 0)}, {{"g", "Second", 2}, {"f", "First", 1}});
  Check(p.size() == 3 && !p[0].folder && p[0].texts[0].id == "b" && p[0].texts[1].id == "a", "column keeps hidden rooms, ignores DMs, sorts unknown folder at top");
  Check(p[1].folder->id == "f" && p[1].voices[0].id == "w", "column sorts folders and each kind independently");
  auto ties = app::ChatColumn({room("z", "text", "", 0, "b"), room("b", "text", "", 0, "a"), room("a", "text", "", 0, "a")}, {}, {});
  Check(ties[0].texts[0].id == "a" && ties[0].texts[1].id == "b", "column breaks position ties by name then id");
  auto names = app::ChatColumn({room("z", "text", "", 0, "zebra"), room("a", "text", "", 0, "álbum"),
      room("plain", "text", "", 0, "ab"), room("dash", "text", "", 0, "a-b")}, {},
      {{"z", "Zebra", 0}, {"a", "Álbum", 0}});
  Check(names[0].texts[0].id == "dash" && names[0].texts[1].id == "plain" && names[0].texts[2].id == "a" &&
      names[1].folder->id == "a", "linguistic sorting keeps accents near base letters and hyphens significant");
  auto check = [&](const std::optional<app::ChatOrder>& o, std::vector<std::string> cats,
      std::vector<std::pair<std::string, std::string>> channels, const char* label) {
    Check(o && o->categories == cats && o->channels == channels, label);
  };
  // Expected complete orders follow reorder.ts flatten: top, folders, texts then voices.
  check(app::ChatMoveRoom(p, "a", {D::Target::Room,"b",D::Where::Before}), {"f","g"}, {{"a",""},{"b",""},{"c","f"},{"w","f"},{"v","f"}}, "room before same kind");
  check(app::ChatMoveRoom(p, "b", {D::Target::Room,"a",D::Where::After}), {"f","g"}, {{"a",""},{"b",""},{"c","f"},{"w","f"},{"v","f"}}, "room after same kind");
  check(app::ChatMoveRoom(p, "a", {D::Target::Folder,"f",D::Where::Into}), {"f","g"}, {{"b",""},{"c","f"},{"a","f"},{"w","f"},{"v","f"}}, "room into folder appends own kind");
  check(app::ChatMoveRoom(p, "a", {D::Target::Room,"v",D::Where::After}), {"f","g"}, {{"b",""},{"c","f"},{"a","f"},{"w","f"},{"v","f"}}, "text onto voice appends texts");
  check(app::ChatMoveRoom(p, "v", {D::Target::Room,"c",D::Where::Before}), {"f","g"}, {{"b",""},{"a",""},{"c","f"},{"v","f"},{"w","f"}}, "voice onto text prepends voices");
  check(app::ChatMoveRoom(p, "c", {D::Target::Folder,"",D::Where::Into}), {"f","g"}, {{"b",""},{"a",""},{"c",""},{"w","f"},{"v","f"}}, "room into top");
  check(app::ChatMoveFolder(p,"g",{D::Target::Folder,"f",D::Where::Before}), {"g","f"}, {{"b",""},{"a",""},{"c","f"},{"w","f"},{"v","f"}}, "folder before keeps channel placement");
  check(app::ChatMoveFolder(p,"f",{D::Target::Folder,"g",D::Where::After}), {"g","f"}, {{"b",""},{"a",""},{"c","f"},{"w","f"},{"v","f"}}, "folder after");
  Check(!app::ChatMoveRoom(p,"b",{D::Target::Room,"a",D::Where::Before}) && !app::ChatMoveRoom(p,"a",{D::Target::Room,"a",D::Where::After}), "unchanged and self drops return null");
  Check(!app::ChatMoveRoom(p,"missing",{}) && !app::ChatMoveRoom(p,"a",{D::Target::Folder,"missing",D::Where::Into}) && !app::ChatMoveRoom(p,"a",{D::Target::Folder,"f",D::Where::Before}) && !app::ChatMoveRoom(p,"a",{D::Target::Room,"missing",D::Where::Before}), "invalid room drops return null");
  Check(!app::ChatMoveFolder(p,"f",{D::Target::Folder,"g",D::Where::Before}) && !app::ChatMoveFolder(p,"f",{D::Target::Folder,"",D::Where::Before}) && !app::ChatMoveFolder(p,"f",{D::Target::Folder,"g",D::Where::Into}) && !app::ChatMoveFolder(p,"f",{D::Target::Room,"a",D::Where::Before}) && !app::ChatMoveFolder(p,"missing",{D::Target::Folder,"g",D::Where::Before}), "unchanged and invalid folder drops return null");
}
void LayoutOperations(const std::filesystem::path& root) {
  Fake f(root / "layout-ops.json");
  const auto original = R"({"channels":[{"id":"t","kind":"text","category_id":"f","position":7},{"id":"v","kind":"voice","category_id":"f","position":8},{"id":"w","kind":"voice","category_id":"f","position":9}],"categories":[{"id":"f","name":"Old","position":0}]})";
  f.intercept = [&](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path == "/api/channels") return net::Response{200, original}; return {};
  };
  f.chat->Resync();
  app::ChatOrder order{{"f"}, {{"v",""},{"t",""},{"w",""}}};
  bool reject = false;
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (path == "/api/channels") return net::Response{200, original};
    if (path != "/api/layout") return {};
    Check(method == "PUT" && (*body)["categories"][0].Str() == "f" && (*body)["channels"].Size() == 3 && !(*body)["channels"][0].Has("position"), "save sends complete order using server shape");
    f.chat->Inspect([](const auto& s) {
      Check(s.conversations[0].category.empty() && s.conversations[0].position == 0 && s.rooms[0].position == 0 && s.rooms[1].position == 1, "layout applied before transport with per-kind counters");
    });
    if (reject) return net::Response{409, R"({"error":"stale_layout"})"};
    return net::Response{200, R"({"categories":[{"id":"f","name":"Server","position":0}],"channels":[{"id":"t","category_id":"","position":2}]})"};
  };
  Check(f.chat->SaveLayout(order).empty(), "save returns empty on success");
  f.chat->Inspect([](const auto& s) { Check(s.categories[0].name == "Server" && s.conversations[0].position == 2, "save applies server layout response"); });
  reject = true;
  Check(f.chat->SaveLayout(order) == "A lista mudou enquanto você arrastava. Ela foi recarregada; tente de novo.", "409 returns web stale-layout explanation");
  f.chat->Inspect([](const auto& s) { Check(s.conversations[0].category == "f" && s.conversations[0].position == 7 && s.rooms[0].position == 8, "rejected optimistic order resyncs original server placement"); });
  int called = 0;
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (method == "GET") return {};
    ++called;
    if (called == 1) {
      Check(method == "PATCH" && path == "/api/channels/v%2F1" && (*body)["name"].Str() == "New", "rename channel escapes id and trims name");
      return net::Response{200, R"({"id":"v","kind":"voice","name":"New"})"};
    }
    if (called == 2) Check(method == "POST" && path == "/api/categories" && (*body)["name"].Str() == "New", "create category route and body");
    if (called == 3) Check(method == "PATCH" && path == "/api/categories/f%2F1" && (*body)["name"].Str() == "New", "rename category route and body");
    if (called == 4) Check(method == "DELETE" && path == "/api/categories/f%2F1" && !body, "delete category route");
    return net::Response{204, ""};
  };
  Check(f.chat->RenameChannel("v/1", " New ").empty(), "rename success");
  f.chat->Inspect([](const auto& s) { Check(s.rooms[0].name == "New", "rename response updates voice immediately"); });
  Check(f.chat->CreateCategory(" New ").empty() && f.chat->RenameCategory("f/1", " New ").empty() && f.chat->RemoveCategory("f/1").empty() && called == 4, "category operations succeed and reload metadata");
  // Creating and deleting a channel: the answer lands in the model at once.
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (method == "POST" && path == "/api/channels") {
      Check((*body)["name"].Str() == "novo" && (*body)["kind"].Str() == "voice" && (*body)["topic"].Str().empty(), "create channel sends name, kind and empty topic");
      return net::Response{201, R"({"id":"n1","kind":"voice","name":"novo"})"};
    }
    if (method == "DELETE") { Check(path == "/api/channels/n1", "delete channel route"); return net::Response{200, R"({"ok":true})"}; }
    return {};
  };
  std::string made;
  Check(f.chat->CreateChannel(" novo ", "voice", &made).empty() && made == "n1", "create channel returns the new id");
  f.chat->Inspect([](const auto& s) { Check(std::any_of(s.rooms.begin(), s.rooms.end(), [](const auto& r) { return r.id == "n1"; }), "a created voice room is in the model at once"); });
  Check(f.chat->RemoveChannel("n1").empty(), "delete channel success");
  f.chat->Inspect([](const auto& s) { Check(std::none_of(s.rooms.begin(), s.rooms.end(), [](const auto& r) { return r.id == "n1"; }), "a deleted room leaves the model at once"); });
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{403, R"({"error":"not_allowed"})"}; };
  const auto denied = "Você não tem permissão para isso.";
  Check(f.chat->CreateChannel("x", "text") == denied && f.chat->RemoveChannel("t") == denied, "create and delete channel return the server refusal");
  Check(f.chat->RenameChannel("t","x") == denied && f.chat->CreateCategory("x") == denied && f.chat->RenameCategory("f","x") == denied && f.chat->RemoveCategory("f") == denied, "all metadata operations return Portuguese server refusal");
  for (const auto& [code, expected] : std::vector<std::pair<std::string,std::string>>{
      {"bad_channel_name","Nome de canal inválido: letras minúsculas, números e traço."},
      {"bad_category_name","Dê um nome à categoria, com até 32 caracteres."}, {"name_taken","Já existe um com esse nome."},
      {"pin_limit","Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra."}}) {
    auto v = json::Value::Object(); v["error"] = code;
    Check(app::ChatError({400,v.Dump()}) == expected, "new errors exactly match api.ts");
  }
}
void Pins(const std::filesystem::path& root) {
  Fake f(root / "pins.json"); f.chat->Open("t");
  auto text = app::ParseChatMessage(Message(3));
  auto dm = app::ParseChatMessage(Message(5,"d"));
  auto voice = app::ParseChatMessage(Message(6,"v"));
  f.chat->Event("channel", *json::Parse(R"({"id":"v","kind":"voice"})"));
  Check(!f.chat->CanPin(text) && f.chat->CanPin(dm) && !f.chat->CanPin(voice), "member can pin only in DM");
  auto deleted = dm; deleted.deleted = true; auto note = dm; note.kind = "pin";
  Check(!f.chat->CanPin(deleted) && !f.chat->CanPin(note), "deleted and system messages cannot be pinned");
  auto unknown = dm; unknown.channel = "unknown";
  Check(!f.chat->CanPin(unknown), "unknown membership does not allow pinning");
  bool manage = false;
  f.intercept = [&](const auto& method, const auto& path, const auto*) -> std::optional<net::Response> {
    if (!path.ends_with("/pin")) return {};
    Check(method == "POST", "permission matrix uses pin POST");
    if (path == "/api/messages/5/pin" || (manage && path == "/api/messages/3/pin")) {
      auto m = Message(path == "/api/messages/5/pin" ? 5 : 3, path == "/api/messages/5/pin" ? "d" : "t");
      m["pinned"] = true; return net::Response{200,m.Dump()};
    }
    return net::Response{403,R"({"error":"not_allowed"})"};
  };
  Check(!f.chat->Pin("t",3,true).empty() && f.chat->Pin("d",5,true).empty() && !f.chat->Pin("v",6,true).empty(), "member pin responses across text, DM and voice");
  manage = true;
  Check(f.chat->Pin("t",3,true).empty() && f.chat->Pin("d",5,true).empty() && !f.chat->Pin("v",6,true).empty(), "owner pin responses across text, DM and voice");
  // Gerenciar canais (can_manage) alone does not pin since D12c.
  f.intercept = [](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path == "/api/channels") return net::Response{200, R"({"can_manage":true,"channels":[{"id":"t","kind":"text"},{"id":"d","kind":"dm"},{"id":"v","kind":"voice"}]})"}; return {};
  };
  f.chat->Resync();
  Check(!f.chat->CanPin(text) && f.chat->CanPin(dm), "managing channels does not pin in a channel");
  f.intercept = [](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path == "/api/me") return net::Response{200, R"({"user":{"username":"alice","is_admin":true}})"};
    if (path == "/api/channels") return net::Response{200, R"({"can_manage":true,"channels":[{"id":"t","kind":"text"},{"id":"d","kind":"dm"},{"id":"v","kind":"voice"}]})"}; return {};
  };
  f.chat->Resync();
  Check(f.chat->CanPin(text) && f.chat->CanPin(dm) && !f.chat->CanPin(voice) && !f.chat->CanPin(deleted) && !f.chat->CanPin(note), "owner can pin ordinary text and DM messages only");
  f.page = json::Value::Object(); f.page["messages"] = json::Value::Array(); f.page["messages"].Push(Message(3,"d"));
  f.chat->Open("d");
  int requests = 0;
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (!path.ends_with("/pin")) return {};
    ++requests;
    Check(path == "/api/messages/3/pin" && !body && method == (requests == 1 ? "POST" : "DELETE"), "pin endpoint and verbs match contract");
    auto m = Message(3, requests == 1 ? "t" : "d"); if (requests == 1) m["pinned"] = true;
    return net::Response{200,m.Dump()};
  };
  Check(f.chat->Pin("t",3,true).empty() && f.Timeline().messages[0].pinned, "pin HTTP response upserts loaded text");
  Check(f.chat->Pin("d",3,false).empty() && !f.Timeline("d").messages[0].pinned, "unpin HTTP response upserts loaded DM");
  auto changed = Message(3); changed["pinned"] = true; f.chat->Event("message_changed",changed);
  Check(f.Timeline().messages[0].pinned, "pin event updates existing row");
  f.chat->Event("message_changed",Message(3));
  Check(!f.Timeline().messages[0].pinned, "absent pinned in event clears flag");
  f.intercept = [&](const auto& method, const auto& path, const auto*) -> std::optional<net::Response> {
    Check(method == "GET" && path == "/api/channels/d%2F1/pins", "pins escapes channel id");
    auto v = json::Value::Object(); v["messages"] = json::Value::Array();
    for (auto id : {9,2,7}) { auto m = Message(id,"d/1"); m["pinned"] = true; v["messages"].Push(m); }
    v["limit"] = 250; return net::Response{200,v.Dump()};
  };
  std::string error = "old"; auto pins = f.chat->Pins("d/1",error);
  Check(error.empty() && pins.size() == 3 && pins[0].id == 9 && pins[1].id == 2 && pins[2].id == 7 && pins[2].pinned, "pins preserves server pin-time order rather than id order");
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{404,R"({"error":"not_found"})"}; };
  Check(f.chat->Pins("private",error).empty() && error == "Não encontrado.", "pins returns server membership refusal");
  Check(f.chat->Pin("v",3,true) == "Não encontrado.", "pin propagates server refusal on voice");
  f.intercept = [](const auto&, const auto&, const auto*) -> std::optional<net::Response> { return net::Response{409,R"({"error":"pin_limit"})"}; };
  Check(f.chat->Pin("t",3,true) == "Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra." && !f.Timeline().messages[0].pinned, "failed pin preserves row and returns exact error");
}
void LayoutData(const std::filesystem::path& root) {
  Fake f(root / "layout-data.json");
  f.intercept = [](const auto&, const auto& path, const auto*) -> std::optional<net::Response> {
    if (path == "/api/channels") return net::Response{200, R"({"channels":[{"id":"t","kind":"text","category_id":"f","position":2},{"id":"v","kind":"voice","name":"Voice","category_id":"f","position":3}],"categories":[{"id":"f","name":"Folder","position":1}]})"};
    return {};
  };
  f.chat->Resync();
  f.chat->Inspect([](const auto& s) {
    Check(s.conversations.size() == 1 && s.conversations[0].category == "f" && s.conversations[0].position == 2, "resync retains text placement without voice conversations");
    Check(s.rooms.size() == 1 && s.rooms[0].position == 3 && s.categories[0].name == "Folder", "resync reads folders and voice placement");
  });
  auto announced = f.announcements;
  Check(f.chat->Event("layout", *json::Parse(R"({"categories":[],"channels":[{"id":"t","category_id":"","position":4},{"id":"v","category_id":"","position":5},{"id":"unknown","position":6}]})")), "layout event handled");
  Check(f.announcements == announced + 1, "layout event invokes changed callback");
  f.chat->Inspect([](const auto& s) {
    Check(s.categories.empty() && s.conversations.size() == 1 && s.conversations[0].position == 4 && s.conversations[0].category.empty() && s.rooms[0].position == 5, "layout updates known text and voice only");
    Check(s.changes.back().kind == app::ChatChangeKind::Metadata, "layout announces metadata change");
  });
  f.chat->Event("channel", *json::Parse(R"({"id":"v","kind":"voice","name":"Renamed"})"));
  f.chat->Inspect([](const auto& s) { Check(s.rooms[0].name == "Renamed", "voice channel event updates name"); });
  f.chat->Event("channel_removed", *json::Parse(R"({"id":"v"})"));
  f.chat->Inspect([](const auto& s) { Check(s.rooms.empty(), "removed voice leaves rooms"); });
  auto m = Message(1); m["pinned"] = true;
  Check(app::ParseChatMessage(m).pinned && !app::ParseChatMessage(Message(1)).pinned, "pinned defaults false when absent");
}
// Roles (D12c), the D12e criterion in the model: the moderator's name turns
// green, deleting and pinning follow Gerenciar mensagens in text channels only,
// and taking the role away takes them away, live, from the `roles` event.
void Roles(const std::filesystem::path& root) {
  auto role = [](std::string id, std::string name, std::string color, int64_t pos, bool hoist,
      std::vector<std::string> perms, std::vector<std::string> members) {
    app::ChatRole r; r.id = id; r.name = name; r.color = color; r.position = pos; r.hoist = hoist; r.permissions = perms; r.members = members; return r;
  };
  std::vector<app::ChatRole> roles{role("b", "Membro", "", 1, true, {}, {"bob", "carol"}),
      role("a", "Moderador", "#2ecc71", 0, false, {"manage_messages"}, {"bob"}),
      role("c", "Cinza", "#95a5a6", 2, true, {"manage_channels"}, {"carol"})};
  Check(app::ChatRolesOf("bob", roles).size() == 2 && app::ChatRolesOf("bob", roles)[0].id == "a", "somebody's roles come highest first");
  Check(app::ChatColorOf("bob", roles) == "#2ecc71" && app::ChatColorOf("carol", roles) == "#95a5a6" && app::ChatColorOf("dave", roles).empty(),
      "the name takes the highest role that has a colour, and none without one");
  Check(app::ChatColorValue("#2ecc71") == 0x2ecc71u && !app::ChatColorValue("") && !app::ChatColorValue("#12345g"), "colours parse as hex and nothing else");
  Check(app::ChatHoistOf("bob", roles)->id == "b" && app::ChatHoistOf("carol", roles)->id == "b" && !app::ChatHoistOf("dave", roles), "the group is the highest role shown apart");
  Check(app::ChatPermissionsOf("bob", false, roles) == std::set<std::string>{"manage_messages"} && app::ChatPermissionsOf("x", true, {}).size() == 3,
      "permissions are the union of roles, and everything for the owner");
  Check(app::ChatTopOf("bob", false, roles) == 0 && app::ChatTopOf("x", true, roles) == -1 && app::ChatTopOf("dave", false, roles) == std::numeric_limits<int64_t>::max(),
      "the top is the highest position, -1 for the owner");
  Check(*app::ChatMoveRole(roles, "c", "a", true) == std::vector<std::string>{"c", "a", "b"} && *app::ChatMoveRole(roles, "a", "c", false) == std::vector<std::string>{"b", "c", "a"},
      "dragging a role sends the whole order");
  Check(!app::ChatMoveRole(roles, "a", "a", true) && !app::ChatMoveRole(roles, "a", "b", true) && !app::ChatMoveRole(roles, "x", "a", true), "a drop that moves nothing sends nothing");

  // bob, a member with the role, live.
  Fake f(root / "roles.json"); f.owner = "bob";
  bool withRole = false; std::vector<std::string> writes;
  auto list = [&] {
    auto v = json::Value::Object(); v["roles"] = json::Value::Array(); v["colors"] = json::Value::Array(); v["colors"].Push("#2ecc71");
    auto r = json::Value::Object(); r["id"] = "a"; r["name"] = "Moderador"; r["color"] = "#2ecc71"; r["position"] = 0; r["hoist"] = false;
    r["permissions"] = json::Value::Array(); r["permissions"].Push("manage_messages"); r["members"] = json::Value::Array(); if (withRole) r["members"].Push("bob");
    v["roles"].Push(r); return v;
  };
  f.intercept = [&](const auto& method, const auto& path, const auto* body) -> std::optional<net::Response> {
    if (path == "/api/roles" && method == "GET") return net::Response{200, list().Dump()};
    if (path.starts_with("/api/roles")) {
      writes.push_back(method + " " + path + (body ? " " + body->Dump() : ""));
      if (path == "/api/roles/a/members/bob") withRole = method == "PUT";
      if (path == "/api/roles/order") return net::Response{409, R"({"error":"stale_roles"})"};
      return net::Response{method == "POST" ? 201 : 200, R"({"id":"n"})"};
    }
    return {};
  };
  f.chat->Resync(); f.chat->Open("t");
  auto alices = app::ParseChatMessage(Message(3)); auto inDM = app::ParseChatMessage(Message(5, "d"));
  f.chat->Inspect([&](const auto& s) { Check(s.palette.size() == 1 && s.roles.size() == 1 && !s.Allowed("manage_messages") && !s.admin, "the list and palette come with the resync"); });
  Check(!f.chat->CanDelete(alices) && !f.chat->CanPin(alices), "without the role, no deleting or pinning somebody else's message");
  withRole = true; f.chat->Event("roles", list());
  uint64_t version = 0;
  f.chat->Inspect([&](const auto& s) { version = s.rolesVersion; Check(app::ChatColorOf("bob", s.roles) == "#2ecc71" && s.Allowed("manage_messages") && !s.canModerate,
      "the roles event turns the name green and gives Gerenciar mensagens, not Gerenciar canais"); });
  Check(f.chat->CanDelete(alices) && f.chat->CanPin(alices), "with the role, deleting and pinning in a text channel");
  Check(!f.chat->CanDelete(inDM) && f.chat->CanPin(inDM), "in a DM the role does not delete the other's message");
  withRole = false; f.chat->Event("roles", list());
  f.chat->Inspect([&](const auto& s) { Check(s.rolesVersion > version && app::ChatColorOf("bob", s.roles).empty(), "taking the role repaints the name"); });
  Check(!f.chat->CanDelete(alices) && !f.chat->CanPin(alices), "taking the role takes deleting and pinning away at once");
  Check(!f.chat->Event("roles", *json::Parse(R"({"other":1})")), "a roles event without a list changes nothing");

  // Writes: routes, bodies, the list read back, Portuguese refusals.
  std::string made;
  Check(f.chat->CreateRole(&made).empty() && made == "n", "create returns the new id");
  auto patch = json::Value::Object(); patch["color"] = "#2ecc71";
  Check(f.chat->UpdateRole("a/1", patch).empty() && f.chat->RoleMember("a", "bob", true).empty(), "edit and give succeed");
  f.chat->Inspect([&](const auto& s) { Check(app::ChatColorOf("bob", s.roles) == "#2ecc71", "a write reads the list back without waiting for the event"); });
  Check(f.chat->RoleMember("a", "bob", false).empty() && f.chat->RemoveRole("a").empty(), "take and delete succeed");
  Check(f.chat->OrderRoles({"a"}) == "A lista de cargos mudou enquanto você arrastava. Ela foi recarregada; tente de novo.", "a stale order explains itself as the web does");
  Check(writes.size() == 6 && writes[0] == R"(POST /api/roles {"color":"","hoist":false,"name":"Novo cargo","permissions":[]})" &&
      writes[1] == R"(PATCH /api/roles/a%2F1 {"color":"#2ecc71"})" && writes[2] == "PUT /api/roles/a/members/bob" &&
      writes[3] == "DELETE /api/roles/a/members/bob" && writes[4] == "DELETE /api/roles/a" && writes[5] == R"(PUT /api/roles/order {"roles":["a"]})",
      "role writes follow api/roles.go");
  for (const auto& [code, expected] : std::vector<std::pair<std::string,std::string>>{
      {"not_allowed","Você não tem permissão para isso."}, {"bad_role_name","Dê um nome ao cargo, com até 32 caracteres."},
      {"bad_color","Escolha uma das cores da lista."}, {"role_limit","O servidor já tem 50 cargos. Apague um para criar outro."}}) {
    auto v = json::Value::Object(); v["error"] = code;
    Check(app::ChatError({400,v.Dump()}) == expected, "role errors exactly match api.ts");
  }
}
void PersonalPreferences(const std::filesystem::path& root) {
  Fake f(root / "personal.json");
  std::string stored = R"({"items":[{"username":"bob","muted":true,"blocked":false}]})";
  f.intercept = [&](auto const& method, auto const& path, auto const*) -> std::optional<net::Response> {
    if (path == "/api/me/blocks") return net::Response{200, stored};
    if (path == "/api/me/blocks/bob" && method == "PUT") {
      stored = R"({"items":[{"username":"bob","muted":true,"blocked":true}]})"; return net::Response{200, stored};
    }
    if (path == "/api/me/blocks/bob" && method == "DELETE") {
      stored = R"({"items":[{"username":"bob","muted":true,"blocked":false}]})"; return net::Response{200, stored};
    }
    return {};
  };
  f.chat->RefreshPersonal();
  f.chat->Inspect([&](auto const& s) { Check(s.personalReady && s.Silenced("bob") && !s.Silenced("bob", true), "personal silence preserves screen sound"); });
  uint64_t version = 0; f.chat->Inspect([&](auto const& s) { version = s.personalVersion; });
  f.chat->RefreshPersonal();
  f.chat->Inspect([&](auto const& s) { Check(s.personalVersion == version, "duplicate snapshot does not fold an individually revealed message again"); });
  Check(f.chat->Personal("bob", true, true).empty(), "block uses authenticated own-list endpoint");
  f.chat->Inspect([&](auto const& s) { Check(s.Blocked("bob") && s.Silenced("bob", true), "block gates screen and microphone"); });
  auto message = Message(5, "t", "bob"); message["mentions"].Push("alice"); f.chat->Event("message", message);
  f.chat->Inspect([&](auto const& s) { for (auto const& c : s.conversations) if (c.id == "t") Check(c.unread == 0 && c.mentions == 0, "blocked message raises neither unread nor mention"); });
  Check(f.chat->Personal("bob", true, false).empty(), "unblock uses DELETE");
  f.chat->Inspect([&](auto const& s) { Check(!s.Blocked("bob") && s.Silenced("bob") && !s.Silenced("bob", true), "unblock preserves independent mute"); });
  stored = R"({"items":[{"username":"bob","muted":true,"blocked":true}]})"; f.chat->RefreshPersonal();
  stored = "{}"; f.chat->RefreshPersonal();
  f.chat->Inspect([&](auto const& s) { Check(s.personalReady && s.Blocked("bob") && s.Silenced("bob") && s.Silenced("bob", true), "a failed refresh keeps the last known list"); });
  f.chat->Forget();
  f.chat->Inspect([&](auto const& s) { Check(s.personal.empty() && !s.personalReady && !s.Silenced("bob"), "logout clears private preferences, and an unknown list silences nobody"); });
  Check(app::ChatError({409, R"({"error":"delivery_failed"})"}) == "Não foi possível entregar.", "blocked sender receives generic wording");
}
void UnitTests(const std::filesystem::path& root) {
  {
    Fake f(root/"resource-census.json");f.chat->Open("t");f.chat->PublishResources();
    auto first=resources::Read();f.chat->PublishResources();auto same=resources::Read();
    Check(first.channels==same.channels && first.channelNames==same.channelNames,"unchanged chat keeps exact resource census");
    f.chat->Event("message",Message(5));f.chat->PublishResources();
    auto changed=resources::Read();Check(changed.channels!=first.channels,"message invalidates resource census");
    f.chat->Forget();f.chat->PublishResources();Check(resources::Read().channels.empty(),"sign-out clears cached resource census");
  }

  PersonalPreferences(root);
  std::filesystem::create_directories(root);
  Roles(root); Reorder(); Pins(root); LayoutOperations(root); LayoutData(root); Paging(root); Sending(root); Changes(root); TypingAndRead(root); Drafts(root); PureFunctions(); Memory(root); Conversations(root);
  Check(app::ChatTrim(" \t\r\n\xc2\xa0\xe2\x80\x83") == "", "composer ignores all JavaScript whitespace");
  Check(app::ChatDownloadName("bruna", "", std::string_view("\x89PNG\r\n", 6)) == "bruna.png" && app::ChatDownloadName("foto", "", "\xff\xd8\xff\xe0") == "foto.jpg" &&
        app::ChatDownloadName("x", "image/webp", "") == "x.webp" && app::ChatDownloadName("notas.txt", "", "") == "notas.txt" && app::ChatDownloadName("", "", "") == "arquivo.bin",
        "downloads always carry an extension: from the name, the bytes, then the type");
  Check(app::ChatTrim("\xe2\x80\x83hello\xc2\xa0") == "hello", "composer trims Unicode edges without touching the body");
  const char* oldZone = std::getenv("TZ");
  std::string savedZone = oldZone ? oldZone : "";
  _putenv_s("TZ", "UTC0"); _tzset();
  auto now = app::ChatSeconds("2026-10-18T12:00:00Z");
  Check(app::ChatCallText(true,false,L"Alice") == L"Voc\u00ea ligou \u2014 ningu\u00e9m atendeu", "outgoing unanswered call explains its outcome");
  Check(app::ChatCallText(false,false,L"Alice") == L"Chamada perdida de Alice", "incoming missed call identifies the caller");
  Check(app::ChatCallText(true,true,L"Alice") == L"Alice recusou a chamada", "outgoing rejected call follows web wording");
  Check(app::ChatCallText(false,true,L"Alice") == L"Voc\u00ea recusou a chamada", "incoming rejected call follows web wording");
  Check(app::ChatPinText(false,L"Alice") == L"Alice fixou uma mensagem.", "a pin line names who pinned, as on the web");
  Check(app::ChatPinText(true,L"Alice") == L"Voc\u00ea fixou uma mensagem.", "your own pin line says you");
  Check(app::ChatShortDate("2026-10-18T10:02:03Z") == L"18/10/26", "search date includes two-digit year in local time");
  Check(app::ChatClock("2026-10-18T10:02:03Z", now) == L"10:02", "today shows only the clock");
  Check(app::ChatClock("2026-10-17T23:59:00Z", now) == L"Ontem 23:59", "yesterday remains identifiable away from its day separator");
  Check(app::ChatClock("2025-10-16T09:04:00Z", now) == L"16/10 09:04", "older history shows date and clock like pt-BR web");
  Check(app::ChatDay("2025-10-02T12:00:00Z") == L"quinta-feira, 02 de outubro", "day separator uses two digits without an extra year");
  Check(app::ChatFullStamp("2026-10-18T10:02:03Z") == L"domingo, 18 de outubro de 2026 às 10:02:03", "timestamp tooltip includes the complete local date and seconds");
  Check(app::ChatSeconds("2026-10-18T10:02:03.456-03:00") == app::ChatSeconds("2026-10-18T13:02:03Z"), "server timestamps honor their UTC offset");
  Check(app::ChatMilliseconds("2026-10-18T10:05:00.100Z") - app::ChatMilliseconds("2026-10-18T10:00:00.900Z") == 299200, "message grouping keeps subsecond precision at five minutes");
  _putenv_s("TZ", savedZone.c_str()); _tzset();
  std::vector<app::ChatConversation> conversations;
  for (auto [id, kind, last] : {std::tuple{"text-b", "text", 1}, {"old", "dm", 3}, {"text-a", "text", 9},
                               {"new", "dm", 7}, {"empty", "dm", 0}, {"tie", "dm", 7}}) {
    app::ChatConversation c; c.id = id; c.kind = kind; c.lastMessage = last; conversations.push_back(c);
  }
  app::ChatOrderConversations(conversations);
  std::string order; for (const auto& c : conversations) order += c.id + ",";
  Check(order == "empty,new,tie,old,text-b,text-a,", "DM order follows activity, keeps ties stable and preserves text channel order");
  app::ChatTimeline unread;
  unread.messages.push_back(app::ParseChatMessage(Message(2, "t", "alice")));
  unread.messages.push_back(app::ParseChatMessage(Message(3, "t", "bob")));
  Check(app::ChatUnreadAt(unread, 1, "alice") == 3 && app::ChatUnreadAt(unread, 0, "alice") == 0,
        "unread separator skips own messages and does not invent an opening mark");
  Check(app::ChatInitials(L"  Álvaro da Silva\u00a0") == L"ÁS", "avatar initials uppercase accented names without depending on the process locale");
  Check(app::ChatInitials(L"Sol 🌞") == L"S" && app::ChatInitials(L"1 pessoa") == L"1P", "avatar initials skip symbols and keep first and last eligible words");
}
}
namespace {
std::string RandomSecret() {
  std::array<unsigned char, 24> bytes{};
  Check(BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "generate fixture password");
  const char* hex = "0123456789abcdef"; std::string result;
  for (auto b : bytes) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
}
std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }
struct NativeHandle {
  HANDLE value = nullptr;
  ~NativeHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// Only fixture children are assigned to this job. Closing it cannot terminate
// another agent's server, and go run's compiled child cannot outlive our test.
class LocalServer {
 public:
  explicit LocalServer(const std::filesystem::path& output) {
    auto cwd = std::filesystem::current_path();
    while (!std::filesystem::exists(cwd / L"go.mod") && cwd.has_parent_path() && cwd.parent_path() != cwd) cwd = cwd.parent_path();
    Check(std::filesystem::exists(cwd / L"cmd/harmony/main.go"), "run test from harmony worktree"); repo_ = cwd;
    root_ = std::filesystem::absolute(output / (L"local-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root_ / L"files");
    // Clean allowlist: the local fixture never inherits mail keys or production
    // configuration. These are OS/toolchain locations, not account data.
    std::map<std::wstring, std::wstring> environment;
    for (auto name : {L"SystemRoot", L"WINDIR", L"PATH", L"TEMP", L"TMP", L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
      wchar_t buffer[32768]; auto n = GetEnvironmentVariableW(name, buffer, DWORD(std::size(buffer)));
      if (n && n < std::size(buffer)) environment[name] = buffer;
    }
    environment[L"HARMONY_INSECURE_COOKIE"] = L"1";
    environment[L"HARMONY_BASE_URL"] = L"http://127.0.0.1:8093";
    environment[L"HARMONY_EMAIL_REQUIRED"] = L"0";
    for (const auto& [name, value] : environment) {
      environment_.insert(environment_.end(), name.begin(), name.end()); environment_.push_back(L'=');
      environment_.insert(environment_.end(), value.begin(), value.end()); environment_.push_back(0);
    }
    environment_.push_back(0);
    job_.value = CreateJobObjectW(nullptr, nullptr); Check(job_.value != nullptr, "create isolated fixture process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Check(SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE, "fixture descendants terminate with test");
    SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, TRUE};
    null_.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
    Check(null_.value != INVALID_HANDLE_VALUE, "open silent fixture output");
  }
  ~LocalServer() { if (job_.value) TerminateJobObject(job_.value, 0); }
  std::vector<std::string> Start() {
    // Refuse a port already serving anything. Never reuse or stop its owner.
    net::Client probe("http://127.0.0.1:8093");
    Check(probe.Get("/api/me").status == 0, "port 8093 must be free before local self-test");
    SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, TRUE}; NativeHandle read, write;
    Check(CreatePipe(&read.value, &write.value, &attributes, 0) != FALSE, "capture local invite output privately");
    SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0);
    auto mint = Spawn(L" -invite -n 3 -db " + Quote(root_ / L"h.db"), write.value);
    CloseHandle(write.value); write.value = nullptr;
    std::string captured; std::array<char, 4096> buffer; DWORD count = 0;
    const auto mintDeadline = GetTickCount64() + 180000;
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr)) break;
      if (available) {
        Check(ReadFile(read.value, buffer.data(), std::min<DWORD>(available, DWORD(buffer.size())), &count, nullptr) != FALSE, "read private invite response");
        Check(captured.size() + count < 65536, "bounded invite command output"); captured.append(buffer.data(), count);
      } else if (WaitForSingleObject(mint, 0) == WAIT_OBJECT_0) break;
      else { Check(GetTickCount64() < mintDeadline, "local invite generation deadline"); Sleep(20); }
    }
    Check(WaitForSingleObject(mint, 180000) == WAIT_OBJECT_0, "local invite generation completes");
    DWORD exit = 1; GetExitCodeProcess(mint, &exit); CloseHandle(mint);
    Check(exit == 0, "local invite command succeeds");
    std::vector<std::string> invites; std::istringstream lines(captured); std::string line;
    while (std::getline(lines, line)) if (line.starts_with("invite  ")) {
      auto code = line.substr(8); if (!code.empty() && code.back() == '\r') code.pop_back(); invites.push_back(code);
    }
    SecureZeroMemory(captured.data(), captured.size()); Check(invites.size() == 3, "three single-use local fixture invites minted");
    server_.value = Spawn(L" -addr 127.0.0.1:8093 -db " + Quote(root_ / L"h.db") + L" -files " + Quote(root_ / L"files") + L" -voice-port 0", null_.value);
    auto deadline = GetTickCount64() + 180000;
    while (GetTickCount64() < deadline) {
      Check(WaitForSingleObject(server_.value, 0) == WAIT_TIMEOUT, "fixture server remains running");
      if (probe.Get("/api/me").status == 401) return invites;
      Sleep(100);
    }
    throw std::runtime_error("local server startup timeout");
  }
  const std::filesystem::path& root() const { return root_; }
 private:
  HANDLE Spawn(const std::wstring& arguments, HANDLE output) {
    const std::filesystem::path go = L"C:/Program Files/Go/bin/go.exe";
    auto command = Quote(go) + L" run ./cmd/harmony" + arguments;
    STARTUPINFOW startup{sizeof startup}; startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE; startup.hStdInput = null_.value; startup.hStdOutput = output; startup.hStdError = null_.value;
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(go.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, environment_.data(), repo_.c_str(), &startup, &process) != FALSE,
        "start go run in local worktree");
    if (!AssignProcessToJobObject(job_.value, process.hProcess)) {
      TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess);
      throw std::runtime_error("cannot isolate fixture process job");
    }
    ResumeThread(process.hThread); CloseHandle(process.hThread); return process.hProcess;
  }
  std::filesystem::path repo_, root_;
  std::vector<wchar_t> environment_;
  NativeHandle null_, job_, server_;
};
struct LiveClient {
  net::Client net{"http://127.0.0.1:8093"};
  std::mutex auditMu;
  std::map<std::string, int> statuses;
  app::Chat chat;
  std::unique_ptr<net::Socket> socket;
  std::atomic<bool> ready{false};
  LiveClient(const std::filesystem::path& draft) : chat([this](const auto& method, const auto& path, const auto* body) {
    auto r = app::ChatRequest(net, method, path, body);
    { std::lock_guard lock(auditMu); statuses[path] = r.status; }
    return r;
  }, [] {}, draft) {}
  ~LiveClient() { if (socket) socket->Stop(); }
  void Signup(const std::string& code, const std::string& username) {
    auto body = json::Value::Object(); auto password = RandomSecret();
    body["username"] = username; body["display_name"] = username; body["code"] = code;
    body["email"] = username + "@example.invalid"; body["password"] = password;
    auto r = net.Post("/api/signup", body); body = {}; SecureZeroMemory(password.data(), password.size());
    Check(r.status == 201 && !net.cookie().empty(), "signup creates isolated local session");
    socket = std::make_unique<net::Socket>(&net, [this](const auto& type, const auto& data) { chat.Event(type, data); },
        [this] { chat.Resync(); ready = true; }); socket->Start();
  }
  app::ChatTimeline Timeline(const std::string& channel) {
    app::ChatTimeline t; chat.Inspect([&](const auto& s) { if (s.timelines.contains(channel)) t = s.timelines.at(channel); }); return t;
  }
};
template<class Predicate> void Await(Predicate predicate, const char* name, uint64_t timeout = 15000) {
  auto deadline = GetTickCount64() + timeout;
  while (GetTickCount64() < deadline) { if (predicate()) { Check(true, name); return; } Sleep(20); }
  Check(false, name);
}
void Integration(const std::filesystem::path& output) {
  auto result = json::Value::Object(); result["passed"] = false;
  auto write = [&] {
    auto temp = output; temp += L".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << result.Dump(); out.close();
    if (out) MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  };
  try {
    result["stage"] = "local_server"; write();
    LocalServer fixture(output.parent_path()); auto invites = fixture.Start();
    LiveClient a(fixture.root()/L"a.json"), b(fixture.root()/L"b.json"), c(fixture.root()/L"c.json");
    const auto suffix = std::to_string(GetCurrentProcessId());
    const auto userA = "w5a" + suffix, userB = "w5b" + suffix, userC = "w5c" + suffix;
    a.Signup(invites[0], userA); b.Signup(invites[1], userB); c.Signup(invites[2], userC);
    for (auto& code : invites) SecureZeroMemory(code.data(), code.size());
    Await([&] { return a.ready && b.ready && c.ready; }, "both chat clients and privacy witness connect", 30000);
    a.chat.OpenDM(userB);
    std::string dm; a.chat.Inspect([&](const auto& s) { dm = s.active; }); Check(!dm.empty(), "shared DM opens");
    Check(a.chat.Nickname(userB, "Private nickname").status == 200, "private nickname saved through web endpoint");
    auto nickOf = [&](app::Chat& chat) { std::string nick; chat.Inspect([&](const auto& state) { for (const auto& person : state.members) if (person.username == userB) nick = person.nickname; }); return nick; };
    Check(nickOf(a.chat) == "Private nickname", "nickname updates own member cache");
    c.chat.Resync();
    Check(nickOf(c.chat).empty(), "user C cannot see A's private nickname for B");
    a.chat.Resync(); Check(nickOf(a.chat) == "Private nickname", "private nickname survives resync");
    Check(a.chat.Nickname(userB, "").status == 200 && nickOf(a.chat).empty(), "empty nickname removes private override");
    b.chat.Open(dm);
    result["stage"] = "round_trip"; write();
    a.chat.Draft(dm, "round trip fixture"); a.chat.Send(dm, "round trip fixture");
    Await([&] { return b.Timeline(dm).messages.size() == 1; }, "send reaches second client via gateway");
    auto id = b.Timeline(dm).messages.back().id;
    a.chat.Edit(dm, id, "edited fixture");
    Await([&] { return b.Timeline(dm).messages.back().body == "edited fixture"; }, "edit reaches second client via gateway");
    b.chat.React(dm, id, "👍", true);
    Await([&] { auto t = a.Timeline(dm); return !t.messages.back().reactions.empty() && t.messages.back().reactions[0].Includes(userB); }, "reaction reaches original author");
    b.chat.React(dm, id, "👍", false);
    Await([&] { return a.Timeline(dm).messages.back().reactions.empty(); }, "reaction removal reaches original author");
    b.chat.Draft(dm, "reply fixture", id); b.chat.Send(dm, "reply fixture", {}, id);
    Await([&] { auto t = a.Timeline(dm); return t.messages.size() == 2 && t.messages.back().reply && t.messages.back().reply->id == id; }, "reply reaches original author with reference");
    a.chat.Remove(dm, id);
    Await([&] { return b.Timeline(dm).messages.front().deleted; }, "delete reaches second client as tombstone");
    result["round_trip"] = true;
    // A link sent at once gets its card on both sides with no second send (the
    // owner, 30/10/2026). The local server reads Vimeo's oEmbed on the internet.
    a.chat.Send(dm, "https://vimeo.com/76979871");
    Await([&] { auto t = a.Timeline(dm); return !t.messages.empty() && t.messages.back().embeds.size() == 1; }, "the sender sees the link card without sending again", 15000);
    Await([&] { auto t = b.Timeline(dm); return !t.messages.empty() && t.messages.back().embeds.size() == 1; }, "the other side sees the link card too", 15000);
    result["link_card"] = true;
    b.chat.OpenDM(userC); std::string privateDM; b.chat.Inspect([&](const auto& s) { privateDM = s.active; });
    b.chat.Send(privateDM, "isolationsearchfixture");
    Check(b.chat.Search("isolationsearchfixture").Json()["messages"].Size() == 1, "private search fixture is visible to its member");
    Check(a.chat.Search("isolationsearchfixture").Json()["messages"].Size() == 0, "search does not reveal another person's private DM");
    // This is a real HTTP request, even though the channel is absent from A's
    // sidebar. Owner/admin privileges do not grant membership in B-C's DM.
    a.chat.Open(privateDM);
    auto refused = a.Timeline(privateDM);
    Check(refused.messages.empty() && refused.error == "Não encontrado.", "user A cannot read B-C DM and receives Portuguese error");
    { std::lock_guard lock(a.auditMu);
      Check(a.statuses["/api/channels/" + app::ChatEscape(privateDM) + "/messages"] == 404,
          "DM isolation was refused by real HTTP 404, not a local membership guard"); }
    result["dm_isolation"] = true;
    a.chat.Open(dm); b.chat.Open(dm);
    const std::array<unsigned char, 67> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,10,73,68,65,84,120,156,99,0,1,0,0,5,0,1,13,10,45,180,0,0,0,0,73,69,78,68,174,66,96,130};
    auto imagePath = fixture.root()/L"fixture.png";
    { std::ofstream file(imagePath, std::ios::binary); file.write(reinterpret_cast<const char*>(png.data()), png.size()); }
    std::atomic<bool> cancel{false}; uint64_t progressed = 0;
    auto uploaded = a.chat.Upload(a.net, dm, imagePath, [&](uint64_t sent, uint64_t total) { Check(sent <= total && sent >= progressed, "upload progress is monotonic"); progressed = sent; }, cancel);
    auto attachment = uploaded.Json(); Check(uploaded.status == 201 && attachment["mime"].Str() == "image/png" && progressed == png.size(), "small PNG upload accepted with progress");
    a.chat.Send(dm, "", {attachment["id"].Str()});
    Await([&] { auto t = b.Timeline(dm); return t.messages.size() == 3 && t.messages.back().attachments.size() == 1; }, "uploaded PNG reaches peer as message attachment");
    auto bytes = b.chat.Image(b.net, attachment["url"].Str());
    Check(bytes && bytes->size() == png.size() && std::equal(bytes->begin(), bytes->end(), png.begin(), [](char x, unsigned char y) { return static_cast<unsigned char>(x) == y; }), "peer downloads exact uploaded PNG bytes");
    Check(b.chat.Image(b.net, attachment["url"].Str()) == bytes, "image cache reuses shared byte allocation");
    cancel = true; auto canceled = a.chat.Upload(a.net, dm, imagePath, {}, cancel);
    Check(canceled.Json()["error"].Str() == "canceled", "pre-canceled upload never starts");
    result["upload"] = true;
    auto channelBody = json::Value::Object(); channelBody["name"] = "w5-history"; channelBody["kind"] = "text";
    auto created = a.net.Post("/api/channels", channelBody); Check(created.status == 201, "create isolated history channel");
    auto history = created.Json()["id"].Str(); a.chat.Open(history); b.chat.Open(history);
    result["stage"] = "history_500"; write();
    int64_t first = 0, last = 0;
    for (int index = 0; index < 500; ++index) {
      auto& sender = index % 2 ? b : a;
      // Respect the production handler's per-user quota. A rate-limit failure
      // is retained by the layer, then retried after its one-minute window.
      for (;;) {
        sender.chat.Draft(history, "history fixture"); sender.chat.Send(history, "history fixture");
        auto t = sender.Timeline(history);
        if (t.error.empty()) { Check(!t.messages.empty(), "history send acknowledged"); last = t.messages.back().id; break; }
        Check(t.error == "Você mandou mensagens rápido demais. Espere um minuto.", "history send fails only for expected quota");
        std::puts("waiting for local message quota (61 seconds)");
        for (int second = 0; second < 61; ++second) Sleep(1000);
      }
      if (!index) first = last;
      if ((index+1) % 50 == 0) { result["sent"] = index+1; write(); std::printf("history sent: %d/500\n", index+1); }
    }
    // A fresh layer proves paging from disk, with none of the 500 live rows.
    app::Chat reader([&](const auto& method, const auto& path, const auto* body) { return app::ChatRequest(b.net, method, path, body); }, {}, fixture.root()/L"reader.json");
    reader.Resync(); reader.Open(history);
    size_t pageCount = 1, total = 0; bool more = true; int64_t oldest = 0, newest = 0;
    while (more) {
      reader.Inspect([&](const auto& s) { const auto& t = s.timelines.at(history); total = t.messages.size();
        more = t.hasMore; oldest = t.messages.empty() ? 0 : t.messages.front().id; newest = t.messages.empty() ? 0 : t.messages.back().id; });
      if (!more) break;
      Check(pageCount < 20, "history paging makes bounded progress"); reader.Older(history); ++pageCount;
    }
    Check(total == 500 && oldest == first && newest == last && pageCount >= 10, "five hundred messages page back to exact first id");
    result["history_count"] = int(total); result["history_pages"] = int(pageCount);
    result["passed"] = true; result["stage"] = "complete"; write();
  } catch (...) { write(); throw; }
}
}
int main(int argc, char** argv) {
  try {
    auto root = std::filesystem::absolute(L"test-output/chat-tests");
    if (argc > 1 && std::string(argv[1]) == "--self-test") {
      auto report = std::filesystem::absolute(argc > 2 ? std::filesystem::path(net::Wide(argv[2])) : std::filesystem::path(L"test-output/chat-self-test.json"));
      std::filesystem::create_directories(report.parent_path()); Integration(report);
    } else UnitTests(root);
    std::printf("PASS: %d chat assertions\n", checks); return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s (%d assertions)\n", error.what(), checks); return 1; }
}
