#pragma once
#include <windows.h>
#include <psapi.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#pragma comment(lib,"psapi.lib")
namespace resources {
inline uint64_t PrivateBytes() {PROCESS_MEMORY_COUNTERS_EX m{};return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof m)?m.PrivateUsage:0;}
enum Kind {Voice,Dfn,Camera,KindCount};
struct Part {unsigned loaded=0;uint64_t estimate=0;bool measured=false;};
struct Shared {
  std::mutex mutex;std::array<Part,KindCount> parts{};
  HANDLE cameraProcess=nullptr;
  uint64_t encodedCount=0,encodedBytes=0,decodedCount=0,decodedBytes=0,emojiCount=0,emojiBytes=0,avatarCount=0,avatarBytes=0;
  std::vector<std::pair<size_t,uint64_t>> channels;
  std::vector<std::string> channelNames;
};
inline Shared& State(){static Shared state;return state;}
// Deltas are process-private observations during loading, not heap ownership.
// Keep the last measured delta but report zero attributed bytes after teardown.
class Load {
  Kind kind_;uint64_t before_=PrivateBytes();bool active_=false;
 public:
  explicit Load(Kind kind):kind_(kind){}
  void Loaded(){if(active_)return;uint64_t after=PrivateBytes();auto& s=State();std::lock_guard lock(s.mutex);auto& p=s.parts[kind_];++p.loaded;p.estimate=after>before_?after-before_:0;p.measured=true;active_=true;}
  ~Load(){if(active_){auto& s=State();std::lock_guard lock(s.mutex);--s.parts[kind_].loaded;}}
};
struct Snapshot {
  std::array<Part,KindCount> parts{};uint64_t cameraPrivate=0;
  uint64_t encodedCount=0,encodedBytes=0,decodedCount=0,decodedBytes=0,emojiCount=0,emojiBytes=0,avatarCount=0,avatarBytes=0;
  std::vector<std::pair<size_t,uint64_t>> channels;
  std::vector<std::string> channelNames;
};
inline Snapshot Read(){auto& s=State();std::lock_guard lock(s.mutex);Snapshot out;
  out.parts=s.parts;out.encodedCount=s.encodedCount;out.encodedBytes=s.encodedBytes;out.decodedCount=s.decodedCount;out.decodedBytes=s.decodedBytes;
  out.emojiCount=s.emojiCount;out.emojiBytes=s.emojiBytes;out.avatarCount=s.avatarCount;out.avatarBytes=s.avatarBytes;out.channels=s.channels;out.channelNames=s.channelNames;
  if(s.cameraProcess){PROCESS_MEMORY_COUNTERS_EX m{};if(GetProcessMemoryInfo(s.cameraProcess,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof m))out.cameraPrivate=m.PrivateUsage;}
  return out;
}
inline void CameraProcess(HANDLE handle){auto& s=State();std::lock_guard lock(s.mutex);s.cameraProcess=handle;}
}
