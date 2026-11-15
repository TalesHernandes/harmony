#pragma once
#include "../../native/gpu/cadence.h"
#include <cstdio>
#include <cstdlib>

inline bool RegularTimestampTests() {
  bool failed = false;
  auto check = [&](bool ok, const char* name) {
    std::printf("regular timestamp %s: %s\n", name, ok ? "PASS" : "FAIL");
    failed |= !ok;
  };
  const int64_t origin = 987654321000LL;
  gpu::RegularCaptureClock clock(60);
  check(clock.Timestamp(origin) == origin, "first frame preserves clock origin");
  check(clock.Timestamp(origin + 12000) == origin + 16667, "early frame");
  check(clock.Timestamp(origin + 36000) == origin + 33333, "late frame");
  check(clock.Timestamp(origin + 100000) == origin + 100000, "skipped frames preserve elapsed time");
  check(clock.Timestamp(origin + 5010000) == origin + 5016667, "long pause skips grid slots");
  gpu::RegularCaptureClock crowded(60);
  crowded.Timestamp(origin);
  const auto before = crowded.Timestamp(origin + 12000);
  const auto duplicate = crowded.Timestamp(origin + 17000);
  check(duplicate >= before + 1000 && duplicate < origin + 25000 && crowded.crowded() == 1,
        "crowded slot does not borrow a full future period");
  check(crowded.Timestamp(origin + 34000) == origin + 33333,
        "crowded slot does not leave timing debt");
  gpu::RegularCaptureClock backwards(60);
  backwards.Timestamp(origin);
  backwards.Timestamp(origin + 50000);
  check(backwards.Timestamp(origin + 40000) == origin + 51000,
        "out of order input remains monotonic");
  check(backwards.Timestamp(origin + 67000) == origin + 66667,
        "out of order input recovers absolute grid");
  for (int fps : {30, 60}) {
    gpu::RegularCaptureClock hour(fps), drift(fps);
    int64_t previous = origin - 1000, last = origin, max_error = 0;
    bool monotonic = true, bounded = true, grid = true;
    for (int64_t i = 0; i <= int64_t(fps) * 3600; ++i) {
      const int64_t ideal = (i * 1000000 + fps / 2) / fps;
      const int64_t jitter = i == 0 || i == int64_t(fps)*3600 ? 0 : (i % 2 ? 4000 : -4000);
      last = hour.Timestamp(origin + ideal + jitter);
      grid &= last == origin + ideal;
      const int64_t real = origin + ideal + ideal / 10000; // 100 ppm clock/rate mismatch.
      const int64_t adjusted = drift.Timestamp(real);
      const int64_t error = std::llabs(adjusted - real);
      if (error > max_error) max_error = error;
      monotonic &= adjusted >= previous + 1000;
      bounded &= error <= (1000000 + 2 * fps - 1) / (2 * fps);
      previous = adjusted;
    }
    char name[100];
    std::snprintf(name, sizeof(name), "%d fps one hour no accumulated rounding drift", fps);
    check(grid && last == origin + 3600000000LL, name);
    std::snprintf(name, sizeof(name), "%d fps one hour 100 ppm mismatch bounded (max %lld us)",
                  fps, static_cast<long long>(max_error));
    check(monotonic && bounded, name);
  }
  return failed;
}
