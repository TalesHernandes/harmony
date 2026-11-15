#include "verified-launch.h"
#include <iostream>
#include <psapi.h>
namespace fs=std::filesystem;
namespace setup=app::installer;
static void Check(bool ok,const char* label) {if(!ok)throw std::runtime_error(label);std::cout<<"PASS: "<<label<<'\n';}
static void Write(const fs::path& p,const std::string& s){std::ofstream(p,std::ios::binary)<<s;}
static void Time(const fs::path& p,uint64_t value) {
  setup::NativeHandle h(CreateFileW(p.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
  FILETIME t{DWORD(value),DWORD(value>>32)};if(h.value==INVALID_HANDLE_VALUE||!SetFileTime(h.value,nullptr,nullptr,&t))throw std::runtime_error("fixture timestamp failed");
}
static bool Quick(const fs::path& root){bool quick=false;setup::VerifiedNative(root,&quick);return quick;}
static bool Refused(const fs::path& root){try{Quick(root);return false;}catch(...){return true;}}
static json::Value Seed(const fs::path& root,int version,const std::string& content) {
  auto depot=root/(L"app-"+std::to_wstring(version));fs::create_directories(depot);
  Write(depot/L"harmony-nativo.exe",content);
  auto manifest=json::Value::Object();manifest["version"]=version;manifest["complete"]=true;manifest["files"]=json::Value::Array();
  auto file=json::Value::Object();file["path"]="harmony-nativo.exe";file["bytes"]=int(content.size());file["sha256"]=setup::SHA256(depot/L"harmony-nativo.exe");manifest["files"].Push(file);
  Write(depot/L"manifest.json",manifest.Dump());return manifest;
}
static void Select(const fs::path& root,int version){Write(root/L"current.json","{\"version\":"+std::to_string(version)+",\"directory\":\"app-"+std::to_string(version)+"\",\"live\":\"app\"}");}
static void IndependentLive(const fs::path& root){fs::remove(root/L"app/harmony-nativo.exe");fs::copy_file(root/L"app-1/harmony-nativo.exe",root/L"app/harmony-nativo.exe");}
struct Observer {
  PROCESS_INFORMATION process{};setup::NativeHandle ready,release;
  std::wstring name;
  Observer(const fs::path& root,const fs::path& self):
    ready(CreateEventW(nullptr,TRUE,FALSE,(setup::NativeGateName(root)+L"-test-ready").c_str())),
    release(CreateEventW(nullptr,TRUE,FALSE,(setup::NativeGateName(root)+L"-test-release").c_str())) {
    auto id=setup::ReadJSON(root/L"verified-launch.json")["id"].Str();
    auto command=L"\""+self.wstring()+L"\" --observe \""+root.wstring()+L"\" "+net::Wide(id);
    STARTUPINFOW si{sizeof si};
    if(!CreateProcessW(self.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&process))throw std::runtime_error("observer fixture failed");
    if(WaitForSingleObject(ready.value,10000)!=WAIT_OBJECT_0)throw std::runtime_error("observer never opened file");
  }
  void Finish(){SetEvent(release.value);if(WaitForSingleObject(process.hProcess,10000)!=WAIT_OBJECT_0)throw std::runtime_error("observer timeout");}
  ~Observer(){if(WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}CloseHandle(process.hProcess);CloseHandle(process.hThread);}
};
int wmain(int argc,wchar_t** argv) {
  if(argc==4&&std::wstring(argv[1])==L"--observe") {
    fs::path root=argv[2];
    auto hook=[&]{
      setup::NativeHandle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,(setup::NativeGateName(root)+L"-test-ready").c_str()));
      setup::NativeHandle release(OpenEventW(SYNCHRONIZE,FALSE,(setup::NativeGateName(root)+L"-test-release").c_str()));
      setup::NativeHandle cancel(OpenEventW(SYNCHRONIZE,FALSE,(setup::NativeGateName(root)+L"-cancel").c_str()));
      SetEvent(ready.value);HANDLE waits[]{release.value,cancel.value};WaitForMultipleObjects(2,waits,FALSE,10000);
    };
    auto failure=setup::BackgroundNative(root,net::Narrow(argv[3]),hook);
    Write(root/L"observer-result.txt",failure);ExitProcess(0);
  }
  if(argc==3&&std::wstring(argv[1])==L"--warm") {try {setup::VerifiedNative(fs::absolute(argv[2]));return 0;}catch(...){return 1;}}
  if(argc!=2)return 2;
  auto root=fs::absolute(argv[1]);auto self=app::Executable();
  try {
    fs::create_directories(root);Write(root/L".harmony-native-install","");auto manifest=Seed(root,1,"fixture");auto next=Seed(root,2,"updated");
    setup::PromoteNative(root,root/L"app-1",manifest);Select(root,1);IndependentLive(root);
    // The scripts lowercase with ToLowerInvariant; the gate must agree on accents.
    Check(setup::NativeGateName(L"C:\\Users\\\u00C1LVARO \u00C9bano\\Harmony")==L"Local\\harmony-native-observer-c__users_\u00E1lvaro \u00E9bano_harmony","observer gate name lowercases accented capitals like the installer scripts");
    Check(!Quick(root)&&Quick(root),"missing record hashes fully, then metadata opens quickly");
    auto live=root/L"app/harmony-nativo.exe";
    auto original=setup::NativeStat(live);Time(live,original.time+1);
    Check(!Quick(root)&&Quick(root),"one FILETIME tick forces full verification without rounding");
    auto record=setup::ReadJSON(root/L"verified-launch.json");
    Check(record["files"]["harmony-nativo.exe"]["filetime"].Str()==std::to_string(original.time+1),"record stores the complete 64-bit FILETIME as decimal text");
    record["version"]=2;Write(root/L"verified-launch.json",record.Dump());Check(!Quick(root),"another version's record forces full verification");
    Write(root/L"verified-launch.json","{\"version\":");Check(!Quick(root),"truncated record forces full verification");
    fs::remove(root/L"verified-launch.json");Write(root/L"verified-launch.json.part","interrupted");Check(!Quick(root),"interrupted .part never substitutes for a committed record");
    {
      Observer worker(root,self);auto begin=GetTickCount64();Check(Quick(root),"second opening succeeds while background reader holds a file");
      Check(GetTickCount64()-begin<1000,"second opening never waits for observer mutex or install lock");
      PROCESS_MEMORY_COUNTERS memory{sizeof memory};GetProcessMemoryInfo(worker.process.hProcess,&memory,sizeof memory);
      std::cout<<"observer_working_set_bytes="<<memory.WorkingSetSize<<" private_pagefile_bytes="<<memory.PagefileUsage<<'\n';
      worker.Finish();Check(fs::exists(root/L"verified-launch.json"),"successful background verification preserves the record");
    }
    original=setup::NativeStat(live);Write(live,"gixture");Time(live,original.time);
    Check(Quick(root),"same-size same-time corruption passes metadata opening");
    {Observer worker(root,self);worker.Finish();}
    Check(!fs::exists(root/L"verified-launch.json"),"background SHA-256 invalidates stable corrupted content");
    Check(setup::ReadJSON(root/L"current.json")["version"].Int()==1,"background failure leaves current version untouched");
    Check(Refused(root)&&setup::SHA256(live)==setup::SHA256(root/L"app-1/harmony-nativo.exe"),"next opening performs existing repair and refuses same-version tampering");
    Check(!Quick(root)&&Quick(root),"opening after repair verifies fully and creates a fresh record");
    IndependentLive(root);Quick(root);
    Write(live,"wrong size");Check(Refused(root),"size change caught before opening");Quick(root);
    fs::remove(live);Check(Refused(root),"missing file caught before opening");Quick(root);
    Write(root/L"app/extra.dll","extra");Check(Refused(root),"unlisted file caught before opening");Quick(root);
    // Renaming an open file is allowed by FILE_SHARE_DELETE.
    {
      Observer worker(root,self);fs::rename(live,root/L"app/moved.exe");fs::rename(root/L"app/moved.exe",live);
      worker.Finish();Check(fs::exists(root/L"verified-launch.json"),"rename during observation never invalidates the record");
    }
    {
      Observer worker(root,self);setup::PromoteNative(root,root/L"app-2",next);Select(root,2);
      worker.Finish();Check(!Quick(root)&&Quick(root),"promotion cancels active reader; first updated opening hashes fully");
    }
    setup::PromoteNative(root,root/L"app-1",manifest);Select(root,1);
    Check(!Quick(root)&&Quick(root),"rollback invalidates record even when returning to a previously verified version");
    {
      Observer worker(root,self);TerminateProcess(worker.process.hProcess,77);WaitForSingleObject(worker.process.hProcess,5000);
      Check(Quick(root),"killed observer leaves installation and committed record usable");
      setup::PromoteNative(root,root/L"app-1",manifest);Check(!Quick(root),"abandoned observer mutex cannot block repair");
    }
    std::cout<<"PASS: fast launch and background invariants complete\n";
  }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
