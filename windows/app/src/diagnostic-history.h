#pragma once
#include "../../native/src/perf.h"
#include "../../native/src/log.h"
#include "../../native/src/network-state.h"
#include "../../native/src/resource-usage.h"
#include <windows.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <string>
#include <vector>
namespace app {
// 300 slots, one sample every two seconds, including time without an open tab.
template<class T,size_t Capacity=300> class HistoryRing {
  std::array<T,Capacity> data_{};
  size_t next_=0,size_=0;
 public:
  void Push(T value) {data_[next_]=std::move(value);next_=(next_+1)%Capacity;size_=std::min(size_+1,Capacity);}
  std::vector<T> Read() const {std::vector<T> result;result.reserve(size_);for(size_t i=0;i<size_;++i)result.push_back(data_[(next_+Capacity-size_+i)%Capacity]);return result;}
  void TrimBefore(double at) {while(size_) {auto& value=data_[(next_+Capacity-size_)%Capacity];if(value.at>at)break;value=T{};--size_;}}
  template<class F> void Visit(F visit) const{for(size_t i=0;i<size_;++i)visit(data_[(next_+Capacity-size_+i)%Capacity]);}
};
struct DiagnosticSample {double at=0;perf::Snapshot process;network::Sample room,call;resources::Snapshot resources;};
// The report's one line of numbers per sample, kept for six hours so a
// report covers a whole call, not only the last ten minutes (the owner,
// 29/10/2026). `call`: a room or direct call measured in the last 5 s.
struct DiagnosticRow {double at=0;float ws=0,priv=0,cpu=0,stage=0,rtt=-1,loss=-1;bool call=false;};
// The report's numbers (the owner, 29/10/2026: "se fosse da chamada inteira
// seria ótimo"): from a minute before the newest stretch with a call, gaps
// under 30 s joined; with no call in the six hours, the last ten minutes.
// Past two hours the older rows thin out; the last ten minutes stay whole.
// Returns how many seconds back the text starts, for the log to match.
inline double CallRowsText(const std::vector<DiagnosticRow>& rows,std::string& out) {
  if(rows.empty()) return 600;
  size_t begin=0,last=rows.size();
  while(begin+1<rows.size() && rows[begin].at<=rows.back().at-600) ++begin;
  for(size_t i=rows.size();i-->0;) if(rows[i].call){last=i;break;}
  if(last<rows.size()){
    size_t start=last;
    for(size_t i=last;i-->0;){ if(rows[i].call) start=i; else if(rows[start].at-rows[i].at>30) break; }
    begin=start; while(begin && rows[begin-1].at>=rows[start].at-60) --begin;
  }
  size_t count=rows.size()-begin,step=count>3600?(count-300+3299)/3300:1;
  char head[240];sprintf_s(head,"\n== %s, %.0f min, a cada %zu s (os últimos 10 min a cada 2 s): segundos ws_mb privada_mb cpu%% maior_etapa_ms rtt_ms perda%% ==\n",
    last<rows.size()?"desde o começo da chamada":"últimos 10 min",(rows.back().at-rows[begin].at)/60,step*2);out+=head;
  if(rows.size()>1 && rows.back().at-rows[rows.size()-2].at>3) out+="Ritmo em repouso: 10 s; em sala/chamada ou painel aberto: 2 s.\n";
  for(size_t i=begin;i<rows.size();++i){
    if(step>1 && i+300<rows.size() && (i-begin)%step) continue;
    auto& r=rows[i];char row[160];
    sprintf_s(row,"%.0f %.0f %.0f %.1f %.0f %.0f %.1f\n",r.at-rows[begin].at,r.ws,r.priv,r.cpu,r.stage,r.rtt,r.loss);out+=row;
  }
  return rows.back().at-rows[begin].at+5;
}
class DiagnosticHistory {
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_=false, active_=false, inCall_=false;
  HistoryRing<DiagnosticSample> samples_;
  HistoryRing<DiagnosticRow,10800> rows_;
  std::atomic<uint64_t> collectorCpu_{0},collected_{0};
  std::thread worker_;
  static uint64_t Cpu(){FILETIME c,e,k,u;if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u))return 0;return ((uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+((uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime);}
 public:
  DiagnosticHistory() : worker_([this] {
    perf::NameThread(L"harmony-diagnostics");auto beganCpu=Cpu();perf::TakeSnapshot();
    std::unique_lock lock(mutex_);double lastSummary=double(GetTickCount64())/1000.;
    while(!stop_) {
      auto interval=std::chrono::seconds(active_ || inCall_ ? 2 : 10);
      if(wake_.wait_for(lock,interval)!=std::cv_status::timeout) continue;
      if(stop_) break;
      lock.unlock();
      DiagnosticSample sample{double(GetTickCount64())/1000.,perf::TakeSnapshot(),network::Latest(false).Read(),network::Latest(true).Read(),resources::Read()};
      // One bounded summary per minute, never one line per stage per second.
      if(sample.at-lastSummary>=60){auto& p=sample.process;auto& r=sample.resources;size_t messages=0;uint64_t messageBytes=0;for(auto [n,b]:r.channels){messages+=n;messageBytes+=b;}
        lastSummary=sample.at;
        std::string top;for(auto& t:p.top_threads){if(!top.empty())top+=",";top+=t.name+":"+std::to_string(uint64_t(t.total_cpu_ms));}double uiMs=0;for(auto& stage:p.stages)if(stage.name=="diagnostics.ui-resources")uiMs=stage.avg_ms;
        logf("resources private=%llu ws=%llu encoded=%llu/%llu decoded=%llu/%llu emoji=%llu/%llu avatars=%llu/%llu channels=%zu messages=%zu/%llu voice=%u/%llu dfn=%u/%llu camera=%u/%llu camera_child=%llu threads=%u top_ms=[%s] handles=%u gdi=%u user=%u collector_cpu_ms=%.3f samples=%llu ui_resource_ms=%.4f retained_bytes=%llu",
          p.private_bytes,p.working_set_bytes,r.encodedCount,r.encodedBytes,r.decodedCount,r.decodedBytes,r.emojiCount,r.emojiBytes,r.avatarCount,r.avatarBytes,r.channels.size(),messages,messageBytes,
          r.parts[0].loaded,r.parts[0].loaded?r.parts[0].estimate:0,r.parts[1].loaded,r.parts[1].loaded?r.parts[1].estimate:0,r.parts[2].loaded,r.parts[2].loaded?r.parts[2].estimate:0,r.cameraPrivate,p.thread_count,top.c_str(),p.handles,p.gdi_objects,p.user_objects,double(collectorCpu_.load())/10000.,collected_.load(),uiMs,StorageBytes());
      }
      collectorCpu_=Cpu()-beganCpu;++collected_;
      DiagnosticRow row{sample.at,float(double(sample.process.working_set_bytes)/1048576),float(double(sample.process.private_bytes)/1048576),float(sample.process.process_cpu_percent)};
      for(auto& stage:sample.process.stages)row.stage=std::max(row.stage,float(stage.max_ms));
      auto tick=GetTickCount64();bool call=sample.call.at&&tick-sample.call.at<5000,room=sample.room.at&&tick-sample.room.at<5000;
      if(call||room){auto& n=call?sample.call:sample.room;row.rtt=float(n.rtt);row.loss=float(n.loss);row.call=true;}
      lock.lock();samples_.Push(std::move(sample));rows_.Push(row);
      samples_.TrimBefore(row.at-600);rows_.TrimBefore(row.at-21600);
    }
  }) {}
  // Visibility changes cadence, never whether collection exists.
  void Active(bool active) {{std::lock_guard lock(mutex_);if(active_==active)return;active_=active;}wake_.notify_all();}
  void InCall(bool active) {{std::lock_guard lock(mutex_);if(inCall_==active)return;inCall_=active;}wake_.notify_all();}
  std::vector<DiagnosticSample> Read() const {std::lock_guard lock(mutex_);return samples_.Read();}
  std::vector<DiagnosticRow> ReadRows() const {std::lock_guard lock(mutex_);return rows_.Read();}
  uint64_t Cpu100ns() const{return collectorCpu_.load();}uint64_t Collected() const{return collected_.load();}
  uint64_t StorageBytes() const {
    std::lock_guard lock(mutex_);uint64_t bytes=sizeof(samples_)+sizeof(rows_);
    samples_.Visit([&](auto const& s){auto& p=s.process;bytes+=p.stages.capacity()*sizeof(perf::StageRow)+(p.threads.capacity()+p.top_threads.capacity())*sizeof(perf::ThreadRow);
      for(auto& v:p.stages)bytes+=v.name.capacity();for(auto& v:p.threads)bytes+=v.name.capacity();for(auto& v:p.top_threads)bytes+=v.name.capacity();
      bytes+=s.resources.channels.capacity()*sizeof(std::pair<size_t,uint64_t>)+s.resources.channelNames.capacity()*sizeof(std::string);for(auto& n:s.resources.channelNames)bytes+=n.capacity();
    });return bytes;
  }
  ~DiagnosticHistory() { {std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();if(worker_.joinable())worker_.join(); }
};
}
