#include "../src/photo-crop.h"
#include <cassert>
#include <iostream>
int main() {
  PhotoCrop crop;crop.width=800;crop.height=400;crop.pixels.resize(800*400*4);
  for(int y=0;y<400;++y)for(int x=0;x<800;++x){auto i=(y*800+x)*4;crop.pixels[i]=x>=400?255:0;crop.pixels[i+1]=0;crop.pixels[i+2]=x<400?255:0;crop.pixels[i+3]=255;}
  crop.Constrain();auto centered=crop.Render(512);assert(centered.size()==512*512*4);assert(centered[2]==255 && centered[511*4]==255);
  crop.x=10000;crop.y=-10000;crop.Constrain();assert(crop.x==256 && crop.y==0);auto left=crop.Render(512);assert(left[(256*512+256)*4+2]==255);
  crop.zoom=2;crop.x=-10000;crop.y=10000;crop.Constrain();assert(crop.x==-768 && crop.y==256);auto right=crop.Render(512);assert(right[(256*512+256)*4]==255);
  crop.zoom=100;crop.Constrain();assert(crop.zoom==4);crop.zoom=0;crop.Constrain();assert(crop.zoom==1);
  crop.width=crop.height=1;crop.pixels={0,0,0,0};crop.x=crop.y=0;auto transparent=crop.Render(512);assert(transparent[0]==255 && transparent[1]==255 && transparent[2]==255 && transparent[3]==255);
  std::cout<<"PASS: photo crop center, bounded pan/zoom, output size and transparency\n";
}
