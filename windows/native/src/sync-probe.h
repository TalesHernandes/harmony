// Opt-in bench events. One bounded queue shared by capture, audio and selftest.
#pragma once
#include "json.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
namespace sync_probe {
inline bool Enabled() {
  static const bool enabled=[] {const char* v=std::getenv("HARMONY_SYNC_PROBE");return v && std::strcmp(v,"1")==0;}();
  return enabled;
}
inline double WallMs() {
  FILETIME f;GetSystemTimePreciseAsFileTime(&f);
  return double(((uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime)-116444736000000000ULL)/10000.0;
}
inline double QpcMs() {
  LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);
  return double(t.QuadPart)*1000.0/double(f.QuadPart);
}
struct Queue {std::mutex mu;std::vector<json::Value> events;int64_t lost=0;};
inline Queue& Events() {static Queue q;return q;}
inline void Record(json::Value event) {
  auto& q=Events();std::lock_guard lock(q.mu);
  if(q.events.size()>=4096) {++q.lost;return;}q.events.push_back(std::move(event));
}
inline json::Value Drain() {
  auto value=json::Value::Object();auto list=json::Value::Array();
  auto& q=Events();std::lock_guard lock(q.mu);
  for(auto& e:q.events)list.Push(std::move(e));q.events.clear();
  value["events"]=std::move(list);value["lost"]=q.lost;q.lost=0;
  value["enabled"]=Enabled();return value;
}
inline json::Value Event(const char* kind) {
  auto e=json::Value::Object();e["kind"]=kind;e["wall_ms"]=WallMs();e["qpc_ms"]=QpcMs();return e;
}
} // namespace sync_probe
