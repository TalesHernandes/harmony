#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace camera {
// 720p in either orientation: a phone used as a webcam (Camo, 720x1280)
// stands up, and measuring it against 1280x720 lying down shrank it to
// 404x720 and then the tile cut its middle out (the owner, 02/11/2026: "a
// visualização da minha própria câmera tá com uma qualidade ruim").
inline double FormatScore(int width,int height,double fps,int conversionCost) {
  if(width<2||height<2||fps<=0)return 1e30;
  int longer=std::max(width,height),shorter=std::min(width,height);
  return std::abs(double(longer)-1280)+std::abs(double(shorter)-720)+std::abs(fps-30)*30+conversionCost*0.1;
}
struct Size {int width,height;};
inline Size WebSize(int width,int height) {
  double scale=std::min({1.0,1280.0/std::max(width,height),720.0/std::min(width,height)});
  return {std::max(2,int(width*scale)/2*2),std::max(2,int(height*scale)/2*2)};
}
inline bool CameraDue(int64_t now,int64_t& next) {
  if(next&&now<next-16666)return false;
  next=(next&&next>now-33333?next:now)+33333;return true;
}
}
