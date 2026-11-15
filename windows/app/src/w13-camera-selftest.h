#pragma once
// Test-only process inventory. Sample Windows heaps every thirty seconds.
// Heap walking holds allocation locks; frame-rate results include this overhead.
#include <psapi.h>
#include <tlhelp32.h>
#include "../../native/src/resource-usage.h"
inline json::Value W13Memory() {
  auto v=json::Value::Object();PROCESS_MEMORY_COUNTERS_EX m{};
  GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof m);
  v["private_bytes"]=int64_t(m.PrivateUsage);v["working_bytes"]=int64_t(m.WorkingSetSize);
  DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);v["handles"]=int64_t(handles);
  int count=0;auto threads=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 t{sizeof t};
  if(Thread32First(threads,&t))do{if(t.th32OwnerProcessID==GetCurrentProcessId())++count;}while(Thread32Next(threads,&t));
  CloseHandle(threads);v["threads"]=count;
  auto resources=resources::Read();v["camera_child_private"]=int64_t(resources.cameraPrivate);
  auto parts=json::Value::Object();for(size_t i=0;i<3;++i){auto p=json::Value::Object();p["loaded"]=int(resources.parts[i].loaded);p["observed_load_delta"]=int64_t(resources.parts[i].estimate);parts[i==0?"voice":i==1?"dfn":"camera"]=p;}v["resources"]=parts;
  static ULONGLONG last=0;
  if(GetTickCount64()-last>=30000){last=GetTickCount64();
    DWORD n=GetProcessHeaps(0,nullptr);std::vector<HANDLE> heaps(n);n=GetProcessHeaps(n,heaps.data());uint64_t bytes=0,blocks=0;int failed=0;
    if(n<=heaps.size())for(DWORD i=0;i<n;++i){if(!HeapLock(heaps[i])){++failed;continue;}PROCESS_HEAP_ENTRY e{};
      while(HeapWalk(heaps[i],&e))if(e.wFlags&PROCESS_HEAP_ENTRY_BUSY){bytes+=e.cbData;++blocks;}
      if(GetLastError()!=ERROR_NO_MORE_ITEMS)++failed;HeapUnlock(heaps[i]);}
    v["heap_busy_bytes"]=int64_t(bytes);v["heap_busy_blocks"]=int64_t(blocks);v["heaps_failed"]=failed;
  }return v;
}

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
class W13VideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
  std::mutex mutex_;webrtc::scoped_refptr<webrtc::VideoTrackInterface> track_;
 public:
  std::atomic<uint64_t> frames{0};
  void OnFrame(const webrtc::VideoFrame&)override{++frames;}
  void Attach(webrtc::VideoTrackInterface* track){std::lock_guard lock(mutex_);if(track_)track_->RemoveSink(this);track_=track;if(track_)track_->AddOrUpdateSink(this,webrtc::VideoSinkWants{});}
  ~W13VideoSink(){Attach(nullptr);}
};
