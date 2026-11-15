#pragma once
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <wrl/implements.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
// Async MF ReadSample never blocks the caller waiting for a driver frame.
// Callback lifetime belongs to MF as well as the worker; late completions do
// not refer to a destroyed camera object.
namespace camera {
class ReaderCallback final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IMFSourceReaderCallback> {
 public:
  STDMETHODIMP OnReadSample(HRESULT hr,DWORD,DWORD flags,LONGLONG timestamp,IMFSample* sample) override {
    {std::lock_guard lock(mu_);hr_=hr;flags_=flags;timestamp_=timestamp;sample_=sample;ready_=true;}wake_.notify_all();return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override {return S_OK;}
  STDMETHODIMP OnEvent(DWORD,IMFMediaEvent*) override {return S_OK;}
  HRESULT Read(IMFSourceReader* reader,std::atomic<bool>& stop,DWORD& flags,LONGLONG& timestamp,Microsoft::WRL::ComPtr<IMFSample>& sample) {
    {std::lock_guard lock(mu_);ready_=false;sample_.Reset();}
    HRESULT hr=reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,nullptr,nullptr,nullptr);if(FAILED(hr))return hr;
    std::unique_lock lock(mu_);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!ready_&&!stop) {if(wake_.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(50)))==std::cv_status::timeout&&std::chrono::steady_clock::now()>=deadline)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
    if(stop)return E_ABORT;
    flags=flags_;timestamp=timestamp_;sample=sample_;return hr_;
  }
 private:
  std::mutex mu_;std::condition_variable wake_;bool ready_=false;HRESULT hr_=E_PENDING;DWORD flags_=0;LONGLONG timestamp_=0;Microsoft::WRL::ComPtr<IMFSample> sample_;
};
}

