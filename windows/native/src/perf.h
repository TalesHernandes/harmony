// The program measuring itself (PLANO-WINDOWS §2.6). Every stage owns one
// Stage and is the only thing that writes to it; the "Desempenho" panel, the
// diagnostic log and the bench only read Snapshot().
//
//   static perf::Stage& stage = perf::Get("encode.alta");
//   auto t = stage.Begin();      // a frame/block entered
//   ...work...
//   t.Done();                    // left; time recorded
//   stage.Drop();                // or: it was thrown away
//
// Counters that are not stages (e.g. "gpu->cpu copies", the proof that no
// frame passed through ordinary memory) use perf::Count("name").
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace perf {

class Stage {
 public:
  explicit Stage(std::string name) : name_(std::move(name)) {}
  Stage(const Stage&) = delete;
  Stage& operator=(const Stage&) = delete;

  class Timer {
   public:
    explicit Timer(Stage* s) : s_(s), t0_(Now()) { s_->in_.fetch_add(1, std::memory_order_relaxed); }
    Timer(Timer&& o) noexcept : s_(o.s_), t0_(o.t0_) { o.s_ = nullptr; }
    Timer(const Timer&) = delete;
    ~Timer() { if (s_) s_->dropped_.fetch_add(1, std::memory_order_relaxed); }
    // The unit left the stage normally.
    void Done() {
      if (!s_) return;
      s_->Finish(Now() - t0_);
      s_ = nullptr;
    }
    // The unit was thrown away (counts as entered and dropped).
    void Drop() {
      if (!s_) return;
      s_->dropped_.fetch_add(1, std::memory_order_relaxed);
      s_ = nullptr;
    }

   private:
    Stage* s_;
    int64_t t0_;
  };

  Timer Begin() { return Timer(this); }
  // For work timed elsewhere (e.g. GPU timestamps): one unit in and out.
  void Record(int64_t ns) {
    in_.fetch_add(1, std::memory_order_relaxed);
    Finish(ns);
  }
  void Drop() {
    in_.fetch_add(1, std::memory_order_relaxed);
    dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  // Free-form value the stage wants to show (bytes, bitrate, width...).
  void SetValue(int64_t v) { value_.store(v, std::memory_order_relaxed); }

  const std::string& name() const { return name_; }

  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

 private:
  friend struct Reader;
  void Finish(int64_t ns) {
    out_.fetch_add(1, std::memory_order_relaxed);
    busy_ns_.fetch_add(ns, std::memory_order_relaxed);
    int64_t m = max_ns_.load(std::memory_order_relaxed);
    while (ns > m && !max_ns_.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {
    }
  }

  std::string name_;
  std::atomic<uint64_t> in_{0}, out_{0}, dropped_{0};
  std::atomic<int64_t> busy_ns_{0}, max_ns_{0}, value_{0};
};

// Returns the stage with this name, creating it the first time. The reference
// stays valid for the life of the program; keep it in a static.
Stage& Get(const std::string& name);
// A plain counter is a stage that only ever records zero-time units.
inline void Count(const std::string& name, uint64_t n = 1) {
  Stage& s = Get(name);
  for (uint64_t i = 0; i < n; ++i) s.Record(0);
}

// Names the calling thread for Windows (SetThreadDescription), so per-thread
// CPU can be attributed to the stage that runs on it. Snapshot discovers
// live threads without keeping a duplicate handle for each name.
void NameThread(const std::wstring& name);

struct StageRow {
  std::string name;
  uint64_t in = 0, out = 0, dropped = 0;  // since the previous snapshot
  double avg_ms = 0, max_ms = 0;          // per unit, over the interval
  int64_t value = 0;
};
struct ThreadRow {
  std::string name;
  double cpu_percent = 0;  // of one core, over the interval
  double total_cpu_ms = 0; // grouped lifetime CPU, including retired threads
};
struct Snapshot {
  double seconds = 0;  // length of the interval
  std::vector<StageRow> stages;
  std::vector<ThreadRow> threads;
  double process_cpu_percent = 0;  // of one core
  uint64_t working_set_bytes = 0;
  uint64_t private_bytes = 0;
  uint32_t thread_count = 0,handles = 0,gdi_objects = 0,user_objects = 0;
  std::vector<ThreadRow> top_threads;
};

// Differences since the previous call. Only ONE reader may call this (the
// continuous 2 s collector) — two readers would each see half of every delta, the
// "two owners of one difference" bug from the web client.
Snapshot TakeSnapshot();
// One line per stage, for the diagnostic log and the bench.
std::string Format(const Snapshot& s);

}  // namespace perf
