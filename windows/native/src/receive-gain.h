#pragma once
#include <map>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace audio {
// The slider reads 0-200%. Up to 100 it is the plain fraction; above, the
// step is in decibels, up to +12 dB (4x) at 200: a straight 2x is only +6 dB,
// which the owner heard as "barely louder" (25/10/2026).
inline double LoudnessGain(double slider) {
  return slider <= 1 ? (std::max)(0.0, slider) : std::pow(10.0, ((std::min)(slider, 2.0) - 1) * 12 / 20);
}
inline double ReceiveGain(const std::map<std::string,double>& volumes, const std::string& person, bool deafened) {
  auto gain=volumes.find(person);
  return deafened ? 0 : gain==volumes.end() ? 1 : LoudnessGain(gain->second);
}
// A boosted voice would clip into a crackle at the top; above 3/4 of full
// scale it is rounded off instead (tanh knee). At or below unity the samples
// pass exactly as before.
inline void ScaleReceivePcm(int16_t* samples, size_t count, double gain) {
  if (gain <= 1) { for(size_t i=0;i<count;++i) samples[i]=int16_t(std::clamp(std::lround(samples[i]*gain),-32768L,32767L)); return; }
  constexpr double knee = 0.75;
  for(size_t i=0;i<count;++i) {
    double x = samples[i] * gain / 32768.0, a = std::abs(x);
    if (a > knee) a = knee + (1 - knee) * std::tanh((a - knee) / (1 - knee));
    samples[i] = int16_t(std::clamp(std::lround(std::copysign(a, x) * 32767.0), -32768L, 32767L));
  }
}
}
