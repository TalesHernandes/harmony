#include "camera.h"
#include "common_video/include/video_frame_buffer_pool.h"
#include "resource-usage.h"
#include "camera-reader.h"
#include "camera-policy.h"
#include "camera-conversion.h"
#include "net.h"
#include "log.h"
#include "perf.h"
#include "api/make_ref_counted.h"
#include "api/video/adapted_video_track_source.h"
#include "api/video/i420_buffer.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include "libyuv.h"
#include <atomic>
#include <thread>
#include <mutex>
using Microsoft::WRL::ComPtr;
namespace camera {
namespace {
std::string String(IMFAttributes* attributes,REFGUID key) {
  wchar_t* text=nullptr; UINT32 size=0;
  if(FAILED(attributes->GetAllocatedString(key,&text,&size))) return {};
  auto result=net::Narrow(text); CoTaskMemFree(text); return result;
}
class DriverSource : public webrtc::AdaptedVideoTrackSource {
 public:
  explicit DriverSource(std::string id) : thread_([this,id]{Run(id);}) {}
  ~DriverSource() override { stop_=true; ComPtr<IMFSourceReader> reader; {std::lock_guard lock(mu_);reader=reader_;} if(thread_.joinable()) thread_.join(); }
  SourceState state() const override { return ended_?kEnded:kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return false; }
  std::optional<bool> needs_denoising() const override { return false; }
 private:
  void Run(std::string id) {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED); perf::NameThread(L"harmony-camera"); MFStartup(MF_VERSION);
    ComPtr<IMFAttributes> attributes; MFCreateAttributes(&attributes,2);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    auto devices=Devices(); if(id.empty()&&!devices.empty()) id=devices.front().id;
    attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,net::Wide(id).c_str());
    ComPtr<IMFMediaSource> source;
    HRESULT hr=MFCreateDeviceSource(attributes.Get(),&source);
    ComPtr<IMFSourceReader> reader;
    if(FAILED(hr)&&!devices.empty()&&id!=devices.front().id) {attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,net::Wide(devices.front().id).c_str());hr=MFCreateDeviceSource(attributes.Get(),&source);logf("camera selected unavailable; fallback to first available device");}
    auto callback=Microsoft::WRL::Make<ReaderCallback>();
    if(SUCCEEDED(hr)) {
      ComPtr<IMFAttributes> options; MFCreateAttributes(&options,2);
      options->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get());
      options->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE);
      hr=MFCreateSourceReaderFromMediaSource(source.Get(),options.Get(),&reader);
    }
    if(SUCCEEDED(hr)) {
      // Match the web's ideal 1280x720 at 30; retain the camera's aspect ratio.
      double best=1e30; ComPtr<IMFMediaType> chosen;
      for(DWORD i=0;;++i) {
        ComPtr<IMFMediaType> native; if(FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,i,&native))) break;
        UINT32 w=0,h=0,n=0,d=0; MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&w,&h); MFGetAttributeRatio(native.Get(),MF_MT_FRAME_RATE,&n,&d);
        if(!w||!h||!d) continue;
        GUID subtype{};native->GetGUID(MF_MT_SUBTYPE,&subtype);
        double score=FormatScore(w,h,double(n)/d,subtype==MFVideoFormat_NV12?0:subtype==MFVideoFormat_MJPG?1:2);
        if(score<best) {best=score; chosen=native;}
      }
      if(chosen) hr=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,chosen.Get());
      // Raw NV12/YUY2 stay in YUV. MJPEG is decoded by MF directly to NV12.
      GUID nativeSubtype{};if(chosen)chosen->GetGUID(MF_MT_SUBTYPE,&nativeSubtype);
      ComPtr<IMFMediaType> output; MFCreateMediaType(&output); output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE,YUVOutputSubtype(nativeSubtype));
      if(SUCCEEDED(hr)) hr=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,output.Get());
    }
    if(SUCCEEDED(hr)) {
      ComPtr<IMFMediaType> type; reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type);
      UINT32 w=0,h=0; MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h);
      GUID subtype{};type->GetGUID(MF_MT_SUBTYPE,&subtype);
      const bool nv12=subtype==MFVideoFormat_NV12;
      LONG stride=LONG(MFGetAttributeUINT32(type.Get(),MF_MT_DEFAULT_STRIDE,w*(nv12?1:2)));
      logf("camera actual width=%u height=%u converted=%s stride=%ld requested_fps=30",w,h,nv12?"NV12":"YUY2",stride);
      {std::lock_guard lock(mu_);reader_=reader;} int64_t next=0;
      while(!stop_) {
        DWORD flags=0; LONGLONG timestamp=0; ComPtr<IMFSample> sample;
        hr=callback->Read(reader.Get(),stop_,flags,timestamp,sample);
        if(FAILED(hr)||(flags&MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if(!sample) continue;
        if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) break;
        int64_t now=perf::Stage::Now()/1000;
        // The W1 cadence tolerates capture jitter without dropping every
        // second frame from a camera delivering just below its nominal rate.
        if(!CameraDue(now,next))continue;
        static perf::Stage& stageCameraCapture = perf::Get("camera.capture");
        auto timer=stageCameraCapture.Begin();
        ComPtr<IMFMediaBuffer> buffer; if(FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data=nullptr; DWORD length=0; if(FAILED(buffer->Lock(&data,nullptr,&length))) continue;
        auto frame=(nv12||subtype==MFVideoFormat_YUY2)?ConvertYUV(data,length,w,h,stride,nv12):nullptr;
        buffer->Unlock(); if(!frame) {timer.Drop();continue;}
        timer.Done(); OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(frame).set_timestamp_us(now).set_rotation(webrtc::kVideoRotation_0).build());
      }
      {std::lock_guard lock(mu_);reader_.Reset();}
    }
    logf("camera closed hresult=0x%08lx",hr); reader.Reset(); if(source) source->Shutdown();
    ended_=true; MFShutdown(); CoUninitialize();
  }
  std::atomic<bool> stop_{false},ended_{false}; std::mutex mu_; ComPtr<IMFSourceReader> reader_; std::thread thread_;
};
}
std::vector<Device> Devices() {
  auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED); MFStartup(MF_VERSION);
  ComPtr<IMFAttributes> attrs; MFCreateAttributes(&attrs,1); attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** list=nullptr; UINT32 count=0; std::vector<Device> devices;
  if(SUCCEEDED(MFEnumDeviceSources(attrs.Get(),&list,&count))) for(UINT32 i=0;i<count;++i) {
    devices.push_back({String(list[i],MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK),String(list[i],MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)}); list[i]->Release();
  }
  CoTaskMemFree(list); MFShutdown(); if(SUCCEEDED(apartment)) CoUninitialize(); return devices;
}
namespace {
struct Packet {uint32_t magic=0x48344341,width=0,height=0;int64_t timestamp=0;};
class PipeSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit PipeSink(HANDLE pipe):pipe_(pipe){}
  void OnFrame(const webrtc::VideoFrame& frame) override {
    auto buffer=frame.video_frame_buffer()->ToI420();Packet header;header.width=buffer->width();header.height=buffer->height();header.timestamp=frame.timestamp_us();
    if(!Write(&header,sizeof(header)))return;
    for(int plane=0;plane<3;++plane){int w=plane?buffer->width()/2:buffer->width(),h=plane?buffer->height()/2:buffer->height();const uint8_t* data=plane==0?buffer->DataY():plane==1?buffer->DataU():buffer->DataV();int stride=plane==0?buffer->StrideY():plane==1?buffer->StrideU():buffer->StrideV();
      if(stride==w) {if(!Write(data,DWORD(w*h)))return;}
      else for(int row=0;row<h;++row)if(!Write(data+row*stride,w))return;
    }
  }
 private:
  bool Write(const void* input,DWORD size) {
    auto* bytes=static_cast<const uint8_t*>(input);
    while(size) {DWORD written=0;if(!WriteFile(pipe_,bytes,size,&written,nullptr)||!written)return false;bytes+=written;size-=written;}
    return true;
  }
  HANDLE pipe_;
};
class Source : public webrtc::AdaptedVideoTrackSource {
 public:
  explicit Source(std::string id):thread_([this,id]{Run(id);}){}
  ~Source() override {stop_=true;if(stopEvent_)SetEvent(stopEvent_);if(thread_.joinable())thread_.join();if(stopEvent_)CloseHandle(stopEvent_);if(readEvent_)CloseHandle(readEvent_);}
  SourceState state() const override {return ended_?kEnded:kLive;}
  bool remote() const override{return false;}
  bool is_screencast() const override{return false;}
  std::optional<bool> needs_denoising() const override{return false;}
 private:
  bool Read(HANDLE pipe,void* output,DWORD size,uint64_t deadline) {
    static auto& requests=perf::Get("camera.pipe.read_requests");requests.Record(0);
    auto* bytes=static_cast<uint8_t*>(output);
    HANDLE completed=readEvent_;
    if(!completed) return false;
    while(size && !stop_) {
      auto now=GetTickCount64();if(now>=deadline)break;
      OVERLAPPED operation{};operation.hEvent=completed;ResetEvent(completed);
      DWORD got=0;BOOL ready=ReadFile(pipe,bytes,size,&got,&operation);
      if(!ready && GetLastError()!=ERROR_IO_PENDING)break;
      if(!ready) {
        HANDLE events[]{stopEvent_,completed};
        auto result=WaitForMultipleObjects(2,events,FALSE,DWORD(deadline-now));
        if(result!=WAIT_OBJECT_0+1) {
          CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&got,TRUE);break;
        }
      }
      if(!GetOverlappedResult(pipe,&operation,&got,TRUE)||!got)break;
      bytes+=got;size-=got;
    }
    return !size;
  }
  void Run(const std::string& id,bool testDefault=false) {
    perf::NameThread(L"harmony-camera-pipe");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE input=nullptr,output=nullptr;
    static std::atomic<unsigned> pipeSerial{0};
    auto name=L"\\\\.\\pipe\\harmony-camera-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++pipeSerial);
    input=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,0,4*1024*1024,3000,nullptr);
    if(input==INVALID_HANDLE_VALUE || !stopEvent_ || !readEvent_){if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);ended_=true;return;}
    output=CreateFileW(name.c_str(),GENERIC_WRITE,0,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    OVERLAPPED connection{};connection.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool connected=output!=INVALID_HANDLE_VALUE && connection.hEvent && (ConnectNamedPipe(input,&connection) || GetLastError()==ERROR_PIPE_CONNECTED);
    if(connection.hEvent)CloseHandle(connection.hEvent);
    if(!connected){if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);CloseHandle(input);ended_=true;return;}
    webrtc::VideoFrameBufferPool pool(false,8);
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    auto command=L"\""+std::wstring(executable)+L"\" --camera-worker "+std::to_wstring(reinterpret_cast<uintptr_t>(output))+L" \""+net::Wide(testDefault?"__harmony_default_frame_test__":id)+L"\"";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    // Inherit only the pipe. Account/socket handles never enter the worker.
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<uint8_t> storage(bytes);STARTUPINFOEXW extended{};extended.StartupInfo=startup;extended.StartupInfo.cb=sizeof(extended);extended.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized=InitializeProcThreadAttributeList(extended.lpAttributeList,1,0,&bytes);
    bool attributes=initialized&&UpdateProcThreadAttribute(extended.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&output,sizeof(output),nullptr,nullptr);
    bool started=attributes&&CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&extended.StartupInfo,&process);
    if(initialized)DeleteProcThreadAttributeList(extended.lpAttributeList);CloseHandle(output);
    if(started){
      resources::CameraProcess(process.hProcess);
      while(!stop_){Packet header;auto deadline=GetTickCount64()+3000;
        if(!Read(input,&header,sizeof(header),deadline))break;
        if(header.magic!=0x48344341||header.width<2||header.height<2||std::max(header.width,header.height)>1280||std::min(header.width,header.height)>720||header.width%2||header.height%2)break;
        auto frame=pool.CreateI420Buffer(header.width,header.height);
        if(!frame)frame=webrtc::I420Buffer::Create(header.width,header.height);
        bool valid=true;
        for(int plane=0;plane<3&&valid;++plane){int w=plane?header.width/2:header.width,h=plane?header.height/2:header.height;uint8_t* data=plane==0?frame->MutableDataY():plane==1?frame->MutableDataU():frame->MutableDataV();int stride=plane==0?frame->StrideY():plane==1?frame->StrideU():frame->StrideV();
          if(stride==w)valid=Read(input,data,DWORD(w*h),deadline);
          else for(int row=0;row<h&&valid;++row)valid=Read(input,data+row*stride,w,deadline);
        }
        if(!valid)break;
        resourceLoad_.Loaded();
        OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(frame).set_timestamp_us(header.timestamp).set_rotation(webrtc::kVideoRotation_0).build());
      }
      // Even a driver stuck in opening, Flush or Shutdown dies with its host.
      TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,1000);resources::CameraProcess(nullptr);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    }
    CloseHandle(input);
    if(!stop_&&!id.empty()&&id!="__harmony_deadline_test__") {logf("camera selected source ended; retry default once");Run("",id=="__harmony_removed_test__");return;}
    ended_=true;logf("camera host ended; opening/read deadline=3000ms");
  }
  resources::Load resourceLoad_{resources::Camera};
  std::atomic<bool> stop_{false},ended_{false};HANDLE stopEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr),readEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);std::thread thread_;
};
}
int WorkerMain(uintptr_t pipe,const std::string& id) {
  if(id=="__harmony_pipe_perf_test__") {
    auto frame=webrtc::I420Buffer::Create(1280,720);
    memset(frame->MutableDataY(),81,frame->StrideY()*720);memset(frame->MutableDataU(),90,frame->StrideU()*360);memset(frame->MutableDataV(),240,frame->StrideV()*360);
    PipeSink sink(reinterpret_cast<HANDLE>(pipe));auto next=std::chrono::steady_clock::now();
    for(int i=0;i<36000;++i) {sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(frame).set_timestamp_us(perf::Stage::Now()/1000).build());next+=std::chrono::microseconds(33333);std::this_thread::sleep_until(next);}
    return 0;
  }
  if(id=="__harmony_deadline_test__"){Sleep(INFINITE);return 1;}
  if(id=="__harmony_removed_test__")return 1;
  if(id=="__harmony_default_frame_test__") {auto frame=webrtc::I420Buffer::Create(404,720);memset(frame->MutableDataY(),16,frame->StrideY()*720);memset(frame->MutableDataU(),128,frame->StrideU()*360);memset(frame->MutableDataV(),128,frame->StrideV()*360);PipeSink sink(reinterpret_cast<HANDLE>(pipe));sink.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(frame).set_timestamp_us(1).build());Sleep(INFINITE);return 1;}
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source=webrtc::make_ref_counted<DriverSource>(id);PipeSink sink(reinterpret_cast<HANDLE>(pipe));webrtc::VideoSinkWants wants;source->AddOrUpdateSink(&sink,wants);
  while(source->state()!=webrtc::MediaSourceInterface::kEnded)Sleep(20);
  source->RemoveSink(&sink);return 0;
}
int DeadlineSelfTest() {
  auto start=GetTickCount64();auto source=webrtc::make_ref_counted<Source>("__harmony_deadline_test__");
  while(source->state()!=webrtc::MediaSourceInterface::kEnded&&GetTickCount64()-start<5000)Sleep(10);
  bool ended=source->state()==webrtc::MediaSourceInterface::kEnded;source=nullptr;
  logf("camera blocked driver deadline test ended=%d elapsed_ms=%llu",ended,GetTickCount64()-start);
  return ended&&GetTickCount64()-start<4500?0:1;
}
int FallbackSelfTest() {
  struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {std::atomic<int> frames{0};void OnFrame(const webrtc::VideoFrame& frame)override{if(frame.width()==404&&frame.height()==720)++frames;}} sink;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source=webrtc::make_ref_counted<Source>("__harmony_removed_test__");webrtc::VideoSinkWants wants;source->AddOrUpdateSink(&sink,wants);
  auto deadline=GetTickCount64()+1500;while(!sink.frames&&GetTickCount64()<deadline)Sleep(10);
  source->RemoveSink(&sink);source=nullptr;return sink.frames?0:1;
}
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> Open(std::string id) { return webrtc::make_ref_counted<Source>(std::move(id)); }
}
