// sound-rules.h, on a clock the test turns by hand: the same cases as
// Android's SoundRulesTest.
#include "../src/sound-rules.h"
#include <cstdio>
#include <memory>

using namespace sounds;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

struct Bench {
  int64_t clock = 10000;
  std::vector<Sound> heard;
  struct Timer { int64_t at; std::function<void()> block; std::shared_ptr<bool> live; };
  std::vector<Timer> timers;
  Rules rules{[this](Sound s) { heard.push_back(s); }, [this] { return clock; },
      [this](int64_t ms, std::function<void()> block) -> std::function<void()> {
        auto live = std::make_shared<bool>(true);
        timers.push_back({clock + ms, std::move(block), live});
        return [live] { *live = false; };
      }};
  void Pass(int64_t ms) {
    clock += ms;
    auto due = timers; timers.clear();
    for (auto& t : due) if (t.at > clock) timers.push_back(t); else if (*t.live) t.block();
  }
  void Room(std::vector<RoomPerson> people, bool quiet = false) {
    people.push_back({"eu"});
    rules.Room("sala", people, "eu", quiet);
    Pass(200); // past the 150 ms that makes two equal sounds one
  }
  Own Voice(bool live, bool reconnecting = false, bool muted = false, bool deaf = false, bool screen = false) { return Own{"sala", live, reconnecting, muted, deaf, screen}; }
};

int main() {
  {  // joining and leaving are heard once
    Bench b; b.rules.Voice(Own{}); b.rules.Voice(b.Voice(false));
    CHECK((b.heard == std::vector<Sound>{Sound::Entrar})); // at the click
    b.Room({{"ana"}}, true);
    b.rules.Voice(b.Voice(true));
    b.Room({{"ana"}}); b.Room({{"ana"}, {"beto"}}); b.Room({{"beto"}});
    b.rules.Voice(Own{});
    CHECK((b.heard == std::vector<Sound>{Sound::Entrar, Sound::Entrar, Sound::Sair, Sound::Sair}));
  }
  {  // mute and deafen: deafening mutes too, and is one sound
    Bench b; b.rules.Voice(Own{}); b.rules.Voice(b.Voice(true)); b.Pass(200);
    b.rules.Voice(b.Voice(true, false, true)); b.Pass(200);
    b.rules.Voice(b.Voice(true)); b.Pass(200);
    b.rules.Voice(b.Voice(true, false, true, true)); b.Pass(200);
    b.rules.Voice(b.Voice(true)); b.Pass(200);
    CHECK((b.heard == std::vector<Sound>{Sound::Entrar, Sound::Mutar, Sound::Desmutar, Sound::Mutar, Sound::Desmutar}));
  }
  {  // a blink of the connection is silent, a long drop is heard
    Bench b; b.rules.Voice(Own{}); b.rules.Voice(b.Voice(true)); b.heard.clear();
    b.rules.Voice(b.Voice(false, true)); b.Pass(500); b.rules.Voice(b.Voice(true)); b.Pass(2000);
    CHECK(b.heard.empty());
    b.rules.Voice(b.Voice(false, true)); b.Pass(1300); b.rules.Voice(b.Voice(true));
    CHECK((b.heard == std::vector<Sound>{Sound::Sair, Sound::Entrar}));
  }
  {  // a screen that blinks starts once; its end is heard by the whole room
    Bench b; b.rules.Voice(Own{}); b.rules.Voice(b.Voice(true)); b.Room({{"ana"}}); b.heard.clear();
    b.Room({{"ana", true}}); b.Room({{"ana"}}); b.Room({{"ana", true}});
    CHECK((b.heard == std::vector<Sound>{Sound::TelaComecou}));
    b.Room({{"ana"}}); b.Pass(1500);
    CHECK((b.heard == std::vector<Sound>{Sound::TelaComecou, Sound::TelaAcabou}));
    b.Room({{"ana", true}}); b.Room({}); b.Pass(1500); // left sharing: a leave
    CHECK((b.heard == std::vector<Sound>{Sound::TelaComecou, Sound::TelaAcabou, Sound::TelaComecou, Sound::Sair}));
  }
  {  // one's own screen is heard by who shares it; leaving with it on is one goodbye
    Bench b; b.rules.Voice(Own{}); b.rules.Voice(b.Voice(true)); b.Pass(200);
    b.rules.Voice(b.Voice(true, false, false, false, true)); b.Pass(200);
    b.rules.Voice(b.Voice(true)); b.Pass(200);
    b.rules.Voice(b.Voice(true, false, false, false, true)); b.Pass(200);
    b.rules.Voice(Own{});
    CHECK((b.heard == std::vector<Sound>{Sound::Entrar, Sound::TelaComecou, Sound::TelaAcabou, Sound::TelaComecou, Sound::Sair}));
  }
  {  // the direct call: ringback, answered, muted, ended
    Bench b; b.rules.Direct(Call{}, false);
    b.rules.Direct(Call{CallStep::Calling}, false); CHECK(b.rules.Ringback()); CHECK(!b.rules.Ring());
    b.rules.Direct(Call{CallStep::Connecting}, false); CHECK(!b.rules.Ringback());
    b.rules.Direct(Call{CallStep::Live, true}, false); b.Pass(200);
    b.rules.Direct(Call{CallStep::Live, true, true}, false); b.Pass(200);
    b.rules.Direct(Call{}, false);
    CHECK((b.heard == std::vector<Sound>{Sound::Entrar, Sound::Mutar, Sound::Sair}));
  }
  {  // refused is heard; putting one's own ring down is not; rung here, only the ring
    Bench b; b.rules.Direct(Call{}, false);
    b.rules.Direct(Call{CallStep::Calling}, false); b.rules.Direct(Call{}, false);
    CHECK((b.heard == std::vector<Sound>{Sound::TelaAcabou}));
    b.Pass(200);
    b.rules.Direct(Call{CallStep::Calling}, false); b.rules.Direct(Call{}, true);
    b.rules.Direct(Call{CallStep::Ringing}, false); CHECK(b.rules.Ring()); b.rules.Direct(Call{}, false);
    CHECK((b.heard == std::vector<Sound>{Sound::TelaAcabou}));
  }
  std::printf(failures ? "sound rules: %d FAILED\n" : "sound rules: ok\n", failures);
  return failures ? 1 : 0;
}
