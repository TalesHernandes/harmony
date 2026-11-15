#pragma once
#include <cstdint>
namespace gpu {
// WGC supplies compositor QPC time, not the time we dequeue its texture.
// Anchor once so queueing or encode work cannot stretch the media timeline.
struct CaptureClock {
  int64_t offset_ns;
  int64_t Timestamp(int64_t compositor_ns) const { return compositor_ns + offset_ns; }
};

// Opt-in timestamp experiment. Keep a rational grid anchored to real capture
// time: adding a rounded 16667-us period would drift 72 ms in one hour.
// Crowded slots retain their real time instead of borrowing future frame slots.
// The 1-ms floor also avoids identical millisecond timestamps downstream.
class RegularCaptureClock {
 public:
  explicit RegularCaptureClock(int fps) : fps_(fps > 0 ? fps : 1) {}
  int64_t Timestamp(int64_t capture_us) {
    if (!started_) {
      started_ = true;
      origin_us_ = last_us_ = capture_us;
      return capture_us;
    }
    const int64_t elapsed = capture_us > origin_us_ ? capture_us - origin_us_ : 0;
    const int64_t slot = elapsed / 1000000 * fps_
        + ((elapsed % 1000000) * fps_ + 500000) / 1000000;
    int64_t stamped = origin_us_ + slot / fps_ * 1000000
        + ((slot % fps_) * 1000000 + fps_ / 2) / fps_;
    if (stamped < last_us_ + 1000) {
      ++crowded_;
      stamped = capture_us > last_us_ + 1000 ? capture_us : last_us_ + 1000;
    }
    last_us_ = stamped;
    return stamped;
  }
  int64_t crowded() const { return crowded_; }
 private:
  int fps_;
  bool started_ = false;
  int64_t origin_us_ = 0, last_us_ = 0, crowded_ = 0;
};

// Absolute deadlines without catch-up bursts. A late drawing skips elapsed
// slots rather than drawing obsolete frames back-to-back into the compositor.
inline bool GameDue(int64_t now, int64_t frequency, int64_t start, uint64_t& slot) {
  if (now < start + int64_t((slot * frequency + 59) / 60)) return false;
  slot = uint64_t((now - start) * 60 / frequency) + 1;
  return true;
}
// One clock chooses capture frames. The highest encoder layer takes all of
// them; lower layers have independent deadlines and never retime the top.
inline bool CaptureDue(int64_t now, int fps, int64_t& next) {
  const int64_t period = 1000000LL / fps;
  if (next && now < next - period / 2) return false;
  next = (next && next > now - period ? next : now) + period;
  return true;
}
inline bool LayerDue(int64_t now, int fps, int capture_fps, int64_t& next) {
  if (fps >= capture_fps) return true;
  const int64_t period = 1000000LL / fps;
  if (next && now + 250000LL / fps < next) return false;
  if (!next) next = now;
  do { next += period; } while (next <= now);
  return true;
}
inline int ResolutionRank(int width, int height, int largest_area) {
  const int area = width * height;
  return area == largest_area ? 0 : area * 4 >= largest_area ? 1 : 2;
}
}
