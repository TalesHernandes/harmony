#pragma once
#include "noise-filter.h"
#include "df-api.h"
#include "resource-usage.h"
#include <windows.h>
#include <wincrypt.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
namespace audio {
// Deny write/delete sharing until teardown, including after Rust opens the model.
inline HANDLE VerifiedFile(const std::filesystem::path& path, uint64_t size, const char* expected) {
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE) return file;
  LARGE_INTEGER length{}; HCRYPTPROV provider=0; HCRYPTHASH hash=0;
  bool ok=GetFileSizeEx(file,&length) && uint64_t(length.QuadPart)==size &&
    CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);
  std::array<BYTE,65536> buffer{}; DWORD count=0;
  while(ok) { if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&count,nullptr)) { ok=false; break; } if(!count) break; ok=CryptHashData(hash,buffer.data(),count,0)!=0; }
  BYTE digest[32]{}; DWORD bytes=32; if(ok) ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&bytes,0)!=0 && bytes==32;
  std::string hex; const char* digits="0123456789abcdef"; for(auto b:digest) { hex+=digits[b>>4]; hex+=digits[b&15]; }
  ok=ok && hex==expected; if(hash) CryptDestroyHash(hash); if(provider) CryptReleaseContext(provider,0);
  if(!ok) { CloseHandle(file); return INVALID_HANDLE_VALUE; } return file;
}
inline std::filesystem::path DfnDirectory() {
  std::wstring path(32768,L'\0'); auto n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
  if(!n || n>=path.size()) return {}; path.resize(n); return std::filesystem::path(path).parent_path();
}
struct DfnApi {
  decltype(&df_create) create=nullptr; decltype(&df_get_frame_length) length=nullptr;
  decltype(&df_set_atten_lim) strength=nullptr; decltype(&df_process_frame) process=nullptr; decltype(&df_free) free=nullptr;
};
class DfnFilter final : public NoiseFilter {
 public:
  explicit DfnFilter(std::filesystem::path directory=DfnDirectory(), DfnApi injected={}) : api_(injected) {
    if(directory.empty() || !directory.is_absolute()) return;
    auto model=directory/L"DeepFilterNet3_onnx.tar.gz";
    model_=VerifiedFile(model,7983136,"c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616");
    if(model_==INVALID_HANDLE_VALUE) return;
    auto utf8=model.u8string(); path_.assign(reinterpret_cast<const char*>(utf8.data()),utf8.size());
    if(!api_.create) {
      dllFile_=VerifiedFile(directory/L"df.dll",16678912,"4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f");
      if(dllFile_==INVALID_HANDLE_VALUE) return;
      dll_=LoadLibraryExW((directory/L"df.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
      if(!dll_) return;
      api_.create=reinterpret_cast<decltype(api_.create)>(GetProcAddress(dll_,"df_create"));
      api_.length=reinterpret_cast<decltype(api_.length)>(GetProcAddress(dll_,"df_get_frame_length"));
      api_.strength=reinterpret_cast<decltype(api_.strength)>(GetProcAddress(dll_,"df_set_atten_lim"));
      api_.process=reinterpret_cast<decltype(api_.process)>(GetProcAddress(dll_,"df_process_frame"));
      api_.free=reinterpret_cast<decltype(api_.free)>(GetProcAddress(dll_,"df_free"));
    }
    if(!api_.create || !api_.length || !api_.strength || !api_.process || !api_.free) { api_={}; return; }
    Reset(1); // Create outside the recording callback.
    if(state_) resourceLoad_.Loaded();
  }
  ~DfnFilter() override { if(state_) api_.free(state_); if(dll_) FreeLibrary(dll_); if(model_!=INVALID_HANDLE_VALUE) CloseHandle(model_); if(dllFile_!=INVALID_HANDLE_VALUE) CloseHandle(dllFile_); }
  bool Available() const override { return state_!=nullptr; }
  bool Process(std::vector<int16_t>& block,int rate,size_t channels,int strengthDb) override {
    if(rate!=48000 || (channels!=1 && channels!=2) || block.size()!=480*channels || strengthDb<10 || strengthDb>100) return false;
    if(!state_) return false;
    if(channels!=channels_ && !Reset(channels)) return false;
    // At the full limit DFN3 takes the weak half of a word down with the room:
    // the owner's voice went out 6 dB lower on average and 11 dB on the weak
    // syllables, and a friend heard it pumping (29/10/2026, the owner's call
    // recording replayed offline). While DFN's own SNR estimate says someone
    // is talking, and 50 ms after, the limit is 10 dB (speech 3.7 dB lower,
    // steady); the pauses keep the chosen strength, so keyboard and mouse
    // still vanish. Clicks never rose above 3.5 dB SNR there; speech sat at
    // 13-30. Above 10, not 5: at 5 the loud noise of the speech fixture
    // counted as talk and its pauses lost 5 dB of suppression.
    api_.strength(state_,float(speechLeft_>0 ? std::min(strengthDb,kSpeechLimitDb) : strengthDb));
    // A microphone muted at its own button sends exact zeros, and the network
    // fed only zeros turns to NaN within a minute (the web's worklet did, 16/10).
    // Silence goes out as silence without touching it.
    bool silent=true; for(auto sample:block) if(sample) { silent=false; break; }
    if(silent) return true;
    std::array<float,480> input{},output{};
    for(size_t i=0;i<480;++i) { int sum=0; for(size_t c=0;c<channels;++c) sum+=block[i*channels+c]; input[i]=float(sum)/(32768.f*float(channels)); }
    auto snr=api_.process(state_,input.data(),output.data());
    bool finite=std::isfinite(snr);
    for(auto value:output) if(!std::isfinite(value)) finite=false;
    // A network gone NaN stays NaN: it is made again, up to three times in a
    // call, and after that the block keeps the raw voice (the caller's fallback).
    if(!finite) { if(remakes_<3) { ++remakes_; Reset(channels_); } return false; }
    speechLeft_=snr>kSpeechSnrDb ? kSpeechHoldBlocks : std::max(speechLeft_-1,0);
    for(size_t i=0;i<480;++i) for(size_t c=0;c<channels;++c) block[i*channels+c]=int16_t(std::clamp(output[i]*32768.f,-32768.f,32767.f));
    return true;
  }
 private:
  bool Reset(size_t channels) {
    if(state_) { api_.free(state_); state_=nullptr; }
    state_=api_.create(path_.c_str(),50.f); channels_=channels; speechLeft_=0;
    if(!state_) return false;
    if(api_.length(state_)!=480) { api_.free(state_); state_=nullptr; return false; } return true;
  }
  static constexpr int kSpeechLimitDb=10, kSpeechHoldBlocks=5;
  static constexpr float kSpeechSnrDb=10.f;
  resources::Load resourceLoad_{resources::Dfn};
  DfnApi api_{}; DFState* state_=nullptr; size_t channels_=1; int remakes_=0, speechLeft_=0;
  HANDLE model_=INVALID_HANDLE_VALUE,dllFile_=INVALID_HANDLE_VALUE; HMODULE dll_=nullptr; std::string path_;
};
}
