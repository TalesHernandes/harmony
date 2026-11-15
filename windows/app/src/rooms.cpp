#include "rooms.h"
#include <algorithm>
#include <cctype>

namespace app {
std::string Rooms::StateOf(const std::string& username) const {
  for (const auto& room : rooms) for (const auto& person : room.people)
    if (person.username == username) return "in_call";
  auto at = presence.find(username);
  return at == presence.end() ? "offline" : at->second;
}
namespace {
// The owner asked for the people in a call in alphabetical order (25/10/2026),
// by the name shown, ignoring case and the Portuguese accents (Ágata with the
// a's, not after z): one order for the sidebar and the call's tiles.
std::string SortKey(const Person& p) {
  const auto& name = p.name.empty() ? p.username : p.name;
  // U+00C0..U+00FF, upper and lower halves alike; '~' keeps ×/÷ out of the way.
  static const char* latin = "aaaaaaaceeeeiiiidnooooo~ouuuuyts";
  std::string key;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (c == 0xC3 && i + 1 < name.size()) { unsigned char n = static_cast<unsigned char>(name[++i]); key += latin[(n - 0x80) & 0x1F]; }
    else key += char(c < 0x80 ? std::tolower(c) : c);
  }
  return key;
}
bool ReadPeople(const json::Value& value, std::vector<Person>* out) {
  if (!value.IsArray()) return false;
  std::vector<Person> people;
  for (const auto& item : value.Elements()) {
    if (!item.IsObject() || item["username"].Str().empty()) return false;
    bool screen=false,camera=false;
    for(auto& source:item["publishing"].Elements()) {screen|=source.Str()=="screen"; camera|=source.Str()=="cam";}
    people.push_back({item["username"].Str(), item["display_name"].Str(),
                      item["muted"].Bool(), item["deafened"].Bool(),screen,camera,item["avatar"].Str()});
  }
  std::stable_sort(people.begin(), people.end(), [](const Person& a, const Person& b) {
    auto x = SortKey(a), y = SortKey(b); return x != y ? x < y : a.username < b.username;
  });
  *out = std::move(people);
  return true;
}
}
bool Rooms::Snapshot(const json::Value& channels, const json::Value& occupants) {
  if (!channels["channels"].IsArray() || !occupants["rooms"].IsObject()) return false;
  std::vector<Room> next;
  for (const auto& channel : channels["channels"].Elements()) {
    if (channel["kind"].Str() != "voice") continue;
    Room room{channel["id"].Str(), channel["name"].Str(), {}};
    if (room.id.empty()) return false;
    if (occupants["rooms"].Has(room.id) && !ReadPeople(occupants["rooms"][room.id], &room.people)) return false;
    next.push_back(std::move(room));
  }
  rooms = std::move(next);
  live = true;
  return true;
}
bool Rooms::Event(const std::string& type, const json::Value& data) {
  if (type == "voice_room") {
    const auto& id = data["channel_id"].Str();
    for (auto& room : rooms) if (room.id == id) return ReadPeople(data["peers"], &room.people);
  } else if (type == "presence") {
    const auto& user = data["username"].Str();
    const auto& state = data["state"].Str();
    if (user.empty()) return false;
    if (state == "online" || state == "away") presence[user] = state;
    else presence.erase(user);
    // Offline presence does not remove a voice peer: the SFU owns that fact.
    return true;
  } else if (type == "channel_removed") {
    auto count = rooms.size();
    std::erase_if(rooms, [&](const Room& room) { return room.id == data["id"].Str(); });
    return rooms.size() != count;
  } else if (type == "channel") {
    if (data["kind"].Str() != "voice" || data["id"].Str().empty()) return false;
    for (auto& room : rooms) if (room.id == data["id"].Str()) { room.name = data["name"].Str(); return true; }
    rooms.push_back({data["id"].Str(), data["name"].Str(), {}});
    return true;
  }
  return false;
}
}
