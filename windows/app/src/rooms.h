#pragma once
#include "../../native/src/json.h"
#include <map>
#include <string>
#include <vector>

namespace app {
struct Person { std::string username, name; bool muted = false, deafened = false, screen=false, camera=false; std::string avatar; };
struct Room { std::string id, name; std::vector<Person> people; };
// Only public voice fields are retained; message bodies, session IDs and
// own-user e-mail fields never enter this model.
class Rooms {
 public:
  std::vector<Room> rooms;
  std::map<std::string, std::string> presence;
  bool live = false;
  bool Snapshot(const json::Value& channels, const json::Value& occupants);
  bool Event(const std::string& type, const json::Value& data);
  std::string StateOf(const std::string& username) const;
  void Disconnect() { live = false; } // Keep the last known list, like the web.
};
}
