// Read actual SDK device formats. No window titles, account data or device IDs.
#include <windows.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "../../native/src/camera-reader.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include "../../native/src/json.h"
using Microsoft::WRL::ComPtr;
static bool Save(const std::filesystem::path& path,const json::Value& value) {
  auto pending=path;pending+=L".pending";
  std::ofstream file(pending,std::ios::binary);file<<value.Dump();file.close();
  return file&&MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
static std::string Utf8(const wchar_t* s) {
  int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);
  std::string out(n,0); WideCharToMultiByte(CP_UTF8,0,s,-1,out.data(),n,nullptr,nullptr); out.pop_back(); return out;
}
static std::string Attribute(IMFAttributes* a, REFGUID key) {
  WCHAR* s=nullptr; UINT32 n=0; if(FAILED(a->GetAllocatedString(key,&s,&n))) return "não encontrado";
  auto result=Utf8(s); CoTaskMemFree(s); return result;
}
int wmain(int argc,wchar_t** argv) {
  if(argc<2||argc>4) return 2;
  bool test=argc>=3&&std::wstring(argv[2])==L"--test-camera";
  int cameraIndex=argc==4?_wtoi(argv[3]):-1;bool failed=false;
  CoInitializeEx(nullptr,COINIT_MULTITHREADED); SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto result=json::Value::Object(); result["monitors"]=json::Value::Array(); result["cameras"]=json::Value::Array();result["adapters"]=json::Value::Array();
  ComPtr<IDXGIFactory1> factory; HRESULT dxgi=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  result["dxgi_hresult"]=int64_t(uint32_t(dxgi));
  if(SUCCEEDED(dxgi)) for(UINT a=0;;++a) {
    ComPtr<IDXGIAdapter1> adapter; if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    auto card=json::Value::Object();card["index"]=int(a);card["luid_high"]=int64_t(ad.AdapterLuid.HighPart);card["luid_low"]=int64_t(ad.AdapterLuid.LowPart);card["name"]=Utf8(ad.Description);card["software"]=bool(ad.Flags&DXGI_ADAPTER_FLAG_SOFTWARE);result["adapters"].Push(card);
    for(UINT o=0;;++o) {
      ComPtr<IDXGIOutput> output; if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC d{}; if(FAILED(output->GetDesc(&d))||!d.AttachedToDesktop) continue;
      auto v=json::Value::Object(); v["adapter_index"]=int(a); v["adapter"]=Utf8(ad.Description); v["name"]=Utf8(d.DeviceName);
      v["width"]=int(d.DesktopCoordinates.right-d.DesktopCoordinates.left); v["height"]=int(d.DesktopCoordinates.bottom-d.DesktopCoordinates.top);
      v["rotation"]=int(d.Rotation); UINT x=0,y=0;
      if(SUCCEEDED(GetDpiForMonitor(d.Monitor,MDT_EFFECTIVE_DPI,&x,&y))) { v["dpi_x"]=int(x); v["dpi_y"]=int(y); v["scale"]=double(x)/96; }
      else v["scale"]="não encontrado";
      DEVMODEW mode{}; mode.dmSize=sizeof(mode);
      if(EnumDisplaySettingsW(d.DeviceName,ENUM_CURRENT_SETTINGS,&mode)) v["refresh_hz"]=int(mode.dmDisplayFrequency);
      else v["refresh_hz"]="não encontrado";
      result["monitors"].Push(v);
    }
  }
  HRESULT mf=MFStartup(MF_VERSION); result["mf_hresult"]=int64_t(uint32_t(mf));
  if(SUCCEEDED(mf)) {
    ComPtr<IMFAttributes> attrs; MFCreateAttributes(&attrs,1); attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices=nullptr; UINT32 count=0; HRESULT hr=MFEnumDeviceSources(attrs.Get(),&devices,&count); result["camera_enum_hresult"]=int64_t(uint32_t(hr));
    for(UINT32 i=0;i<count;++i) {
      if(cameraIndex>=0&&int(i)!=cameraIndex){devices[i]->Release();continue;}
      if(test&&cameraIndex<0) {
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        auto childOutput=std::filesystem::path(argv[1]);childOutput+=L".camera-"+std::to_wstring(i)+L".json";
        if(!Save(childOutput,json::Value::Object()))return 1;
        auto command=L"\""+std::wstring(executable)+L"\" \""+childOutput.wstring()+L"\" --test-camera "+std::to_wstring(i);
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        bool timeout=false;DWORD code=1;
        if(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) {
          timeout=WaitForSingleObject(process.hProcess,30000)!=WAIT_OBJECT_0;
          if(timeout){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,1000);}
          GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        }
        std::ifstream file(childOutput,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});auto child=json::Parse(bytes);
        const auto cameras=child?(*child)["cameras"]:json::Value::Array();
        auto camera=cameras.Size()?cameras[0]:json::Value::Object();
        camera["name"]=Attribute(devices[i],MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);camera["deadline_seconds"]=30;camera["timed_out"]=timeout;camera["passed"]=!timeout&&code==0;
        if(timeout||code!=0)failed=true;result["cameras"].Push(camera);devices[i]->Release();
        if(!Save(std::filesystem::path(argv[1]),result))return 1;
        continue;
      }
      auto v=json::Value::Object(); v["name"]=Attribute(devices[i],MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME); v["formats"]=json::Value::Array();
      ComPtr<IMFMediaSource> source; hr=devices[i]->ActivateObject(IID_PPV_ARGS(&source)); v["open_hresult"]=int64_t(uint32_t(hr));
      if(FAILED(hr)&&test)failed=true;
      if(SUCCEEDED(hr)) {
        auto callback=Microsoft::WRL::Make<camera::ReaderCallback>();
        ComPtr<IMFAttributes> readOptions; MFCreateAttributes(&readOptions,1);readOptions->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);readOptions->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get());
        ComPtr<IMFSourceReader> reader; hr=MFCreateSourceReaderFromMediaSource(source.Get(),readOptions.Get(),&reader);
        v["reader_hresult"]=int64_t(uint32_t(hr));
        if(SUCCEEDED(hr)) for(DWORD t=0;;++t) {
          ComPtr<IMFMediaType> type; if(FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,t,&type))) break;
          GUID subtype{}; type->GetGUID(MF_MT_SUBTYPE,&subtype); wchar_t guid[40]{}; StringFromGUID2(subtype,guid,40);
          auto f=json::Value::Object(); f["subtype"]=subtype==MFVideoFormat_MJPG?"MJPEG":subtype==MFVideoFormat_NV12?"NV12":subtype==MFVideoFormat_YUY2?"YUY2":Utf8(guid);
          UINT32 w=0,h=0,n=0,d=0; MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h); MFGetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,&n,&d);
          f["width"]=int(w); f["height"]=int(h); f["fps_numerator"]=int(n); f["fps_denominator"]=int(d);
          if(test) {
            v["pending_format"]=f;
            auto checkpoint=result;checkpoint["cameras"].Push(v);if(!Save(std::filesystem::path(argv[1]),checkpoint))return 1;
            HRESULT opened=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.Get());
            ComPtr<IMFMediaType> rgb;MFCreateMediaType(&rgb);rgb->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);rgb->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
            if(SUCCEEDED(opened))opened=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,rgb.Get());
            ComPtr<IMFSample> sample;DWORD flags=0;LONGLONG timestamp=0;
            std::atomic<bool> stop{false};
            if(SUCCEEDED(opened))opened=callback->Read(reader.Get(),stop,flags,timestamp,sample);
            DWORD bytes=0;if(sample)sample->GetTotalLength(&bytes);
            f["test_hresult"]=int64_t(uint32_t(opened));f["rgb32_sample_bytes"]=int64_t(bytes);f["passed"]=SUCCEEDED(opened)&&bytes>=uint64_t(w)*h*4;if(!f["passed"].Bool())failed=true;
          }
          v["formats"].Push(f);
          if(test) {auto checkpoint=result;checkpoint["cameras"].Push(v);if(!Save(std::filesystem::path(argv[1]),checkpoint))return 1;}
        }
        reader.Reset(); source->Shutdown();
      }
      devices[i]->ShutdownObject(); devices[i]->Release(); result["cameras"].Push(v);
    }
    CoTaskMemFree(devices); MFShutdown();
  }
  auto sound=json::Value::Object(); ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> endpoint; ComPtr<IAudioClient> client;
  HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
  if(SUCCEEDED(hr)) hr=enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&endpoint);
  if(SUCCEEDED(hr)) {
    ComPtr<IPropertyStore> props; PROPVARIANT name{};
    if(SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ,&props))&&SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&name))&&name.vt==VT_LPWSTR) sound["name"]=Utf8(name.pwszVal);
    PropVariantClear(&name); hr=endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf()));
  }
  WAVEFORMATEX* format=nullptr; if(SUCCEEDED(hr)) hr=client->GetMixFormat(&format); sound["hresult"]=int64_t(uint32_t(hr));
  if(SUCCEEDED(hr)) {
    sound["rate"]=int(format->nSamplesPerSec); sound["channels"]=int(format->nChannels); sound["bits"]=int(format->wBitsPerSample); sound["format_tag"]=int(format->wFormatTag);
    if(format->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&format->cbSize>=22) { auto ex=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format); sound["valid_bits"]=int(ex->Samples.wValidBitsPerSample); sound["channel_mask"]=int64_t(ex->dwChannelMask); wchar_t guid[40]{}; StringFromGUID2(ex->SubFormat,guid,40); sound["subformat"]=Utf8(guid); }
    CoTaskMemFree(format);
  } else sound["format"]="não encontrado";
  result["output"]=sound;
  result["session_id"]=int([]{DWORD id=0; ProcessIdToSessionId(GetCurrentProcessId(),&id); return id;}());
  result["foreground_available"]=GetForegroundWindow()!=nullptr;
  return Save(std::filesystem::path(argv[1]),result)&&!failed?0:1;
}
