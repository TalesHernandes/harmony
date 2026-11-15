#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
namespace audio {
// AudioTransport returns a count of interleaved samples, not per-channel
// frames. Never read past the output buffer or submit a 20 ms stereo block.
inline size_t RenderedFrames(size_t requested,size_t channels,size_t returnedSamples) {
  return channels ? std::min(requested,returnedSamples/channels) : 0;
}
// room is the already-scaled speaker mix; external is reference-only.
inline void MixEchoReference(const int16_t* room, const int16_t* external, int16_t* reference, size_t samples) {
  for(size_t i=0;i<samples;++i) reference[i]=int16_t(std::clamp(int(room[i])+external[i],-32768,32767));
}
}
