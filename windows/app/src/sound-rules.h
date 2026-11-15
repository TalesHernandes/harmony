#pragma once
// When each of the harmony's own sounds is heard (sons/LISTA.md, chosen by the
// owner on 29/10/2026). The same rules as the web's sounds.ts and Android's
// SoundRules.kt, kept apart from the player so they can be tested without a
// sound card. The clock and the timers come from outside.
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sounds {
enum class Sound { Entrar, Sair, Mutar, Desmutar, TelaComecou, TelaAcabou };

struct RoomPerson { std::string username; bool screen = false; };

// What this device is doing in a room; live is connected and not recovering.
struct Own { std::string room; bool live = false, reconnecting = false, muted = false, deafened = false, screen = false; };

// The direct call, as the sounds see it.
enum class CallStep { Idle, Calling, Ringing, Connecting, Live };
struct Call { CallStep step = CallStep::Idle; bool connected = false, muted = false, deafened = false; };

class Rules {
 public:
  static constexpr int64_t kScreenGoneMs = 1000, kScreenEndMs = 1000, kLostAfterMs = 1200;
  using Later = std::function<std::function<void()>(int64_t, std::function<void()>)>;
  Rules(std::function<void(Sound)> play, std::function<int64_t()> now, Later later)
      : play_(std::move(play)), now_(std::move(now)), later_(std::move(later)) {}

  // Every list of who is in this device's room. While not live (quiet) it
  // only sets the baseline, and so does the first list of a room: everybody
  // already there is not arriving. By username: a blink that comes back with
  // a new session is not a leave and a join.
  void Room(std::string const& room, std::vector<RoomPerson> const& people, std::string const& me, bool quiet) {
    Seen now{room, {}, {}};
    for (auto& p : people) if (p.username != me) { now.people.insert(p.username); if (p.screen) now.screens.insert(p.username); }
    bool had = hasSeen_; Seen before = seen_;
    seen_ = now; hasSeen_ = true;
    if (quiet || !had || before.room != room) return;
    for (auto& u : now.people) if (!before.people.count(u)) { Sound_(Sound::Entrar); break; }
    for (auto& u : before.people) if (!now.people.count(u)) { Sound_(Sound::Sair); break; }
    auto t = now_(); bool started = false;
    for (auto& u : now.screens) {
      if (before.screens.count(u)) continue;
      if (auto it = ending_.find(u); it != ending_.end()) { it->second(); ending_.erase(it); }
      // A screen flag blinks while sharing is set up or torn down.
      auto gone = goneAt_.find(u);
      if (gone == goneAt_.end() || t - gone->second >= kScreenGoneMs) started = true;
    }
    if (started) Sound_(Sound::TelaComecou);
    for (auto& u : before.screens) {
      if (now.screens.count(u)) continue;
      goneAt_[u] = t;
      // Heard by the whole room, like its start (owner, 29/10/2026); a
      // person who left with the screen on is heard leaving instead.
      if (!now.people.count(u)) continue;
      if (auto it = ending_.find(u); it != ending_.end()) it->second();
      ending_[u] = later_(kScreenEndMs, [this, u, room] {
        ending_.erase(u);
        if (hasSeen_ && seen_.room == room && !seen_.screens.count(u)) Sound_(Sound::TelaAcabou);
      });
    }
  }

  // This device in its room: joining, leaving, a drop and a return, mute,
  // deafen and its own screen. The join is heard at the click, not when the
  // connection is ready (owner, 29/10/2026); the others hear it when this
  // device shows up in their list.
  void Voice(Own const& s) {
    bool had = hasOwn_; Own before = own_;
    own_ = s; hasOwn_ = true;
    if (!had) return;
    bool in = !s.room.empty(), wasIn = !before.room.empty();
    if (in && !wasIn) { recovering_ = lostHeard_ = false; Sound_(Sound::Entrar); }
    if (s.live && !before.live && in && wasIn) {
      CancelLost();
      // Back after a drop long enough to have been heard.
      if (recovering_ && lostHeard_) Sound_(Sound::Entrar);
      recovering_ = lostHeard_ = false;
    }
    if (in && s.reconnecting && before.live) {
      recovering_ = true; CancelLost();
      lost_ = later_(kLostAfterMs, [this] { lost_ = nullptr; if (!own_.live) { lostHeard_ = true; Sound_(Sound::Sair); } });
    }
    if (!in && wasIn) {
      CancelLost();
      // Leaving after the drop was already heard is not a second goodbye.
      if (!(recovering_ && lostHeard_)) Sound_(Sound::Sair);
      recovering_ = lostHeard_ = false;
      Forget();
    }
    // Its own screen, heard by whoever shares it too (owner, 29/10/2026).
    if (in && wasIn && s.screen != before.screen) Sound_(s.screen ? Sound::TelaComecou : Sound::TelaAcabou);
    if (s.live && before.live) Toggles(before.muted, before.deafened, s.muted, s.deafened);
  }

  // The direct call. [ownHangup] says this device put it down: no sound for that
  // while it was only ringing.
  void Direct(Call const& s, bool ownHangup) {
    bool had = hasCall_; Call before = call_;
    call_ = s; hasCall_ = true;
    if (!had) return;
    if (s.connected && !before.connected && s.step == CallStep::Live) Sound_(Sound::Entrar);
    if (s.step == CallStep::Idle && before.step != CallStep::Idle) {
      if (before.connected) Sound_(Sound::Sair);
      // Refused or nobody answered, as the caller hears it.
      else if (before.step == CallStep::Calling && !ownHangup) Sound_(Sound::TelaAcabou);
    }
    if (s.connected && before.connected) Toggles(before.muted, before.deafened, s.muted, s.deafened);
  }
  // The ring the caller hears while it asks (the person called hears the full one).
  bool Ringback() const { return hasCall_ && call_.step == CallStep::Calling; }
  bool Ring() const { return hasCall_ && call_.step == CallStep::Ringing; }

 private:
  struct Seen { std::string room; std::set<std::string> people, screens; };
  void Sound_(Sound s) {
    // The same sound twice inside 150 ms is one: three people arriving with
    // one room update is a chord, not a burst of identical pops.
    auto t = now_(); auto it = last_.find(s);
    if (it != last_.end() && t - it->second < 150) return;
    last_[s] = t; play_(s);
  }
  void Toggles(bool wasMuted, bool wasDeaf, bool muted, bool deaf) {
    if (deaf != wasDeaf) Sound_(deaf ? Sound::Mutar : Sound::Desmutar);
    else if (muted != wasMuted) Sound_(muted ? Sound::Mutar : Sound::Desmutar);
  }
  void CancelLost() { if (lost_) { lost_(); lost_ = nullptr; } }
  void Forget() {
    hasSeen_ = false; seen_ = {};
    for (auto& [u, cancel] : ending_) cancel();
    ending_.clear(); goneAt_.clear();
  }
  std::function<void(Sound)> play_;
  std::function<int64_t()> now_;
  Later later_;
  std::map<Sound, int64_t> last_;
  Seen seen_; bool hasSeen_ = false;
  std::map<std::string, int64_t> goneAt_;
  std::map<std::string, std::function<void()>> ending_;
  Own own_; bool hasOwn_ = false;
  std::function<void()> lost_;
  bool recovering_ = false, lostHeard_ = false;
  Call call_; bool hasCall_ = false;
};
}  // namespace sounds
