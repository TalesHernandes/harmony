// Headless: no window, capture, input injection or audio device is opened.
#include "../../native/gpu/device.cpp"
#include "../../native/gpu/mf.cpp"
#include <iostream>
#include <stdexcept>
void Check(bool ok,const char* what) { if(!ok)throw std::runtime_error(what); std::cout<<"PASS: "<<what<<'\n'; }
int main() { try {
  auto texture=gpu::Texture(128,64,DXGI_FORMAT_B8G8R8A8_UNORM);
  std::vector<uint8_t> pixels(128*64*4,128);
  gpu::Context()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),128*4,0);
  auto frame=webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get(),0,123);
  auto same=frame->CropAndScale(0,0,128,64,128,64);
  Check(same.get()==frame.get(),"identical crop retains the original frame and texture");
  frame=nullptr;texture.Reset();
  Check(same->ToI420()!=nullptr,"retained identity frame owns its texture after producer releases it");
  auto changed=same->CropAndScale(0,0,128,64,64,64);
  Check(changed.get()!=same.get() && changed->width()==64,"resizing still creates a correctly sized frame");
  auto cropped=same->CropAndScale(32,0,64,64,64,64);
  Check(cropped.get()!=same.get(),"nonidentity crop still copies the requested region");
  auto* native=static_cast<gpu::D3DFrameBuffer*>(changed.get());
  auto nv12=gpu::Scale(*native,64,64,DXGI_FORMAT_NV12);
  Check(nv12->format()==DXGI_FORMAT_NV12,"fallback scaling produces NV12 directly on GPU");
  gpu::MFEncoder encoder(64,64,30,200000,false);
  auto first=encoder.Encode(*nv12,0,true);
  auto second=encoder.Encode(*nv12,33333,false);
  auto third=encoder.Encode(*native,66666,true);
  Check(!first.empty() && !second.empty() && !third.empty(),"MF accepts direct NV12 readback and BGRA compatibility input");
  return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;} }
