#pragma once
#include <windows.h>
#include <mutex>
#include <cstdint>

namespace network {
struct Sample {
  uint64_t at=0,generation=0;
  double rtt=-1,jitter=-1,loss=-1,available=-1,send=-1,receive=-1;
  enum Route {Unknown,Direct,Turn} route=Unknown;
};
struct Store {
  std::mutex mutex;Sample sample;
  uint64_t Begin(){std::lock_guard lock(mutex);auto generation=sample.generation+1;sample={};sample.generation=generation;return generation;}
  void Set(Sample value){std::lock_guard lock(mutex);if(value.generation==sample.generation)sample=value;}
  Sample Read(){std::lock_guard lock(mutex);return sample;}
};
inline Store& Latest(bool direct){static Store room,call;return direct?call:room;}
}
