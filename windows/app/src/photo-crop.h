#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Offsets use the web editor's 512-pixel output coordinate system.
struct PhotoCrop {
  int width=0,height=0;
  double zoom=1,x=0,y=0;
  std::vector<uint8_t> pixels;
  double Scale() const { return std::max(512.0/width,512.0/height)*zoom; }
  void Constrain() {
    zoom=std::clamp(zoom,1.0,4.0);
    double mx=std::max(0.0,(width*Scale()-512)/2),my=std::max(0.0,(height*Scale()-512)/2);
    x=std::clamp(x,-mx,mx); y=std::clamp(y,-my,my);
  }
  std::vector<uint8_t> Render(int edge) const {
    std::vector<uint8_t> out(size_t(edge)*edge*4);
    double scale=Scale();
    for(int row=0;row<edge;++row) for(int col=0;col<edge;++col) {
      double sx=std::clamp(((col+.5)*512/edge-256-x)/scale+width/2.0-.5,0.0,double(width-1));
      double sy=std::clamp(((row+.5)*512/edge-256-y)/scale+height/2.0-.5,0.0,double(height-1));
      int left=int(sx),top=int(sy),right=std::min(left+1,width-1),bottom=std::min(top+1,height-1);
      auto at=[&](int px,int py,int c){return pixels[(size_t(py)*width+px)*4+c];};
      double fx=sx-left,fy=sy-top;
      double alpha=(at(left,top,3)*(1-fx)+at(right,top,3)*fx)*(1-fy)+(at(left,bottom,3)*(1-fx)+at(right,bottom,3)*fx)*fy;
      for(int c=0;c<3;++c) {
        double value=(at(left,top,c)*(1-fx)+at(right,top,c)*fx)*(1-fy)+(at(left,bottom,c)*(1-fx)+at(right,bottom,c)*fx)*fy;
        // The decoded pixels are premultiplied; JPEG composites them over white.
        out[(size_t(row)*edge+col)*4+c]=uint8_t(std::clamp(std::round(value+255-alpha),0.0,255.0));
      }
      out[(size_t(row)*edge+col)*4+3]=255;
    }
    return out;
  }
};
