#include "../../native/src/camera.h"
#include "../../native/src/camera-conversion.h"
#include "../../native/src/net.h"
#include "../../native/src/perf.h"
#include <windows.h>
#include <cstdio>
#include <atomic>
int RealCameraPerfTest() {
  if(camera::Devices().empty()){fprintf(stderr,"real camera device not found\n");return 2;}
  struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<int> frames{0},width{0},height{0};
    void OnFrame(const webrtc::VideoFrame& frame)override{width=frame.width();height=frame.height();++frames;}
  } sink;
  auto source=camera::Open("");webrtc::VideoSinkWants wants;source->AddOrUpdateSink(&sink,wants);
  auto deadline=GetTickCount64()+6000;while(sink.frames<5&&GetTickCount64()<deadline)Sleep(10);
  if(sink.frames<5){source->RemoveSink(&sink);fprintf(stderr,"real camera frames not found\n");return 2;}
  perf::TakeSnapshot();double seconds=0,cpu=0;int start=sink.frames;
  for(int i=0;i<5;++i){Sleep(1000);auto snapshot=perf::TakeSnapshot();seconds+=snapshot.seconds;for(auto& row:snapshot.threads)if(row.name=="harmony-camera-pipe")cpu+=row.cpu_percent*snapshot.seconds;printf("%s",perf::Format(snapshot).c_str());}
  int frames=sink.frames-start;source->RemoveSink(&sink);source=nullptr;
  printf("real camera %dx%d frames=%d seconds=%.3f pipe_cpu_core_pct=%.3f\n",int(sink.width),int(sink.height),frames,seconds,cpu/seconds);
  return frames>0?0:2;
}
int PipePerfTest(bool assertBudget) {
  struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<int> frames{0};std::atomic<bool> corrupt{false};
    std::vector<webrtc::scoped_refptr<webrtc::I420BufferInterface>> retained;
    void OnFrame(const webrtc::VideoFrame& video) override {
      auto b=video.video_frame_buffer()->ToI420();
      for(auto const& old:retained)if(old.get()==b.get())corrupt=true;
      if(retained.size()<3)retained.push_back(b);
      if(b->width()!=1280||b->height()!=720) {corrupt=true;return;}
      for(int p=0;p<3;++p) {
        const auto* data=p==0?b->DataY():p==1?b->DataU():b->DataV();
        int stride=p==0?b->StrideY():p==1?b->StrideU():b->StrideV();
        int w=p?640:1280,h=p?360:720;uint8_t expected=p==0?81:p==1?90:240;
        for(int y=0;y<h;++y)if(data[y*stride]!=expected||data[y*stride+w-1]!=expected)corrupt=true;
      }
      ++frames;
    }
  } sink;
  auto source=camera::Open("__harmony_pipe_perf_test__");webrtc::VideoSinkWants wants;source->AddOrUpdateSink(&sink,wants);
  auto deadline=GetTickCount64()+3000;while(sink.frames<5&&GetTickCount64()<deadline)Sleep(10);
  if(sink.frames<5){fprintf(stderr,"pipe producer did not deliver frames\n");return 1;}
  perf::TakeSnapshot();double cpu=0,seconds=0;uint64_t requests=0;int start=sink.frames;
  for(int i=0;i<5;++i) {
    Sleep(1000);auto snapshot=perf::TakeSnapshot();seconds+=snapshot.seconds;
    for(auto& row:snapshot.threads)if(row.name=="harmony-camera-pipe")cpu+=row.cpu_percent*snapshot.seconds;
    for(auto& row:snapshot.stages)if(row.name=="camera.pipe.read_requests")requests+=row.out;
    printf("%s",perf::Format(snapshot).c_str());
  }
  int frames=sink.frames-start;source->RemoveSink(&sink);source=nullptr;
  printf("pipe 1280x720@30 frames=%d seconds=%.3f cpu_core_pct=%.3f read_requests=%llu\n",frames,seconds,cpu/seconds,requests);
  if(sink.corrupt||frames<130||frames>180){fprintf(stderr,"pipe planes and 30 fps must survive transfer\n");return 1;}
  if(assertBudget&&requests>uint64_t(frames+2)*4){fprintf(stderr,"pipe must transfer whole planes, not individual rows\n");return 1;}
  return 0;
}
int wmain(int argc,wchar_t** argv) {
  if(argc==4&&std::wstring(argv[1])==L"--camera-worker")return camera::WorkerMain(_wcstoui64(argv[2],nullptr,10),net::Narrow(argv[3]));
  if(argc==2&&std::wstring(argv[1])==L"--pipe-perf")return PipePerfTest(false);
  if(argc==2&&std::wstring(argv[1])==L"--camera-perf")return RealCameraPerfTest();
  if(camera::YUVOutputSubtype(MFVideoFormat_NV12)!=MFVideoFormat_NV12||camera::YUVOutputSubtype(MFVideoFormat_YUY2)!=MFVideoFormat_YUY2||camera::YUVOutputSubtype(MFVideoFormat_MJPG)!=MFVideoFormat_NV12){fprintf(stderr,"native camera output must stay in YUV\n");return 1;}
  for(auto size:{camera::Size{1280,960},camera::Size{720,1280},camera::Size{1280,720}})for(bool nv12:{true,false}) {
    int stride=size.width*(nv12?1:2)+16;
    std::vector<uint8_t> bytes(stride*(nv12?size.height+size.height/2:size.height),0);
    for(int y=0;y<size.height;++y)for(int x=0;x<size.width;++x) {
      if(nv12)bytes[y*stride+x]=uint8_t(y<size.height/2?200:30);
      else {bytes[y*stride+x*2]=uint8_t(y<size.height/2?200:30);bytes[y*stride+x*2+1]=uint8_t(x%2?240:90);}
    }
    if(nv12)for(int y=0;y<size.height/2;++y)for(int x=0;x<size.width;x+=2){bytes[(size.height+y)*stride+x]=90;bytes[(size.height+y)*stride+x+1]=240;}
    auto expected=camera::WebSize(size.width,size.height);
    auto frame=camera::ConvertYUV(bytes.data(),bytes.size(),size.width,size.height,stride,nv12);
    if(!frame||frame->width()!=expected.width||frame->height()!=expected.height||frame->DataY()[frame->StrideY()*10+10]-frame->DataY()[frame->StrideY()*(frame->height()-11)+10]<100||frame->DataU()[10]!=90||frame->DataV()[10]!=240){fprintf(stderr,"YUV camera preserves aspect, orientation, chroma and padded stride\n");return 1;}
    if(camera::ConvertYUV(bytes.data(),bytes.size()-1,size.width,size.height,stride,nv12)||camera::ConvertYUV(bytes.data(),bytes.size(),size.width,size.height,-stride,nv12)){fprintf(stderr,"YUV camera rejects truncated or invalid planes\n");return 1;}
  }
  for(auto size:{camera::Size{1280,960},camera::Size{720,1280},camera::Size{1280,720}}) {
    std::vector<uint8_t> rgb(size.width*size.height*4);
    for(int y=0;y<size.height;++y)for(int x=0;x<size.width;++x)for(int c=0;c<4;++c)rgb[(y*size.width+x)*4+c]=uint8_t(y<size.height/2?240:20);
    auto expected=camera::WebSize(size.width,size.height);
    for(int sign:{1,-1}) {
      auto frame=camera::ConvertRGB(rgb.data(),rgb.size(),size.width,size.height,sign*size.width*4);
      if(!frame||frame->width()!=expected.width||frame->height()!=expected.height)return 1;
      auto top=frame->DataY()[frame->StrideY()*10+10],bottom=frame->DataY()[frame->StrideY()*(frame->height()-11)+10];
      if((sign==1?top-bottom:bottom-top)<100){fprintf(stderr,"camera conversion preserves orientation\n");return 1;}
    }
    if(camera::ConvertRGB(rgb.data(),rgb.size()-1,size.width,size.height,size.width*4))return 1;
  }
  if(camera::DeadlineSelfTest()){fprintf(stderr,"camera blocked opening has hard deadline\n");return 1;}
  if(camera::FallbackSelfTest()){fprintf(stderr,"camera removed selection falls back to default\n");return 1;}
  auto source=camera::Open("__harmony_deadline_test__");Sleep(100);auto before=GetTickCount64();source=nullptr;
  if(GetTickCount64()-before>1500){fprintf(stderr,"camera stop cancels blocked opening\n");return 1;}
  if(PipePerfTest(true))return 1;
  puts("camera YUV/stride/orientation, batched I420 pipe, blocked opening and cancellation passed");return 0;
}
