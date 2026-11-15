// Deterministic capture-time regression; no device, account or network needed.
#include "loopback.h"
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << std::endl; std::exit(1); } } while (false)

int main() {
  loopback::CaptureBlocks blocks;
  std::vector<std::optional<int64_t>> stamps;
  auto collect = [&](const int16_t* samples, std::optional<int64_t> at) {
    stamps.push_back(at);
    for (int i=0;i<960;++i) CHECK(samples[i]==0);
  };
  // A 200-ms delivery backlog remains twenty distinct capture instants.
  blocks.Push(nullptr,9600,1000000000LL,false,collect);
  CHECK(stamps.size()==20);
  for (size_t i=0;i<20;++i) CHECK(stamps[i]==1000+int64_t(i)*10);
  stamps.clear();
  blocks.Push(nullptr,240,2000000000LL,false,collect);
  blocks.Push(nullptr,720,2005000000LL,false,collect);
  CHECK(stamps.size()==2 && stamps[0]==2000 && stamps[1]==2010);
  stamps.clear();
  blocks.Push(nullptr,240,3000000000LL,false,collect);
  blocks.Push(nullptr,480,4000000000LL,true,collect);
  CHECK(stamps.size()==1 && stamps[0]==4000);
  stamps.clear();
  blocks.Push(nullptr,480,std::nullopt,false,collect);
  CHECK(stamps.size()==1 && !stamps[0]);
  std::cout << "capture timestamps: backlog, partial blocks, discontinuity, missing: PASS\n";
}
