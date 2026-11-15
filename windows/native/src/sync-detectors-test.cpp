#include "sync-detectors.h"
#include <array>
#include <cassert>
#include <cstdio>
int main() {
  sync_probe::FlashDetector flash;
  std::array<uint8_t,32> dark{},white{};white.fill(255);
  assert(!flash.Push(dark.data(),16));
  for(int frame=1;frame<=8;++frame)
    assert(flash.Push(frame>=4?white.data():dark.data(),16)==(frame==4));
  flash.Gap();assert(!flash.Push(white.data(),16));
  assert(!flash.Push(dark.data(),16));assert(flash.Push(white.data(),16));
  sync_probe::BeepDetector beep;
  std::array<int16_t,960> silence{},signal{};
  assert(!beep.Push(silence.data(),480,2,48000));
  for(size_t i=240;i<480;++i) for(size_t c=0;c<2;++c)
    signal[i*2+c]=int16_t(0.15*32767*std::sin(double(i-240)*6.283185307179586*1000/48000));
  auto onset=beep.Push(signal.data(),480,2,48000);
  assert(onset && *onset==241); // First nonzero sample after the 5 ms onset.
  for(size_t i=0;i<480;++i)for(size_t c=0;c<2;++c)
    signal[i*2+c]=int16_t(0.15*32767*std::sin(double(i)*6.283185307179586*1000/48000));
  assert(!beep.Push(signal.data(),480,2,48000));
  assert(!beep.Push(silence.data(),480,2,48000));
  assert(beep.Push(signal.data(),480,2,48000));
  beep.Gap();assert(!beep.Push(signal.data(),480,2,48000));
  signal.fill(100);assert(!beep.Push(signal.data(),480,2,48000));
  puts("sync detectors: frame N, mid-block onset, sustained signal and gaps passed");
}
