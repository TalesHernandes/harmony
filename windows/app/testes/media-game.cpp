#include "../src/test-process-guard.h"
// Separate test process: real HWND at 60fps and real WASAPI output.
#include <windows.h>
#include <cwchar>
#include <cstdlib>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include "../../native/src/json.h"
#include "../../native/src/sync-probe.h"
#include "../../native/gpu/cadence.h"
using Microsoft::WRL::ComPtr;
std::atomic<bool> stopped=false;
std::atomic<int> soundReady{0};
int requestedRate=0,requestedChannels=0;bool audioOnly=false;
static bool Sound() {
  auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  struct Cleanup {HRESULT hr;~Cleanup(){if(SUCCEEDED(hr))CoUninitialize();}} cleanup{apartment};
  ComPtr<IMMDeviceEnumerator> enumerator;ComPtr<IMMDevice> device;ComPtr<IAudioClient> client;ComPtr<IAudioRenderClient> render;
  if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))) return false;
  ComPtr<IMMDeviceCollection> endpoints;
  if(FAILED(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&endpoints)))return false;
  UINT count=0;endpoints->GetCount(&count);
  for(UINT i=0;i<count;++i) {
    ComPtr<IMMDevice> candidate;ComPtr<IPropertyStore> props;PROPVARIANT name{};endpoints->Item(i,&candidate);
    if(SUCCEEDED(candidate->OpenPropertyStore(STGM_READ,&props))&&SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&name))&&name.vt==VT_LPWSTR&&std::wstring(name.pwszVal).find(L"CABLE Input")!=std::wstring::npos)device=candidate;
    PropVariantClear(&name);if(device)break;
  }
  if(!device||FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf()))))return false;
  WAVEFORMATEX* format=nullptr;if(FAILED(client->GetMixFormat(&format)))return false;
  WAVEFORMATEXTENSIBLE forced{};
  if(requestedRate){CoTaskMemFree(format);forced.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;forced.Format.nChannels=WORD(requestedChannels);forced.Format.nSamplesPerSec=requestedRate;forced.Format.wBitsPerSample=16;forced.Format.nBlockAlign=WORD(requestedChannels*2);forced.Format.nAvgBytesPerSec=requestedRate*forced.Format.nBlockAlign;forced.Format.cbSize=22;forced.Samples.wValidBitsPerSample=16;forced.dwChannelMask=requestedChannels==2?KSAUDIO_SPEAKER_STEREO:requestedChannels==6?KSAUDIO_SPEAKER_5POINT1_SURROUND:KSAUDIO_SPEAKER_7POINT1_SURROUND;forced.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;format=&forced.Format;}
  auto freeFormat=[&]{if(!requestedRate)CoTaskMemFree(format);};
  if(FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,format,nullptr))) {freeFormat();return false;}
  UINT32 capacity=0;client->GetBufferSize(&capacity);client->GetService(IID_PPV_ARGS(&render));if(FAILED(client->Start())){freeFormat();return false;} soundReady=1;
  bool floating=format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT||(format->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  uint64_t rendered=0;
  bool beepOn=false;const bool probeEnabled=sync_probe::Enabled();
  while(!stopped) {
    UINT32 padding=0;if(FAILED(client->GetCurrentPadding(&padding)))break;UINT32 frames=capacity-padding;
    BYTE* bytes=nullptr;if(frames&&SUCCEEDED(render->GetBuffer(frames,&bytes))) {
      double start=double(GetTickCount64())+double(padding)*1000/format->nSamplesPerSec;
      std::vector<json::Value> probeEvents;
      for(UINT32 i=0;i<frames;++i) {
        double time=start+double(i)*1000/format->nSamplesPerSec;
        // Advance the oscillator by samples, never by coarse wall-clock ticks.
        double phase=double((rendered+i)%format->nSamplesPerSec)*1000/format->nSamplesPerSec;
        const bool beep=std::fmod(time,2000)<100;
        if(probeEnabled && !audioOnly && beep && !beepOn) {
          auto e=sync_probe::Event("beep");e["event_id"]=int64_t(time/2000);
          e["scheduled_tick_ms"]=time;e["sample_in_buffer"]=int(i);
          e["padding_frames"]=int(padding);e["sample_rate"]=int(format->nSamplesPerSec);
          probeEvents.push_back(std::move(e));
        }
        beepOn=beep;
        double sample=(audioOnly||beep) ? std::sin(phase*6.283185307179586)*0.15 : 0;
        for(WORD channel=0;channel<format->nChannels;++channel) {
          BYTE* at=bytes+i*format->nBlockAlign+channel*(format->wBitsPerSample/8);
          double value=audioOnly&&int(time/1000)%format->nChannels!=channel?0:sample;
          if(floating&&format->wBitsPerSample==32) *reinterpret_cast<float*>(at)=float(value);
          else if(format->wBitsPerSample==16) *reinterpret_cast<int16_t*>(at)=int16_t(value*32767);
          else if(format->wBitsPerSample==32) *reinterpret_cast<int32_t*>(at)=int32_t(value*2147483647);
          else memset(at,0,format->wBitsPerSample/8);
        }
      }
      const auto released=render->ReleaseBuffer(frames,0);
      for(auto& e:probeEvents) {e["submitted_wall_ms"]=sync_probe::WallMs();e["submitted"]=SUCCEEDED(released);sync_probe::Record(std::move(e));}
      rendered+=frames;
    }
    Sleep(2);
  }
  client->Stop();freeFormat();return true;
}

static json::Value RectJson(const RECT& r) {
  auto v=json::Value::Object();v["left"]=int(r.left);v["top"]=int(r.top);v["right"]=int(r.right);v["bottom"]=int(r.bottom);return v;
}
static json::Value MonitorJson(HMONITOR handle) {
  MONITORINFOEXW m{};m.cbSize=sizeof(m);auto v=json::Value::Object();
  if(!GetMonitorInfoW(handle,&m))return v;
  char name[128]{};WideCharToMultiByte(CP_UTF8,0,m.szDevice,-1,name,sizeof(name),nullptr,nullptr);
  v["device"]=name;v["primary"]=(m.dwFlags&MONITORINFOF_PRIMARY)!=0;v["rect"]=RectJson(m.rcMonitor);v["work_rect"]=RectJson(m.rcWork);
  DEVMODEW mode{};mode.dmSize=sizeof(mode);v["refresh_hz"]="não encontrado";
  if(EnumDisplaySettingsW(m.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1)v["refresh_hz"]=int(mode.dmDisplayFrequency);
  return v;
}
static json::Value WindowJson(HWND window) {
  auto v=MonitorJson(MonitorFromWindow(window,MONITOR_DEFAULTTONULL));RECT r{};
  v["hwnd"]=int64_t(reinterpret_cast<uintptr_t>(window));
  if(GetWindowRect(window,&r))v["window_rect"]=RectJson(r);
  return v;
}
struct TitleSearch {const wchar_t* title;HWND window=nullptr;int count=0;};
static BOOL CALLBACK FindTitle(HWND w,LPARAM data) {
  auto& s=*reinterpret_cast<TitleSearch*>(data);wchar_t title[1024]{};GetWindowTextW(w,title,1024);
  if(IsWindowVisible(w)&&wcsstr(title,s.title)){s.window=w;++s.count;}return TRUE;
}
static BOOL CALLBACK ListMonitor(HMONITOR h,HDC,LPRECT,LPARAM data) {
  reinterpret_cast<json::Value*>(data)->Push(MonitorJson(h));return TRUE;
}

int wmain(int argc,wchar_t** argv) {
  if(!app::GuardTestParent())return 3;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if(argc==3&&std::wstring(argv[2])==L"--monitors") {
    auto list=json::Value::Array();if(!EnumDisplayMonitors(nullptr,nullptr,ListMonitor,reinterpret_cast<LPARAM>(&list)))return 1;
    std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<list.Dump();return out?0:1;
  }
  if(argc==8&&std::wstring(argv[2])==L"--place-title") {
    TitleSearch search{argv[3]};
    for(int retry=0;retry<20;++retry){search.window=nullptr;search.count=0;EnumWindows(FindTitle,reinterpret_cast<LPARAM>(&search));if(search.count==1)break;Sleep(50);}
    if(search.count!=1)return 1;
    if(!SetWindowPos(search.window,nullptr,_wtoi(argv[4]),_wtoi(argv[5]),_wtoi(argv[6]),_wtoi(argv[7]),SWP_NOACTIVATE|SWP_NOZORDER))return 1;
    Sleep(150);auto value=WindowJson(search.window);
    std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<value.Dump();return out?0:1;
  }
  if(argc==4&&std::wstring(argv[2])==L"--inspect-window") {
    HWND window=reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[3],nullptr,10)));
    if(!IsWindow(window))return 1;
    std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<WindowJson(window).Dump();return out?0:1;
  }
  if(argc!=2&&argc!=5)return 2;
  if(argc==5){if(std::wstring(argv[2])!=L"--audio-only")return 2;audioOnly=true;requestedRate=_wtoi(argv[3]);requestedChannels=_wtoi(argv[4]);if((requestedRate!=44100&&requestedRate!=48000&&requestedRate!=96000&&requestedRate!=192000)||(requestedChannels!=2&&requestedChannels!=6&&requestedChannels!=8))return 2;}
  WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Harmony.W4.Game.Fixture";cls.lpfnWndProc=[](HWND h,UINT m,WPARAM w,LPARAM l)->LRESULT {if(m==WM_CLOSE){stopped=true;DestroyWindow(h);return 0;}return DefWindowProcW(h,m,w,l);};RegisterClassW(&cls);
  int originX=32,originY=100,windowWidth=1920,windowHeight=1080;
  {wchar_t value[128]{};
    if(GetEnvironmentVariableW(L"W4_WINDOW_RECT",value,128)>0) {
      if(swscanf_s(value,L"%d,%d,%d,%d",&originX,&originY,&windowWidth,&windowHeight)!=4||windowWidth<=0||windowHeight<=0)return 2;
    } else if(GetEnvironmentVariableW(L"W4_ORIGIN",value,128)>0) {
      // Preserve standalone fixture callers; the screen harness always supplies a rectangle.
      swscanf_s(value,L"%d,%d",&originX,&originY);
    }
  }
  HWND window=CreateWindowExW(0,cls.lpszClassName,L"Harmony W4 game fixture",WS_OVERLAPPEDWINDOW,originX,originY,windowWidth,windowHeight,nullptr,nullptr,cls.hInstance,nullptr);
  if(!window)return 1;if(!audioOnly)ShowWindow(window,SW_SHOWNOACTIVATE);
  if(!SetWindowPos(window,nullptr,originX,originY,windowWidth,windowHeight,SWP_NOACTIVATE|SWP_NOZORDER)){DestroyWindow(window);return 1;}
  std::thread sound([]{if(!Sound())soundReady=-1;});
  auto deadline=GetTickCount64()+5000;while(soundReady==0&&GetTickCount64()<deadline)Sleep(10);
  auto result=json::Value::Object();result["monitor"]=WindowJson(window);result["audio_ready"]=soundReady==1;result["audio_device"]=soundReady==1?"CABLE Input":"não encontrado";result["audio_failure"]=soundReady==1?"":"CABLE Input unavailable or initialization failed";result["hwnd"]=int64_t(reinterpret_cast<uintptr_t>(window));result["pid"]=int64_t(GetCurrentProcessId());result["foreground_available"]=GetForegroundWindow()!=nullptr;
  {std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<result.Dump();}
  if(soundReady!=1){stopped=true;sound.join();DestroyWindow(window);return 1;}
  if(audioOnly){while(soundReady==1)Sleep(100);stopped=true;sound.join();return 1;}
  timeBeginPeriod(1);uint64_t frame=0,slot=0;LARGE_INTEGER frequency,start;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
  HANDLE wake=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
  if(!wake)wake=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  if(!wake){stopped=true;sound.join();timeEndPeriod(1);return 1;}
  result["clock"]="QPC absolute deadlines; high-resolution waitable timer; single GDI blit";
  MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
  DEVMODEW mode{};mode.dmSize=sizeof(mode);
  result["monitor_refresh_hz"]="não encontrado";
  if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)&&
      EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1)
    result["monitor_refresh_hz"]=int(mode.dmDisplayFrequency);
  result["cadence_samples"]=json::Value::Array();
  result["sync_events"]=json::Value::Array();
  const bool probeEnabled=sync_probe::Enabled();bool flashOn=false;
  int64_t lastDraw=0, sampleStart=start.QuadPart;
  double maxGap=0;int drawn=0;uint64_t skipped=0;
  HDC surface=GetDC(window),back=CreateCompatibleDC(surface);
  RECT clientRect;GetClientRect(window,&clientRect);
  HBITMAP bitmap=CreateCompatibleBitmap(surface,clientRect.right,clientRect.bottom);
  auto previous=SelectObject(back,bitmap);ReleaseDC(window,surface);
  while(!stopped) {
    MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(stopped)break;
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    auto previousSlot=slot;
    if(!gpu::GameDue(now.QuadPart,frequency.QuadPart,start.QuadPart,slot)) {
      LARGE_INTEGER due;due.QuadPart=-std::max<int64_t>(1,(start.QuadPart+int64_t((slot*frequency.QuadPart+59)/60)-now.QuadPart)*10000000/frequency.QuadPart);
      SetWaitableTimer(wake,&due,0,nullptr,nullptr,FALSE);
      MsgWaitForMultipleObjects(1,&wake,FALSE,100,QS_ALLINPUT);continue;
    }
    skipped+=slot-previousSlot-1;
    if(lastDraw)maxGap=std::max(maxGap,double(now.QuadPart-lastDraw)*1000/frequency.QuadPart);
    lastDraw=now.QuadPart;++drawn;
    HDC dc=back;RECT r=clientRect;auto color=CreateSolidBrush(RGB(BYTE(frame%180+40),50,100));FillRect(dc,&r,color);DeleteObject(color);
    const auto flashTick=GetTickCount64();const bool flash=flashTick%2000<100;
    auto flashEvent=json::Value();
    if(probeEnabled && flash && !flashOn) {flashEvent=sync_probe::Event("flash");flashEvent["event_id"]=int64_t(flashTick/2000);flashEvent["frame"]=int64_t(frame);flashEvent["tick_ms"]=int64_t(flashTick);}
    flashOn=flash;
    RECT band{0,0,r.right,80};auto b=CreateSolidBrush(flash?RGB(255,255,255):RGB(0,0,0));FillRect(dc,&band,b);DeleteObject(b);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));wchar_t text[96];swprintf_s(text,L"Frame %llu — W4 real WGC + process audio",static_cast<unsigned long long>(frame));TextOutW(dc,100,150,text,int(wcslen(text)));
    // The moving bar lets a stalled receiver distinguish a new frame.
    RECT moving{LONG((frame*12)%1700),300,LONG((frame*12)%1700+100),800};auto white=CreateSolidBrush(RGB(220,220,220));FillRect(dc,&moving,white);DeleteObject(white);
    // Publish a complete frame once; WGC must not see intermediate GDI bands.
    surface=GetDC(window);BitBlt(surface,0,0,r.right,r.bottom,back,0,0,SRCCOPY);GdiFlush();ReleaseDC(window,surface);++frame;
    if(!flashEvent.IsNull()) {flashEvent["submitted_wall_ms"]=sync_probe::WallMs();sync_probe::Record(std::move(flashEvent));}
    if(now.QuadPart-sampleStart>=frequency.QuadPart) {
      if(probeEnabled) {auto batch=sync_probe::Drain();for(auto& e:batch["events"].Elements())result["sync_events"].Push(e);result["sync_events_lost"]=result["sync_events_lost"].Int()+batch["lost"].Int();}
      auto sample=json::Value::Object();double elapsed=double(now.QuadPart-sampleStart)/frequency.QuadPart;
      sample["elapsed_seconds"]=double(now.QuadPart-start.QuadPart)/frequency.QuadPart;
      sample["draws_per_second"]=drawn/elapsed;sample["max_draw_gap_ms"]=maxGap;
      sample["skipped_slots"]=int64_t(skipped);result["cadence_samples"].Push(std::move(sample));
      {std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<result.Dump();}
      sampleStart=now.QuadPart;drawn=0;skipped=0;maxGap=0;
    }
  }
  SelectObject(back,previous);DeleteObject(bitmap);DeleteDC(back);CloseHandle(wake);
  sound.join();timeEndPeriod(1);return 0;
}

