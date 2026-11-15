#pragma once
// Measurement-only snapshots: no process attachment, no audio and no GUI input.
#include <windows.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace resource_meter {
inline std::filesystem::path Output() {
  wchar_t path[32768]{};
  auto n=GetEnvironmentVariableW(L"HARMONY_RESOURCE_REPORT",path,32768);
  if(!n || n>=32768)return {};
  auto result=std::filesystem::path(path);return result.is_absolute()?result:std::filesystem::path{};
}
inline uint64_t Time(FILETIME t) {return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
inline std::string Narrow(std::wstring const& w) {
  auto n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
  std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),s.data(),n,nullptr,nullptr);return s;
}
inline std::map<std::string,unsigned> HandleKinds() {
  // ProcessHandleInformation enumerates this process only; query types, never object names.
  struct Entry {HANDLE value;ULONG_PTR handles,pointers;ULONG access,type,attributes,reserved;};
  struct Header {ULONG_PTR count,reserved;Entry entries[1];};
  using Query=LONG(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
  auto processQuery=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess"));
  auto objectQuery=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryObject"));
  std::map<std::string,unsigned> result;if(!processQuery || !objectQuery)return result;
  std::vector<unsigned char> data(65536);ULONG needed=0;LONG status=processQuery(GetCurrentProcess(),51,data.data(),ULONG(data.size()),&needed);
  if(status<0 && needed>data.size()){data.resize(needed+4096);status=processQuery(GetCurrentProcess(),51,data.data(),ULONG(data.size()),&needed);}
  if(status<0)return result;auto* header=reinterpret_cast<Header*>(data.data());
  struct Unicode {USHORT length,maximum;PWSTR buffer;};
  for(size_t i=0;i<header->count;++i){
    alignas(void*) unsigned char type[2048]{};
    if(objectQuery(header->entries[i].value,2,type,sizeof type,nullptr)>=0){auto* name=reinterpret_cast<Unicode*>(type);++result[Narrow(std::wstring(name->buffer,name->length/sizeof(wchar_t)))];}
  }
  return result;
}
inline void Mark(char const* stage) {
  auto path=Output();if(path.empty())return;
  static std::mutex mutex;std::lock_guard lock(mutex);
  PROCESS_MEMORY_COUNTERS_EX m{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof m);
  DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);
  FILETIME c{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);
  std::map<std::string,unsigned> groups;
  using Next=LONG(NTAPI*)(HANDLE,HANDLE,ACCESS_MASK,ULONG,ULONG,PHANDLE);
  using Query=LONG(NTAPI*)(HANDLE,int,PVOID,ULONG,PULONG);
  auto next=reinterpret_cast<Next>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtGetNextThread"));
  auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread"));
  HANDLE previous=nullptr,current=nullptr;
  while(next && next(GetCurrentProcess(),previous,THREAD_QUERY_INFORMATION,0,0,&current)==0){
    if(previous)CloseHandle(previous);previous=current;
    PWSTR desc=nullptr;std::string name;
    if(SUCCEEDED(GetThreadDescription(current,&desc))&&desc){name=Narrow(desc);LocalFree(desc);}
    if(name.empty()){
      void* start=nullptr;HMODULE mod=nullptr;wchar_t module[32768]{};
      if(query && query(current,9,&start,sizeof start,nullptr)==0 && start &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(start),&mod) && GetModuleFileNameW(mod,module,32768))
        name=Narrow(std::filesystem::path(module).filename().wstring());
      else name="unattributed";
    }
    ++groups[name];
  }
  if(previous)CloseHandle(previous);
  DWORD foregroundOwner=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundOwner);
  std::ofstream out(path,std::ios::app);
  out<<"{\"stage\":"<<std::quoted(stage)<<",\"pid\":"<<GetCurrentProcessId()<<",\"tick_ms\":"<<GetTickCount64()
     <<",\"private_bytes\":"<<m.PrivateUsage<<",\"working_set_bytes\":"<<m.WorkingSetSize
     <<",\"foreground_owned\":"<<(foregroundOwner==GetCurrentProcessId())<<",\"handles\":"<<handles<<",\"cpu_ms\":"<<double(Time(k)+Time(u))/10000<<",\"threads\":{";
  bool first=true;for(auto const& [name,count]:groups){if(!first)out<<',';first=false;out<<std::quoted(name)<<':'<<count;}out<<"},\"handle_types\":{";
  first=true;for(auto const& [name,count]:HandleKinds()){if(!first)out<<',';first=false;out<<std::quoted(name)<<':'<<count;}out<<"}}\n";
}
}
