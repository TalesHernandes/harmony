// Metadata-gated launch; the depot remains authoritative on every full check.
#pragma once
#include "../src/updater.h"
#include <set>
#include <cwctype>
#include <cctype>
#include <objbase.h>
namespace app::installer {
struct NativeHandle {
  HANDLE value=INVALID_HANDLE_VALUE;
  explicit NativeHandle(HANDLE h):value(h){}
  ~NativeHandle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
  NativeHandle(const NativeHandle&)=delete;
};
inline void AssertNativeAncestors(std::filesystem::path directory) {
  for(auto parent=directory;!parent.empty();) {
    auto a=GetFileAttributesW(parent.c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("missing or reparse path: "+net::Narrow(parent.wstring()));
    auto next=parent.parent_path();if(next==parent)break;parent=next;
  }
}
inline void AssertNativeTree(const std::filesystem::path& directory) {
  AssertNativeAncestors(directory);
  for(auto const& entry:std::filesystem::recursive_directory_iterator(directory)) {
    auto a=GetFileAttributesW(entry.path().c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("reparse installed entry: "+net::Narrow(entry.path().wstring()));
  }
}
inline std::wstring NativeKey(std::filesystem::path path) {
  auto key=path.make_preferred().wstring();for(auto& c:key)c=std::towlower(c);return key;
}
// The same name is used by live-app.ps1. Names contain no path separators.
inline std::wstring NativeGateName(const std::filesystem::path& root) {
  // Invariant lowercase like ToLowerInvariant in the scripts: towlower in the
  // C locale leaves accented capitals alone, and such a profile got two gate names.
  auto key=std::filesystem::absolute(root).lexically_normal().make_preferred().wstring();
  if(!key.empty()) {
    std::wstring lower(key.size(),L' ');
    if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,key.c_str(),int(key.size()),lower.data(),int(lower.size()),nullptr,nullptr,0)==int(key.size()))key=std::move(lower);
  }
  for(auto& c:key)if(c==L'\\'||c==L'/'||c==L':')c=L'_';
  return L"Local\\harmony-native-observer-"+key;
}
struct NativeMutation {
  NativeHandle event,gate;
  explicit NativeMutation(const std::filesystem::path& root):
    event(CreateEventW(nullptr,TRUE,FALSE,(NativeGateName(root)+L"-cancel").c_str())),
    gate(CreateMutexW(nullptr,FALSE,NativeGateName(root).c_str())) {
    if(!event.value||!gate.value)throw std::runtime_error("observer cancellation failed");
    // Signal again while waiting: an observer that takes the gate between the
    // signal and the wait clears the event once and would otherwise hash to the end.
    DWORD wait=WAIT_TIMEOUT;
    for(int i=0;i<300&&wait==WAIT_TIMEOUT;++i){SetEvent(event.value);wait=WaitForSingleObject(gate.value,100);}
    if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)throw std::runtime_error("observer cancellation timed out");
  }
  ~NativeMutation(){ReleaseMutex(gate.value);}
};
struct NativeCancelled : std::runtime_error {NativeCancelled():std::runtime_error("installation changed during observation") {}};
inline void NativeCancelPoint(HANDLE cancel) {if(cancel&&WaitForSingleObject(cancel,0)==WAIT_OBJECT_0)throw NativeCancelled{};}
inline std::string NativeSharedHash(const std::filesystem::path& path,HANDLE cancel=nullptr,
                                    const std::function<void()>& opened={}) {
  NativeCancelPoint(cancel);
  NativeHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
  BY_HANDLE_FILE_INFORMATION before{},after{};
  if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&before)||(before.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))throw NativeCancelled{};
  HCRYPTPROV provider{};HCRYPTHASH hash{};
  if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw NativeCancelled{};
  struct Crypto {HCRYPTPROV p;HCRYPTHASH h{};~Crypto(){if(h)CryptDestroyHash(h);CryptReleaseContext(p,0);}} crypto{provider};
  if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&crypto.h))throw NativeCancelled{};
  if(opened)opened();
  char buffer[65536];DWORD read{};
  for(;;) {
    NativeCancelPoint(cancel);
    if(!ReadFile(file.value,buffer,sizeof(buffer),&read,nullptr))throw NativeCancelled{};
    if(!read)break;
    if(!CryptHashData(crypto.h,reinterpret_cast<BYTE*>(buffer),read,0))throw NativeCancelled{};
  }
  if(!GetFileInformationByHandle(file.value,&after)||before.nFileIndexHigh!=after.nFileIndexHigh||before.nFileIndexLow!=after.nFileIndexLow||before.nFileSizeHigh!=after.nFileSizeHigh||before.nFileSizeLow!=after.nFileSizeLow||CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime))throw NativeCancelled{};
  BYTE bytes[32];DWORD size=sizeof(bytes);if(!CryptGetHashParam(crypto.h,HP_HASHVAL,bytes,&size,0))throw NativeCancelled{};
  std::string result;constexpr char hex[]="0123456789abcdef";for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}return result;
}
inline json::Value NativeSharedJSON(const std::filesystem::path& path) {
  NativeHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nFileSizeHigh||info.nFileSizeLow>4*1024*1024)return {};
  std::string text(info.nFileSizeLow,'\0');DWORD size{};
  if(!ReadFile(file.value,text.data(),DWORD(text.size()),&size,nullptr)||size!=text.size())return {};
  auto value=json::Parse(text);return value?*value:json::Value{};
}
inline std::string NativeIdentity(const std::filesystem::path& path) {
  NativeHandle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))throw NativeCancelled{};
  return std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
}
struct NativeObservation {
  uint64_t bytes=0,time=0;
  bool operator==(const NativeObservation&) const=default;
};
inline uint64_t NativeTime(FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
inline NativeObservation NativeStat(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
    throw std::runtime_error("missing or reparse file: "+net::Narrow(path.wstring()));
  return {(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,NativeTime(info.ftLastWriteTime)};
}
struct NativeSnapshot {
  std::map<std::wstring,NativeObservation> files;
  std::string manifestHash;
};
inline std::filesystem::path NativeScanScope(const std::filesystem::path& root) {
  return std::filesystem::is_regular_file(root.parent_path()/L".harmony-installer")?root.parent_path():root;
}
// One enumeration checks reparse points and unlisted files. The launch scope is
// native/, excluding unselected app-N depots; callers verifying staging use it.
inline NativeSnapshot ScanNative(const std::filesystem::path& directory,const std::filesystem::path& depot,
                                 const json::Value& manifest,const std::filesystem::path& scope={}) {
  namespace fs=std::filesystem;
  auto base=scope.empty()?directory:scope;
  AssertNativeAncestors(directory);AssertNativeAncestors(depot);AssertNativeAncestors(base);
  NativeSnapshot snapshot;std::set<std::wstring> seen;bool executable=false;
  for(auto const& entry:manifest["files"].Elements()) {
    fs::path relative(net::Wide(entry["path"].Str()));
    if(relative.empty()||relative.has_root_path())throw std::runtime_error("manifest path outside version refused");
    for(auto const& part:relative) {
      auto name=part.wstring();
      if(name.empty()||name==L"."||name==L".."||name.back()==L'.'||name.back()==L' '||name.find(L':')!=std::wstring::npos)
        throw std::runtime_error("manifest path outside version refused");
    }
    auto key=NativeKey(relative);
    if(key==L"manifest.json"||!seen.insert(key).second)throw std::runtime_error("duplicate manifest entry");
    if(key==L"harmony-nativo.exe")executable=true;
  }
  if(!executable)throw std::runtime_error("harmony-nativo.exe missing from manifest");
  std::vector<fs::path> pending{base};
  while(!pending.empty()) {
    auto parent=pending.back();pending.pop_back();
    WIN32_FIND_DATAW info{};
    auto search=FindFirstFileExW((parent/L"*").c_str(),FindExInfoBasic,&info,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(search==INVALID_HANDLE_VALUE)throw std::runtime_error("installed directory enumeration failed: "+net::Narrow(parent.wstring()));
    struct Search {HANDLE handle;~Search(){FindClose(handle);}} close{search};
    for(;;) {
      std::wstring name=info.cFileName;
      if(name!=L"."&&name!=L"..") {
        auto path=parent/name;
        bool unused=!scope.empty()&&parent==depot.parent_path()&&path!=depot&&std::regex_match(name,std::wregex(L"app-[0-9]+"));
        if(!unused) {
          if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("reparse installed entry: "+net::Narrow(path.wstring()));
          if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)pending.push_back(path);
          else {
            auto relative=path.lexically_relative(directory);
            if(!relative.empty()&&*relative.begin()!=L"..") {
              auto key=NativeKey(relative);
              if(key!=L"manifest.json"&&!seen.count(key))throw std::runtime_error("unlisted installed file: "+net::Narrow(path.wstring()));
              snapshot.files.emplace(key,NativeObservation{(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,NativeTime(info.ftLastWriteTime)});
            }
          }
        }
      }
      if(!FindNextFileW(search,&info)) {
        if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("installed directory enumeration interrupted");
        break;
      }
    }
  }
  for(auto const& entry:manifest["files"].Elements()) {
    auto key=NativeKey(fs::path(net::Wide(entry["path"].Str())));
    auto found=snapshot.files.find(key);
    if(entry["bytes"].Int(-1)<0||found==snapshot.files.end()||found->second.bytes!=uint64_t(entry["bytes"].Int()))
      throw std::runtime_error("invalid installed file: "+entry["path"].Str());
  }
  auto liveHash=NativeSharedHash(directory/L"manifest.json"),depotHash=NativeSharedHash(depot/L"manifest.json");
  if(liveHash.size()!=64||liveHash!=depotHash)throw std::runtime_error("live manifest differs from depot");
  snapshot.manifestHash=liveHash;return snapshot;
}
inline void HashNativeFiles(const std::filesystem::path& directory,const json::Value& manifest) {
  for(auto const& entry:manifest["files"].Elements()) {
    auto expected=entry["sha256"].Str();for(auto& c:expected)c=char(std::tolower(static_cast<unsigned char>(c)));
    if(expected.size()!=64||SHA256(directory/net::Wide(entry["path"].Str()))!=expected)
      throw std::runtime_error("invalid installed file: "+entry["path"].Str());
  }
}
inline void VerifyNativeTree(const std::filesystem::path& directory,const std::filesystem::path& depot,const json::Value& manifest) {
  ScanNative(directory,depot,manifest);HashNativeFiles(directory,manifest);
}
inline json::Value NativeRecord(int64_t version,const NativeSnapshot& snapshot) {
  auto record=json::Value::Object();record["format"]=1;record["version"]=version;record["manifest_sha256"]=snapshot.manifestHash;
  GUID id{};wchar_t token[40]{};if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,token,40))throw std::runtime_error("record GUID failed");
  record["id"]=net::Narrow(token);record["files"]=json::Value::Object();
  for(auto const& [path,stat]:snapshot.files) {
    if(path==L"manifest.json")continue;
    auto value=json::Value::Object();value["bytes"]=std::to_string(stat.bytes);value["filetime"]=std::to_string(stat.time);
    record["files"][net::Narrow(path)]=value;
  }
  return record;
}
inline bool NativeRecordMatches(const json::Value& record,int64_t version,const NativeSnapshot& snapshot) {
  if(record["format"].Int()!=1||record["version"].Int()!=version||record["id"].Str().empty()||record["manifest_sha256"].Str()!=snapshot.manifestHash||!record["files"].IsObject()||record["files"].Items().size()+1!=snapshot.files.size())return false;
  for(auto const& [path,stat]:snapshot.files) {
    if(path==L"manifest.json")continue;
    auto const& file=record["files"][net::Narrow(path)];
    if(file["bytes"].Str()!=std::to_string(stat.bytes)||file["filetime"].Str()!=std::to_string(stat.time))return false;
  }
  return true;
}
inline void SaveNativeRecord(const std::filesystem::path& root,const json::Value& record) {
  auto target=root/L"verified-launch.json",part=root/L"verified-launch.json.part";
  // The caller owns .install.lock. An interrupted write is never a valid record.
  auto text=record.Dump();
  NativeHandle out(CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};DWORD written{};
  if(out.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(out.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!WriteFile(out.value,text.data(),DWORD(text.size()),&written,nullptr)||written!=text.size()||!FlushFileBuffers(out.value))throw std::runtime_error("verification record write failed");
  CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;
  if(!MoveFileExW(part.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("verification record replace failed");
}
inline void PromoteNative(const std::filesystem::path& root,const std::filesystem::path& depot,const json::Value& manifest) {
  namespace fs=std::filesystem;
  NativeMutation mutation(root);
  DeleteFileW((root/L"verified-launch.json").c_str());
  VerifyNativeTree(depot,depot,manifest);
  auto live=root/L"app";
  if(fs::exists(live)) {
    AssertNativeTree(live);
    for(auto const& entry:fs::recursive_directory_iterator(live)) {
      if(!entry.is_regular_file())continue;
      auto handle=CreateFileW(entry.path().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
      if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("close native application before promotion");
      CloseHandle(handle);
    }
  }
  // Unique sibling names keep an interrupted swap recoverable from current.json.
  GUID id{};wchar_t name[40];
  if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,name,40))throw std::runtime_error("promotion GUID failed");
  std::wstring suffix=name;
  auto stage=root/(L".staging-app-"+suffix);
  if(!fs::create_directory(stage))throw std::runtime_error("promotion staging exists");
  for(auto const& entry:manifest["files"].Elements()) {
    auto relative=fs::path(net::Wide(entry["path"].Str()));auto target=stage/relative;
    fs::create_directories(target.parent_path());
    if(!CreateHardLinkW(target.c_str(),(depot/relative).c_str(),nullptr))fs::copy_file(depot/relative,target);
  }
  fs::copy_file(depot/L"manifest.json",stage/L"manifest.json");
  VerifyNativeTree(stage,depot,manifest);
  if(fs::exists(live))fs::rename(live,root/(L".retired-app-"+suffix));
  fs::rename(stage,live);
}
inline std::filesystem::path VerifiedNative(const std::filesystem::path& root,bool* quick=nullptr,json::Value* accepted=nullptr) {
  namespace fs=std::filesystem;
  if(quick)*quick=false;
  AssertNativeAncestors(root);
  AssertNativeAncestors(root/L"current.json");
  AssertNativeAncestors(root/L".harmony-native-install");
  if(!fs::is_regular_file(root/L".harmony-native-install"))throw std::runtime_error("installation marker missing");
  NativeHandle lock(CreateFileW((root/L".install.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(lock.value==INVALID_HANDLE_VALUE)throw std::runtime_error("installation in progress");
  auto pointer=ReadJSON(root/L"current.json");auto version=pointer["version"].Int();
  if(version<1||pointer["directory"].Str()!="app-"+std::to_string(version)||(!pointer["live"].Str().empty()&&pointer["live"].Str()!="app"))throw std::runtime_error("invalid version pointer");
  auto depot=root/net::Wide(pointer["directory"].Str());AssertNativeAncestors(depot);
  AssertNativeAncestors(depot/L"manifest.json");
  auto manifest=ReadJSON(depot/L"manifest.json");
  if(!manifest["complete"].Bool()||manifest["version"].Int()!=version||!manifest["files"].IsArray())throw std::runtime_error("incomplete version refused");
  auto directory=root/L"app";
  if(!fs::exists(directory)) {
    if(pointer["live"].Str()=="app")PromoteNative(root,depot,manifest);
    else directory=depot;
  }
  NativeSnapshot snapshot;
  try {
    snapshot=ScanNative(directory,depot,manifest,NativeScanScope(root));
    auto record=ReadJSON(root/L"verified-launch.json");
    if(NativeRecordMatches(record,version,snapshot)) {
      if(quick)*quick=true;if(accepted)*accepted=record;
      return directory/L"harmony-nativo.exe";
    }
    HashNativeFiles(directory,manifest);
  } catch(...) {
    DeleteFileW((root/L"verified-launch.json").c_str());
    if(directory==depot)throw;
    AssertNativeTree(directory);
    auto liveManifest=ReadJSON(directory/L"manifest.json");
    PromoteNative(root,depot,manifest);
    // Same-version tampering is repaired, but this opening still refuses.
    if(!liveManifest["complete"].Bool()||liveManifest["version"].Int()<1||liveManifest["version"].Int()==version)throw;
    snapshot=ScanNative(directory,depot,manifest,NativeScanScope(root));HashNativeFiles(directory,manifest);
  }
  // Stat again after hashing: never bless metadata observed before a write.
  for(auto const& [path,stat]:snapshot.files)if(NativeStat(directory/path)!=stat)throw std::runtime_error("installation changed during verification");
  try { SaveNativeRecord(root,NativeRecord(version,snapshot)); } catch(...) {DeleteFileW((root/L"verified-launch.json").c_str());}
  return directory/L"harmony-nativo.exe";
}
// Returns a diagnostic only for a stable content mismatch. Every race/IO error
// abandons silently. No app is stopped and no repair runs here.
inline std::string BackgroundNative(const std::filesystem::path& root,const std::string& recordID,
                                    const std::function<void()>& opened={}) {
  NativeHandle event(CreateEventW(nullptr,TRUE,FALSE,(NativeGateName(root)+L"-cancel").c_str()));
  // Intentionally owned until process exit: mutators wait for the observer's
  // image and all read handles to disappear, including on abnormal termination.
  auto gate=CreateMutexW(nullptr,FALSE,NativeGateName(root).c_str());
  if(!event.value||!gate)return {};
  auto wait=WaitForSingleObject(gate,0);
  if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(gate);return {};}
  ResetEvent(event.value);
  try {
    if(!SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN))return {};
    auto record=NativeSharedJSON(root/L"verified-launch.json");
    if(record["id"].Str()!=recordID)return {};
    auto pointer=NativeSharedJSON(root/L"current.json");auto version=pointer["version"].Int();
    if(version<1||pointer["directory"].Str()!="app-"+std::to_string(version))return {};
    auto depot=root/net::Wide(pointer["directory"].Str());auto directory=root/L"app";
    if(!std::filesystem::exists(directory)&&pointer["live"].Str().empty())directory=depot;
    auto manifest=NativeSharedJSON(depot/L"manifest.json");
    if(!manifest["complete"].Bool()||manifest["version"].Int()!=version||!manifest["files"].IsArray())return {};
    auto snapshot=ScanNative(directory,depot,manifest,NativeScanScope(root));
    if(!NativeRecordMatches(record,version,snapshot))return {};
    std::map<std::wstring,std::string> identities;
    for(auto const& [path,stat]:snapshot.files)identities.emplace(path,NativeIdentity(directory/path));
    // Hold directory identities with full sharing; renames/removal remain free.
    NativeHandle live(CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION before{},after{};
    if(live.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(live.value,&before))return {};
    std::string failure;
    for(auto const& entry:manifest["files"].Elements()) {
      auto expected=entry["sha256"].Str();for(auto& c:expected)c=char(std::tolower(static_cast<unsigned char>(c)));
      if(NativeSharedHash(directory/net::Wide(entry["path"].Str()),event.value,opened)!=expected){failure="background integrity failed: "+entry["path"].Str();break;}
    }
    NativeCancelPoint(event.value);
    if(failure.empty())return {};
    auto final=ScanNative(directory,depot,manifest,NativeScanScope(root));
    for(auto const& [path,id]:identities)if(NativeIdentity(directory/path)!=id)return {};
    NativeHandle current(CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(current.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(current.value,&after)||before.nFileIndexHigh!=after.nFileIndexHigh||before.nFileIndexLow!=after.nFileIndexLow||before.dwVolumeSerialNumber!=after.dwVolumeSerialNumber)return {};
    if(snapshot.files!=final.files||snapshot.manifestHash!=final.manifestHash)return {};
    NativeHandle lock(CreateFileW((root/L".install.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(lock.value==INVALID_HANDLE_VALUE)return {};
    NativeCancelPoint(event.value);
    if(NativeSharedJSON(root/L"current.json").Dump()!=pointer.Dump()||NativeSharedJSON(root/L"verified-launch.json").Dump()!=record.Dump())return {};
    if(!DeleteFileW((root/L"verified-launch.json").c_str()))return {};
    return failure;
  } catch(...) {return {};}
}

}
