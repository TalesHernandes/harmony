#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include "../../native/src/net.h"
namespace app {
inline std::string TestAudioDevice() {
  // The selftest thread may not have joined COM yet; without it the
  // enumerator cannot be created and CABLE looks absent.
  HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  struct Cleanup{HRESULT hr;~Cleanup(){if(SUCCEEDED(hr))CoUninitialize();}} cleanup{apartment};
  if(FAILED(apartment)&&apartment!=RPC_E_CHANGED_MODE)return {};
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;Microsoft::WRL::ComPtr<IMMDeviceCollection> list;
  if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))||FAILED(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&list)))return {};
  UINT count=0;list->GetCount(&count);
  for(UINT i=0;i<count;++i){Microsoft::WRL::ComPtr<IMMDevice> device;Microsoft::WRL::ComPtr<IPropertyStore> props;PROPVARIANT name{};if(FAILED(list->Item(i,&device)))continue;
    bool match=SUCCEEDED(device->OpenPropertyStore(STGM_READ,&props))&&SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&name))&&name.vt==VT_LPWSTR&&std::wstring(name.pwszVal).find(L"CABLE Input")!=std::wstring::npos;
    PropVariantClear(&name);if(match){LPWSTR id=nullptr;if(FAILED(device->GetId(&id)))return {};auto result=net::Narrow(id);CoTaskMemFree(id);return result;}}
  return {};
}
}
