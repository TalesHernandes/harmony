package org.webrtc.audio;

import android.media.AudioDeviceInfo;
import android.media.AudioTimestamp;
import android.media.AudioTrack;

/** Public routing and timestamp telemetry, and media or call playback. */
public final class PlaybackRoute {
  public static final class Output {
    public final int deviceType;
    public final int latencyMs;
    public final long measuredAtNs;
    Output(int type, int latency, long at) {
      deviceType = type; latencyMs = latency; measuredAtNs = at;
    }
  }
  private static volatile Output output = new Output(0, -1, 0);
  public static Output output() {
    Output value = output;
    return System.nanoTime() - value.measuredAtNs <= 12000000000L
        ? value : new Output(0, -1, 0);
  }
  public static int outputLatencyMs() { return output().latencyMs; }
  public static int routedDeviceType() { return output().deviceType; }

  // Owned by the playback thread. No AudioTrack escapes to the app thread.
  static final class Meter {
    private final AudioTimestamp timestamp = new AudioTimestamp();
    private long writtenBytes, nextPoll, nextRouteCheck, previousFrame = -1, previousTime;
    private double previousDelay = -1;
    private int routeId = -1, routeType, stable, underruns = -1;

    void clear() {
      stable = 0; previousFrame = -1; previousDelay = -1;
      output = new Output(routeType, -1, System.nanoTime());
    }

    void written(AudioTrack track, int bytes) {
      long now = System.nanoTime();
      if (bytes > 0) writtenBytes += bytes;
      if (bytes < 0) { clear(); return; }
      // Count every successful write, but keep platform queries off the 10 ms cadence.
      if (now < nextRouteCheck) return;
      nextRouteCheck = now + 250000000L;
      AudioDeviceInfo device = track.getRoutedDevice();
      int id = device == null ? -1 : device.getId();
      int type = device == null ? 0 : device.getType();
      int count = track.getUnderrunCount();
      if (id != routeId || type != routeType || count != underruns) {
        routeId = id; routeType = type; underruns = count;
        clear(); nextPoll = now + 250000000L;
      }
      if (device == null || track.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) {
        clear(); return;
      }
      if (now < nextPoll) return;
      nextPoll = now + 250000000L;
      if (!track.getTimestamp(timestamp)) { clear(); return; }
      long frames = writtenBytes / (2L * track.getChannelCount()); // PCM16.
      // AudioTimestamp's frame position wraps at 32 bits; writtenBytes does not.
      long frame = (frames & ~0xffffffffL) | (timestamp.framePosition & 0xffffffffL);
      if (frame > frames) frame -= 0x100000000L;
      long time = timestamp.nanoTime;
      int rate = track.getSampleRate();
      double delay = (frames - frame) * 1000.0 / rate + (time - now) / 1000000.0;
      if (frame < 0 || Math.abs(time - now) > 2000000000L || delay < 0 || delay > 2000) {
        clear(); return;
      }
      boolean advancing = previousFrame >= 0 && frame > previousFrame && time > previousTime;
      double speed = advancing ? (frame - previousFrame) * 1000000000.0 / (time - previousTime) / rate : 0;
      boolean consistent = advancing && Math.abs(speed - 1) <= 0.02
          && Math.abs(delay - previousDelay) <= 20;
      stable = consistent ? stable + 1 : 0;
      previousFrame = frame; previousTime = time; previousDelay = delay;
      output = new Output(routeType, stable >= 3 ? (int) Math.round(delay) : -1, now);
      if (stable >= 3) nextPoll = now + 10000000000L;
    }
  }
  static final class Request {
    final AudioDeviceInfo device;
    Request(AudioDeviceInfo device) { this.device = device; }
  }
  static volatile Request request;
  // Call mode: the next AudioTrack plays as a voice call (the phone's own echo
  // canceller then hears it, and every other app). Read at initPlayout only;
  // the app restarts playout to apply it.
  static volatile boolean voice;
  private PlaybackRoute() {}
  public static void setVoice(boolean on) { voice = on; }
  public static void setPreferredDevice(AudioDeviceInfo device) {
    request = new Request(device);
  }
}
