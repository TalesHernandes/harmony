#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <map>
#include <string>
namespace app {
// Small avatars share three device-pixel buckets; profiles keep the full image.
inline int AvatarDecodePixels(double size,double scale) {
  if(size>36) return 0;
  auto pixels=int(std::ceil(size*scale));
  return pixels<=48?48:pixels<=96?96:pixels<=192?192:0;
}

// Decoded surfaces, including every emoji size, share one bounded LRU.
class ImageCacheBudget {
  struct Entry {uint64_t bytes=0,used=0;};
  std::map<std::string,Entry> entries_;uint64_t bytes_=0,sequence_=0;
 public:
  static constexpr uint64_t Limit=32ull<<20;
  static constexpr size_t Entries=512;
  void Touch(std::string const& key){if(auto p=entries_.find(key);p!=entries_.end())p->second.used=++sequence_;}
  template<class Evict> bool Retain(std::string const& key,uint64_t bytes,Evict evict) {
    if(auto p=entries_.find(key);p!=entries_.end()){bytes_-=p->second.bytes;entries_.erase(p);}
    if(bytes>Limit){evict(key);return false;}
    while(!entries_.empty() && (bytes_+bytes>Limit || entries_.size()>=Entries)){
      auto old=std::min_element(entries_.begin(),entries_.end(),[](auto& a,auto& b){return a.second.used<b.second.used;});bytes_-=old->second.bytes;evict(old->first);entries_.erase(old);
    }
    bytes_+=bytes;entries_[key]={bytes,++sequence_};return true;
  }
  uint64_t Bytes() const{return bytes_;}size_t Count() const{return entries_.size();}
  void Clear(){entries_.clear();bytes_=0;}
};
}
