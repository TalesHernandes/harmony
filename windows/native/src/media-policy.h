#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
namespace media {
template<class T> T* Outgoing(const std::string& source,T* mic,T* cam,T* screen,T* screenAudio) {
  return source=="mic"?mic:source=="cam"?cam:source=="screen"?screen:source=="screen_audio"?screenAudio:nullptr;
}
// web/harmony/src/media.ts: layerFor, measureSoon and judgeDecode.
inline int Layer(double width, double dpi, bool visible=true) {
  if(!visible || width<1) return 2;
  double real=width*dpi; return real>=1200?0:real>=600?1:2;
}
inline const char* Rid(int layer) { return layer==0?"alta":layer==1?"media":"baixa"; }
struct DecodeHealth {
  int cap=0,bad=0,clean=0,fails=0; int64_t moved=0,lifted=0;
  bool Sample(int decoded,int dropped,int64_t now) {
    int total=decoded+dropped;
    if(total<10 || now-moved<20000) return false;
    double fraction=double(dropped)/total; int next=cap;
    if(fraction>=0.1) { clean=0; if(++bad>=3 && cap<2) next=cap+1; }
    else if(fraction<=0.02) { bad=0; if(++clean>=20*std::min(fails?fails:1,6)&&cap>0) next=cap-1; }
    else { clean=bad=0; }
    if(next==cap) return false;
    cap=next; moved=now; clean=bad=0;
    if(cap) fails=lifted>0&&now-lifted>=300000?1:fails+1; else lifted=now;
    return true;
  }
};
struct LayerRequest {
  int requested=-1,desired=2; int64_t due=0,retryAt=0; int retryDelay=0; DecodeHealth decode;
  void Resize(double width,double dpi,bool visible,int64_t now) { desired=Layer(width,dpi,visible); if(now>=retryAt) due=now+400; Look(visible&&width>=1,now); }
  // Not being looked at is a state, not a moment. A peek at a text channel or
  // a quick minimise must not stop and restart somebody's stream: the restart
  // costs a key frame and, for a sole viewer, the sender's layer. Only an
  // absence this long stops the stream; looking again starts it at once.
  static constexpr int64_t kAwayPauseMs=30000;
  bool shown=true,paused=false; int64_t awaySince=0;
  void Look(bool looking,int64_t now) { if(looking){shown=true;awaySince=0;} else if(shown){shown=false;awaySince=now;} }
  // +1: stop receiving now; -1: start receiving again now; 0: nothing to do.
  int PauseMove(int64_t now) const { return !shown&&!paused&&now-awaySince>=kAwayPauseMs ? 1 : shown&&paused ? -1 : 0; }
  void Paused(bool on,int64_t now) { paused=on; if(!on){requested=-1;due=now;} }
  bool CanAttempt(int64_t now) const { return now>=due && now>=retryAt; }
  int Ready(int64_t now) const { return CanAttempt(now) ? std::max(desired,decode.cap) : requested; }
  // Reports only the first failure and the recovery of a failure sequence.
  bool Complete(int layer,bool success,int64_t now) {
    bool report=success ? retryDelay!=0 : retryDelay==0;
    if(success) {requested=layer;retryDelay=0;retryAt=0;}
    else {retryDelay=retryDelay?std::min(retryDelay*2,5000):500;retryAt=now+retryDelay;}
    return report;
  }
};
struct WindowFacts { bool visible=false,tool=false,cloaked=false,own=false,excluded=false; };
inline bool Capturable(WindowFacts f) { return f.visible&&!f.tool&&!f.cloaked&&!f.own&&!f.excluded; }
// Lexical Windows paths: reject relative paths, dot components and device aliases.
inline std::wstring FollowPath(std::wstring path) {
  for(auto& c:path) {
    if(c==L'/')c=L'\\';
    if(c>=L'A'&&c<=L'Z')c+=32;
    if(c>=0x00c0&&c<=0x00de&&c!=0x00d7)c+=32;
  }
  while(path.size()>3&&path.back()==L'\\')path.pop_back();
  if(path.size()<4||path[1]!=L':'||path[2]!=L'\\')return {};
  size_t start=3;
  while(start<path.size()) {
    auto end=path.find(L'\\',start);if(end==std::wstring::npos)end=path.size();
    auto part=path.substr(start,end-start);
    if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' '||part.find(L':')!=std::wstring::npos)return {};
    start=end+1;
  }
  return path;
}
inline std::wstring FollowDirectory(const std::wstring& exe) {
  auto path=FollowPath(exe);auto slash=path.rfind(L'\\');
  return slash==std::wstring::npos?std::wstring():path.substr(0,slash);
}
inline bool FollowWithin(const std::wstring& path,const std::wstring& folder) {
  return !folder.empty()&&(path==folder||(path.size()>folder.size()&&path.compare(0,folder.size(),folder)==0&&path[folder.size()]==L'\\'));
}
inline bool FollowGeneric(const std::wstring& folder,const std::wstring& own,const std::wstring& windows) {
  if(folder.size()<=3)return true;
  auto tail=folder.substr(3);
  return FollowWithin(folder,windows)||FollowWithin(folder,own)||tail==L"windows"||
      tail==L"windows\\system32"||tail==L"system32"||tail==L"program files"||tail==L"program files (x86)"||tail==L"programdata"||tail==L"users";
}
inline bool FollowFolder(const std::wstring& originalExe,const std::wstring& candidateExe,
                         const std::wstring& ownExe,const std::wstring& windowsFolder) {
  auto original=FollowDirectory(originalExe),candidate=FollowDirectory(candidateExe);
  auto own=FollowDirectory(ownExe),windows=FollowPath(windowsFolder);
  // Missing identity is not permission to capture an unrelated program.
  if(original.empty()||candidate.empty()||own.empty()||windows.empty()||
      FollowGeneric(original,own,windows)||FollowGeneric(candidate,own,windows))return false;
  auto parent=original.substr(0,original.rfind(L'\\'));
  return FollowWithin(candidate,original)||(!FollowGeneric(parent,own,windows)&&candidate==parent);
}
struct FollowChoice { bool foreground=false; uint64_t area=0; };
inline bool FollowBetter(FollowChoice candidate,FollowChoice current) {
  return candidate.foreground!=current.foreground?candidate.foreground:candidate.area>current.area;
}
struct FollowSearch {
  bool started=false; uint64_t since=0,next=0;
  bool Poll(bool closed,uint64_t now) {
    if(!closed)return false;
    if(!started){started=true;since=now;next=now;}
    if(now-since>=60000||now<next)return false;
    next=now+500;return true;
  }
};
struct Transmission {
  bool active=false,minimized=false,closed=false; int width=0,height=0; uint64_t revision=0;
  bool Choose(bool valid) {if(!valid)return false;active=true;minimized=closed=false;width=height=0;++revision;return true;}
  void Stop() {active=false;minimized=closed=false;width=height=0;}
  const char* Observe(bool alive,bool ended,bool iconic,int w,int h) {
    if(!active)return nullptr;
    // A live capture source now repeats its final picture with the closure
    // notice. Unpublishing here would remove that notice from every receiver.
    if(!alive&&!ended) {if(closed)return nullptr;closed=true;return "window_closed";}
    if(!alive) {Stop();return "window_closed";}
    if(ended) {Stop();return "capture_lost";}
    if(iconic!=minimized) {minimized=iconic;return iconic?"minimized":"transmitting";}
    if(!iconic&&w>0&&h>0&&(w!=width||h!=height)) {width=w;height=h;return "resized";}
    return nullptr;
  }
};
struct AudioTarget { uint32_t pid=0; bool include=false; };
inline AudioTarget SoundTarget(uint32_t windowPid,uint32_t ownPid) {
  return windowPid ? AudioTarget{windowPid,true} : AudioTarget{ownPid,false};
}
inline bool SoundIncludes(AudioTarget t,uint32_t process,bool inTargetTree) {
  (void)process; return t.include?inTargetTree:!inTargetTree;
}
inline int ScreenCeiling(int height,int fps) {
  if(height==480) return 6000000;
  if(height==720) return fps==60?12000000:fps==30?9000000:6000000;
  return fps==60?15000000:fps==30?12000000:9000000;
}
inline int ScreenStart(int height,int fps) {
  if(height==480) return 2400;
  if(height==720) return fps==60?4800:fps==30?3600:2400;
  return fps==60?6000:fps==30?4800:3600;
}
}
