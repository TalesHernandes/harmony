#include "../../native/src/dfn-filter.h"
#include <fstream>
#include <iostream>
#include <limits>
namespace {
int creates=0,frees=0,calls=0; float strength=0,snr=0; bool nullCreate=false,badOutput=false; size_t frame=480;
DFState* Create(const char* path,float) { ++creates; if(!path || !*path) std::abort(); return nullCreate?nullptr:reinterpret_cast<DFState*>(1); }
size_t Length(DFState*) { return frame; }
void Strength(DFState*,float value) { strength=value; }
float Process(DFState*,float* in,float* out) { ++calls; for(int i=0;i<480;++i) out[i]=badOutput?std::numeric_limits<float>::quiet_NaN():in[i]; return snr; }
void Free(DFState*) { ++frees; }
int failures=0;
void Check(bool ok,const char* label) { if(!ok) { ++failures; std::cerr<<label<<"\n"; } }
}
int wmain(int argc,wchar_t** argv) {
  if(argc<2) return 2;
  auto directory=std::filesystem::absolute(argv[1]);
  if(argc==4) {
    // Exercise the exact production adapter and FilterSlot on the real mixture.
    std::ifstream in(argv[2],std::ios::binary); std::ofstream out(argv[3],std::ios::binary);
    audio::FilterSlot slot(std::make_unique<audio::DfnFilter>(directory));
    std::vector<int16_t> block(480); uint64_t processed=0;
    while(in.read(reinterpret_cast<char*>(block.data()),960)) {
      slot.Process(block,48000,1,50); out.write(reinterpret_cast<const char*>(block.data()),960); ++processed;
    }
    std::cout<<"blocks="<<processed<<" fallbacks="<<slot.fallbacks<<" active="<<slot.active<<"\n";
    return !out || !processed || !slot.active;
  }
  audio::DfnApi api{Create,Length,Strength,Process,Free};
  std::vector<int16_t> block(480,1200),original=block;
  {
    audio::DfnFilter filter(directory,api);
    Check(creates==1,"dfn verified model creates state");
    Check(filter.Process(block,48000,1,73) && strength==73,"dfn strength applied");
    filter.Process(block,48000,1,50); Check(creates==1,"dfn state retained between blocks");
    // Speech softens the limit for 50 ms; a click-level SNR never does.
    snr=3.5f; filter.Process(block,48000,1,50); filter.Process(block,48000,1,50); Check(strength==50,"dfn click SNR keeps the full limit");
    snr=20; filter.Process(block,48000,1,50); snr=-14; filter.Process(block,48000,1,50); Check(strength==10,"dfn speech SNR lowers the limit to 10 dB");
    for(int i=0;i<3;++i) filter.Process(block,48000,1,50);
    filter.Process(block,48000,1,10); Check(strength==10,"dfn the slider's lowest limit still applies during speech");
    Check(strength==10,"dfn speech limit holds 50 ms"); filter.Process(block,48000,1,50); Check(strength==50,"dfn full limit returns after the hold");
    block=original; snr=0;
    std::vector<int16_t> stereo(960,1200); filter.Process(stereo,48000,2,50);
    Check(creates==2 && frees==1,"dfn format change recreates state");
    Check(stereo[0]==stereo[1],"dfn mono copied to stereo");
    auto before=calls; Check(!filter.Process(block,44100,1,50) && calls==before,"dfn invalid rate never calls Rust");
    block.resize(479); Check(!filter.Process(block,48000,1,50) && calls==before,"dfn invalid frame never calls Rust"); block=original;
    filter.Process(block,48000,1,50); block=original; // back to mono first: that change alone remakes the state
    badOutput=true; auto made=creates; Check(!filter.Process(block,48000,1,50) && block==original,"dfn nonfinite output preserves input"); badOutput=false;
    Check(creates==made+1 && filter.Process(block,48000,1,50),"dfn nonfinite output remakes the network, and the next block is filtered");
    badOutput=true; for(int i=0;i<5;++i) filter.Process(block,48000,1,50); badOutput=false;
    Check(creates==made+3,"dfn remakes at most three times in a call");
    std::vector<int16_t> quiet(480,0); before=calls; Check(filter.Process(quiet,48000,1,50) && calls==before && quiet==std::vector<int16_t>(480,0),"dfn exact silence passes without the network");
  }
  Check(frees==creates,"dfn frees all states");
  nullCreate=true;
  audio::FilterSlot slot(std::make_unique<audio::DfnFilter>(directory,api));
  Check(!slot.Process(block,48000,1) && block==original && slot.fallbacks==1,"dfn null create uses raw NS block");
  nullCreate=false;
  auto missing=directory/L"missing"; std::filesystem::create_directories(missing);
  auto before=creates;
  { audio::DfnFilter filter(missing,api); Check(!filter.Process(block,48000,1,50) && creates==before,"dfn absent model never calls create"); }
  { std::ofstream bad(missing/L"DeepFilterNet3_onnx.tar.gz",std::ios::binary); bad<<"truncated"; }
  { audio::DfnFilter filter(missing,api); Check(!filter.Process(block,48000,1,50) && creates==before,"dfn truncated model never calls create"); }
  { std::ofstream bad(missing/L"DeepFilterNet3_onnx.tar.gz",std::ios::binary); bad.seekp(7983135); bad.put(0); }
  { audio::DfnFilter filter(missing,api); Check(!filter.Process(block,48000,1,50) && creates==before,"dfn replaced model never calls create"); }
  audio::DfnFilter real(directory);
  Check(real.Process(block,48000,1,50),"dfn real DLL accepts UTF8 accented absolute path");
  std::cout<<"DFN adapter regressions failures="<<failures<<"\n";
  return failures?1:0;
}
