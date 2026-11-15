#include "../src/image-cache-budget.h"
#include "../../native/src/resource-usage.h"
#include "../../native/src/perf.h"
#include <iostream>
#include <stdexcept>
void Check(bool ok,char const* label){if(!ok)throw std::runtime_error(label);std::cout<<"PASS: "<<label<<'\n';}
int main(){try{
  for(auto size:{28.,32.,36.})for(auto scale:{1.,1.25,1.5,2.,2.5,3.,4.,5.,6.}) {
    auto pixels=app::AvatarDecodePixels(size,scale);
    Check(pixels==0 || (pixels>=std::ceil(size*scale) && (pixels==48||pixels==96||pixels==192)),"avatar buckets round up device pixels without adding arbitrary cache sizes");
  }
  Check(app::AvatarDecodePixels(72,1)==0 && app::AvatarDecodePixels(128,2)==0,"profile avatars retain full resolution");
  app::ImageCacheBudget cache;std::map<std::string,bool> retained;auto evict=[&](auto const& key){retained.erase(key);};
  for(int i=0;i<10000;++i){auto key="emoji-size-"+std::to_string(i);if(cache.Retain(key,512*1024,evict))retained[key]=true;if(cache.Bytes()>cache.Limit || cache.Count()>cache.Entries)throw std::runtime_error("decoded cache exceeded its bounds");}
  Check(cache.Count()==64 && retained.size()==64,"emoji sizes share the same byte budget as photographs");
  cache.Touch("emoji-size-9936");cache.Retain("next",512*1024,evict);Check(retained.contains("emoji-size-9936") && !retained.contains("emoji-size-9937"),"LRU retains recently reused emoji");
  Check(!cache.Retain("oversize",cache.Limit+1,evict),"large visible image is delivered without retaining an oversized surface");cache.Clear();Check(cache.Count()==0 && cache.Bytes()==0,"account reset drops decoded accounting");
  auto initial=resources::Read().parts[resources::Voice].loaded;
  {resources::Load load(resources::Voice);load.Loaded();load.Loaded();Check(resources::Read().parts[resources::Voice].loaded==initial+1,"loading is counted once per engine lifetime");}
  Check(resources::Read().parts[resources::Voice].loaded==initial,"engine destruction returns voice attribution to idle");
  auto handles=[](){DWORD n=0;GetProcessHandleCount(GetCurrentProcess(),&n);return n;};auto before=handles();for(int i=0;i<10000;++i)perf::NameThread(L"resource-test");Check(handles()<=before+2,"naming repeated threads does not retain duplicate process handles");
  auto& stage = perf::Get("resource-test.counter");
  stage.Record(2000000); stage.Drop(); stage.SetValue(42);
  auto counters = perf::TakeSnapshot(); bool exact = false;
  for (auto const& row : counters.stages) if (row.name == "resource-test.counter")
    exact = row.in == 2 && row.out == 1 && row.dropped == 1 && row.avg_ms == 2 && row.max_ms == 2 && row.value == 42;
  Check(exact, "cached stages preserve diagnostic counts, timings and values");
  for (int i = 0; i < 100; ++i) perf::Get("resource-test.registry-" + std::to_string(i));
  Check(&stage == &perf::Get("resource-test.counter"), "cached stage references survive registry growth and snapshots");
  stage.Record(1000000); counters = perf::TakeSnapshot(); exact = false;
  for (auto const& row : counters.stages) if (row.name == "resource-test.counter")
    exact = row.in == 1 && row.out == 1 && row.dropped == 0 && row.avg_ms == 1 && row.max_ms == 1 && row.value == 42;
  Check(exact, "snapshots preserve interval deltas and reset only the maximum");
  auto snapshot=perf::TakeSnapshot();Check(snapshot.private_bytes>0 && snapshot.working_set_bytes>0 && snapshot.thread_count>0 && snapshot.top_threads.size()<=5,"resource snapshot exposes actual process counters and five lifetime CPU groups");
  return 0;
}catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
