#include "../src/updater.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <gdiplus.h>
#include "installer-text.h"
#include "verified-launch.h"
#include "../src/startup-timing.h"
#include <algorithm>
namespace fs = std::filesystem;
namespace setup = app::installer;
namespace {
HWND window{}; bool failed = false, hover = false;
enum class Screen { Welcome, Existing, Installing, Complete, Failure, Remove, Removed };
Screen screen=Screen::Welcome;
bool interactive=false, removing=false, desktop=true, startup=true, reinstall=false;
bool applyingUpdate=false, rollbackNotice=false, nativeOpened=false;
int progress=0, currentStep=0, installedVersion=0, selectedOption=-1;
float dpiScale=1;
POINT pointer{-1,-1};
std::wstring locationError;
fs::path defaultRoot;
fs::path progressPath;
fs::path logPath;
void StartInstall();
void StartRemove();
HANDLE single{};
std::wstring status = L"Preparando o novo harmony…";
fs::path root, stage, bridgeSource; bool tray = false, fresh = false;
std::vector<HANDLE> rootLocks;
void Trace(const char* message, DWORD code=0) {
  logf("installer: %s, Windows code %lu",message,code);
  try { std::ofstream out(logPath,std::ios::app);out<<message<<" code="<<code<<'\n'; } catch (...) {}
}
json::Value ReadProgress(const fs::path& path) {
  // Status is observational: never block the writer's atomic replacement.
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return {};
  std::string data(8192,'\0');DWORD size{};BOOL ok=ReadFile(file,data.data(),DWORD(data.size()),&size,nullptr);CloseHandle(file);
  if(!ok)return {};data.resize(size);auto value=json::Parse(data);return value?*value:json::Value{};
}
constexpr UINT Done = WM_APP+1, Error = WM_APP+2, Rollback = WM_APP+3;
fs::path Known(REFKNOWNFOLDERID id) {
  PWSTR value{}; if (FAILED(SHGetKnownFolderPath(id,0,nullptr,&value))) throw std::runtime_error("known folder missing");
  fs::path result(value); CoTaskMemFree(value); return result;
}
std::wstring Quote(const fs::path& path) {
  if (path.wstring().find(L'"') != std::wstring::npos) throw std::runtime_error("invalid path");
  return L"\"" + path.wstring() + L"\"";
}
DWORD Run(const fs::path& exe, std::wstring args, bool wait = true) {
  auto command = Quote(exe) + L" " + args;
  STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE}; HANDLE output=INVALID_HANDLE_VALUE,input=INVALID_HANDLE_VALUE;
  if(wait) {
    output=CreateFileW(logPath.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&security,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(output!=INVALID_HANDLE_VALUE&&input!=INVALID_HANDLE_VALUE) { si.dwFlags|=STARTF_USESTDHANDLES;si.hStdOutput=si.hStdError=output;si.hStdInput=input; }
  }
  PROCESS_INFORMATION pi{};
  // PowerShell refuses to delete its current directory during rollback/removal.
  auto cwd=logPath.parent_path();
  BOOL created=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,!!(si.dwFlags&STARTF_USESTDHANDLES),CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,cwd.c_str(),&si,&pi);
  if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
  if(!created)return 1;
  if(!tray) AllowSetForegroundWindow(pi.dwProcessId);
  ResumeThread(pi.hThread);
  DWORD code = 0;
  if (wait) {
    if(WaitForSingleObject(pi.hProcess,300000)!=WAIT_OBJECT_0) { TerminateProcess(pi.hProcess,ERROR_TIMEOUT);WaitForSingleObject(pi.hProcess,5000);code=ERROR_TIMEOUT; }
    else GetExitCodeProcess(pi.hProcess,&code);
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
}
fs::path Powershell() { return Known(FOLDERID_System) / L"WindowsPowerShell/v1.0/powershell.exe"; }
bool Under(const fs::path& path, const fs::path& parent) {
  auto text = path.wstring(), prefix = parent.wstring()+L"\\";
  return text.size() > prefix.size() && CompareStringOrdinal(text.data(),int(prefix.size()),prefix.data(),int(prefix.size()),TRUE)==CSTR_EQUAL;
}
// Hold each ancestor without FILE_SHARE_DELETE so a checked directory cannot
// be swapped for a junction while the installer writes through it.
void LockPath(const fs::path& path, bool create, std::vector<HANDLE>& locks) {
  auto absolute=fs::absolute(path).lexically_normal();
  auto current=absolute.root_path();
  for(const auto& part:absolute.relative_path()) {
    current/=part;
    if(create && GetFileAttributesW(current.c_str())==INVALID_FILE_ATTRIBUTES)
      if(!CreateDirectoryW(current.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) throw std::runtime_error("directory creation failed");
    HANDLE h=CreateFileW(current.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("directory lock failed");
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
      CloseHandle(h);throw std::runtime_error("reparse directory refused");
    }
    locks.push_back(h);
  }
}
bool SamePath(const fs::path& a,const fs::path& b) {
  return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
}
std::wstring RegistryKey() {
  auto test=setup::TestRoot();
  return test.empty()?L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\harmony":L"Software\\harmony-installer-tests\\"+test.filename().wstring()+L"\\Uninstall\\harmony";
}
fs::path RegisteredRoot() {
  wchar_t path[32768];DWORD size=sizeof(path);
  if(RegGetValueW(HKEY_CURRENT_USER,RegistryKey().c_str(),L"InstallLocation",RRF_RT_REG_SZ,nullptr,path,&size)!=ERROR_SUCCESS)return {};
  return fs::path(path).lexically_normal().make_preferred();
}
std::wstring CheckLocation(const fs::path& path,bool allowLegacy=false,bool scan=true) {
  auto text=path.wstring();
  if(!path.is_absolute()||text.size()<3||text[1]!=L':'||text[2]!=L'\\'||text.find_first_of(L"\"'`$;*?<>|",3)!=std::wstring::npos||text.find(L':',2)!=std::wstring::npos)
    return L"Escolha uma pasta local, sem atalhos ou caminho de rede.";
  auto drive=GetDriveTypeW(path.root_path().c_str());
  if(drive!=DRIVE_FIXED) return L"Escolha uma pasta em um disco local fixo.";
  for(auto id:{FOLDERID_ProgramFiles,FOLDERID_ProgramFilesX86,FOLDERID_Windows}) {
    auto protectedPath=Known(id);
    if(SamePath(path,protectedPath)||Under(path,protectedPath))return L"Essa pasta exige administrador. Escolha uma pasta pessoal.";
  }
  if(path==path.root_path())return L"Não é possível instalar na raiz do disco. Escolha uma pasta pessoal.";
  auto test=setup::TestRoot();
  if(!test.empty()&&!Under(path,test))return L"O teste só pode instalar dentro da pasta isolada.";
  std::vector<HANDLE> checks;
  try {
    auto ancestor=path;while(!fs::exists(ancestor)&&ancestor!=ancestor.root_path())ancestor=ancestor.parent_path();
    LockPath(ancestor,false,checks);
    if(fs::exists(path)) {
      if(!fs::is_directory(path))throw std::runtime_error("not directory");
      if(!fs::is_empty(path)&&!fs::is_regular_file(path/L".harmony-installer")&&!allowLegacy) {
        for(auto h:checks)CloseHandle(h);
        return L"A pasta não está vazia e não é uma instalação do harmony. Escolha uma pasta vazia.";
      }
      if(scan)for(auto const& entry:fs::recursive_directory_iterator(path))
        if(GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("reparse child");
    }
  } catch(...) {for(auto h:checks)CloseHandle(h);return L"Não é possível usar essa pasta. Escolha uma pasta local sem junções ou atalhos.";}
  for(auto h:checks)CloseHandle(h);
  return {};
}
void AssertRoot() {
  auto error=CheckLocation(root,SamePath(root,defaultRoot));
  if(!error.empty())throw std::runtime_error("installation location refused");
  LockPath(root,true,rootLocks);
}
void SelectLocation(const fs::path& chosen) {
  locationError=CheckLocation(chosen);
  if(locationError.empty()) {
    std::vector<HANDLE> locks;
    try {
      LockPath(chosen,false,locks);
      GUID id{};CoCreateGuid(&id);wchar_t name[40];StringFromGUID2(id,name,40);
      auto probe=chosen/(L".harmony-write-check-"+std::wstring(name));
      HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
      if(file==INVALID_HANDLE_VALUE)locationError=L"Essa pasta não permite gravar sem administrador. Escolha uma pasta pessoal.";
      else CloseHandle(file);
    } catch(...) {locationError=L"Escolha uma pasta local sem junções ou atalhos.";}
    for(auto h:locks)CloseHandle(h);
    auto target=fs::is_regular_file(chosen/L".harmony-installer")?chosen:chosen/L"harmony";
    if(locationError.empty())locationError=CheckLocation(target);if(locationError.empty())root=target;
  }
  auto test=setup::TestRoot();
  if(!test.empty()) {
    std::ofstream out(test/L"location-result.json");
    out<<"{\"root\":"<<json::Value(net::Narrow(root.wstring())).Dump()<<",\"error\":"<<json::Value(net::Narrow(locationError)).Dump()<<"}";
  }
  InvalidateRect(window,nullptr,FALSE);
}
void ChooseLocation() {
  if(screen!=Screen::Welcome||installedVersion)return;
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
  dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_NODEREFERENCELINKS);
  dialog->SetTitle(L"Escolha onde instalar o harmony");
  Microsoft::WRL::ComPtr<IShellItem> initial;
  if(SUCCEEDED(SHCreateItemFromParsingName(root.parent_path().c_str(),nullptr,IID_PPV_ARGS(&initial))))dialog->SetFolder(initial.Get());
  if(dialog->Show(window)!=S_OK)return;
  Microsoft::WRL::ComPtr<IShellItem> item;PWSTR value{};
  if(FAILED(dialog->GetResult(&item))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&value)))return;
  auto chosen=fs::path(value).lexically_normal().make_preferred();CoTaskMemFree(value);
  SelectLocation(chosen);
}
fs::path CopyDownload() {
  auto source=app::Executable();std::vector<HANDLE> locks;
  HANDLE input=INVALID_HANDLE_VALUE, output=INVALID_HANDLE_VALUE;
  GUID id{};CoCreateGuid(&id);wchar_t name[40];StringFromGUID2(id,name,40);
  auto target=root/(L"harmony.download-"+std::wstring(name)+L".exe");
  try {
    LockPath(source.parent_path(),false,locks);
    input=CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if(input==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input,&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) throw std::runtime_error("reparse source refused");
    output=CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE) throw std::runtime_error("bridge copy failed");
    char bytes[65536];DWORD read{},written{};
    while(true) {
      if(!ReadFile(input,bytes,sizeof(bytes),&read,nullptr)) throw std::runtime_error("bridge read failed");
      if(!read) break;
      if(!WriteFile(output,bytes,read,&written,nullptr)||written!=read) throw std::runtime_error("bridge write failed");
    }
    if(!FlushFileBuffers(output)) throw std::runtime_error("bridge flush failed");
    auto hash=setup::SHA256(source);
    if(hash.size()!=64 || hash!=setup::SHA256(target)) throw std::runtime_error("bridge copy hash mismatch");
    CloseHandle(output);output=INVALID_HANDLE_VALUE;
  } catch(...) {
    if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
    if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    for(auto h:locks)CloseHandle(h);
    DeleteFileW(target.c_str());throw;
  }
  CloseHandle(input);for(auto h:locks)CloseHandle(h);return target;
}
int BundleVersion();
void Resource(int id, const fs::path& path) {
  auto resource = FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
  if (!resource) throw std::runtime_error("payload missing");
  auto data = LoadResource(nullptr,resource); auto size = SizeofResource(nullptr,resource);
  std::ofstream out(path,std::ios::binary); out.write(static_cast<const char*>(LockResource(data)),size);
  out.close(); if (!out) throw std::runtime_error("payload write failed");
}
int BundleVersion() {
  auto r = FindResourceW(nullptr,MAKEINTRESOURCEW(103),RT_RCDATA);
  if (!r) throw std::runtime_error("bundle version missing");
  auto bytes = static_cast<const char*>(LockResource(LoadResource(nullptr,r)));
  auto value = json::Parse(std::string(bytes,SizeofResource(nullptr,r)));
  int64_t code = value ? (*value)["version"].Int() : 0;
  if (code < 1 || code > 999999999) throw std::runtime_error("invalid bundle version");
  return int(code);
}
int BundleNativeVersion() {
  auto r=FindResourceW(nullptr,MAKEINTRESOURCEW(103),RT_RCDATA);
  auto bytes=static_cast<const char*>(LockResource(LoadResource(nullptr,r)));
  auto value=json::Parse(std::string(bytes,SizeofResource(nullptr,r)));
  return value?int((*value)["native_version"].Int()):0;
}
bool NativeRunning() {
  startup::Mark("running-scan-start");
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); if (snapshot==INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{sizeof(entry)}; bool running = false;
  for (BOOL ok=Process32FirstW(snapshot,&entry);ok;ok=Process32NextW(snapshot,&entry)) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
    if (!process) continue;
    wchar_t path[32768]; DWORD length=ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process,0,path,&length) && Under(fs::path(path),root/L"native")) running=true;
    CloseHandle(process); if (running) break;
  }
  CloseHandle(snapshot); return running;
}
// The candidate schedules its own cleanup so an older installed launcher also
// benefits on the very first upgrade. Windows locks the running image until exit.
void CleanupCandidateAfterExit() {
  auto literal=[](fs::path const& path) {std::wstring s=L"'";for(auto c:path.wstring()){s+=c;if(c==L'\'')s+=c;}return s+L"'";};
  auto command=L"Wait-Process -Id "+std::to_wstring(GetCurrentProcessId())+L" -ErrorAction SilentlyContinue; ";
  command+=L"foreach($f in @("+literal(root/L"harmony.next.exe")+L","+literal(root/L"harmony.next.exe.json")+L")){for($i=0;$i -lt 20;$i++){if(-not(Test-Path -LiteralPath $f)){break};try{Remove-Item -LiteralPath $f -Force -ErrorAction Stop;break}catch{Start-Sleep -Milliseconds 500}}}";
  // Encode the script so spaces and apostrophes in custom roots remain literal.
  DWORD size=0;CryptBinaryToStringW(reinterpret_cast<const BYTE*>(command.data()),DWORD(command.size()*sizeof(wchar_t)),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&size);
  std::wstring encoded(size,L'\0');CryptBinaryToStringW(reinterpret_cast<const BYTE*>(command.data()),DWORD(command.size()*sizeof(wchar_t)),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&size);encoded.resize(wcslen(encoded.c_str()));
  if(Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand "+encoded,false))throw std::runtime_error("candidate cleanup scheduling failed");
}
void LaunchNative() {
  startup::Mark("files-verify-start");
  bool quick=false;json::Value record;
  auto exe=setup::VerifiedNative(root/L"native",&quick,&record);
  startup::Mark("files-verify-end");
  auto command=Quote(exe)+(tray?L" --bandeja":L"");
  STARTUPINFOW si{sizeof si};PROCESS_INFORMATION child{};
  auto cwd=exe.parent_path().wstring();
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,cwd.c_str(),&si,&child))throw std::runtime_error("native launch failed");
  if(!tray)AllowSetForegroundWindow(child.dwProcessId);
  auto resumed=ResumeThread(child.hThread);CloseHandle(child.hThread);CloseHandle(child.hProcess);
  if(resumed==DWORD(-1))throw std::runtime_error("native resume failed");
  startup::Mark("native-created");
  logf("startup native_pid=%lu",child.dwProcessId);
  if(quick&&Run(app::Executable(),L"--verify-background "+net::Wide(record["id"].Str()),false))Trace("background verification could not start");
}
void BeforePreviousLaunch() {
  if(applyingUpdate && !tray) {PostMessageW(window,Rollback,0,0);Sleep(2200);}
}
void Install() {
  try {
    Trace("installation started");
    AssertRoot();
    std::ofstream(root/L".harmony-installer") << "Harmony installer v1";
    fresh=!fs::is_regular_file(root/L"native/current.json");
    GUID id{}; CoCreateGuid(&id); wchar_t name[40]; StringFromGUID2(id,name,40);
    stage = root/(L".installer-"+std::wstring(name)); fs::create_directory(stage);
    Resource(102,stage/L"bridge-host.ps1");
    if(fresh) {
      auto clean=Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(stage/L"bridge-host.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -CleanBroken");
      if(clean) throw std::runtime_error("broken installation cleanup failed");
    }
    if(bridgeSource!=app::Executable() || app::Executable().parent_path()!=root) bridgeSource=CopyDownload();
    Resource(101,stage/L"payload.zip");Trace("resources written");
    auto code=Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(stage/L"bridge-host.ps1")+
      L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Package "+Quote(stage/L"payload.zip")+L" -Bridge "+Quote(bridgeSource)+L" -Release "+std::to_wstring(BundleVersion())+(fresh?L" -Fresh":L"")+((tray||interactive)?L" -Tray":L"")+(interactive?L" -Interactive":L"")+(desktop?L" -Desktop":L"")+(startup?L" -Startup":L"")+(reinstall?L" -Reinstall":L""));
    Trace("installer returned",code);
    for(auto h:rootLocks) CloseHandle(h); rootLocks.clear();
    if (code == 0) {
      // Keep one compressed bridge and the previous installed version, not
      // another expanded copy of the entire bundle.
      try {
        if (!Under(fs::weakly_canonical(stage),fs::weakly_canonical(root))) throw std::runtime_error("cleanup outside root");
        if(GetFileAttributesW(stage.c_str())&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("cleanup reparse point");
        for(auto const& entry:fs::recursive_directory_iterator(stage))
          if(GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("cleanup reparse point");
        fs::remove_all(stage);
      } catch(...) { Trace("staging cleanup deferred"); }
      if(bridgeSource!=app::Executable() && bridgeSource.parent_path()==root) DeleteFileW(bridgeSource.c_str());
      PostMessageW(window,Done,0,0);
    } else {
      // Rollback has its own progress. Preserve the operation that failed so
      // the error screen never mistakes completed cleanup for registration.
      if(fs::is_regular_file(progressPath)) fs::copy_file(progressPath,progressPath.wstring()+L".failure",fs::copy_options::overwrite_existing);
      if(fs::exists(root/L"recovery.json"))
        Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(root/L"recover-native.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Recover");
      if (fs::exists(root/L"native/current.json") && !NativeRunning()) {BeforePreviousLaunch();LaunchNative();}
      if(fresh) {
        auto cleanup=Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(stage/L"bridge-host.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Remove");
        if(cleanup) Trace("fresh rollback cleanup failed",cleanup);
      }
      else if(!fs::exists(root/L"recovery.json")) {
        std::vector<HANDLE> checked;LockPath(stage,false,checked);for(auto h:checked)CloseHandle(h);
        for(auto const& entry:fs::recursive_directory_iterator(stage))
          if(GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("cleanup reparse point");
        fs::remove_all(stage);
        if(bridgeSource!=app::Executable() && bridgeSource.parent_path()==root) DeleteFileW(bridgeSource.c_str());
      }
      PostMessageW(window,Error,0,0);
    }
  } catch (const std::exception& error) {
    Trace(error.what(),GetLastError());
    for(auto h:rootLocks) CloseHandle(h); rootLocks.clear();
    try {
      if(fresh && fs::is_regular_file(stage/L"bridge-host.ps1")) Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(stage/L"bridge-host.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Remove");
    } catch(...) { Trace("rollback cleanup deferred"); }
    try { if (fs::exists(root/L"native/current.json") && !NativeRunning()) {BeforePreviousLaunch();LaunchNative();} } catch (...) {}
    PostMessageW(window,Error,0,0);
  }
}

void StartInstall() {
  if(screen==Screen::Installing) return;
  locationError=CheckLocation(root,SamePath(root,defaultRoot));
  if(!locationError.empty()){InvalidateRect(window,nullptr,FALSE);return;}
  screen=Screen::Installing; progress=0;currentStep=0;status=L"Preparando a instalação";DeleteFileW(progressPath.c_str());
  InvalidateRect(window,nullptr,FALSE);
  SetTimer(window,1,100,nullptr);
  std::thread(Install).detach();
}
void StartRemove() {
  screen=Screen::Installing; progress=0;currentStep=0;status=L"Encerrando o harmony";DeleteFileW(progressPath.c_str());SetTimer(window,1,100,nullptr);
  InvalidateRect(window,nullptr,FALSE);
  std::thread([] {
    auto code=Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(root/L"uninstall-native.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Remove");
    PostMessageW(window,code?Error:Done,0,0);
  }).detach();
}
void Primary() {
  switch(screen) {
    case Screen::Welcome: StartInstall();break;
    case Screen::Existing: LaunchNative();DestroyWindow(window);break;
    case Screen::Complete: LaunchNative();DestroyWindow(window);break;
    case Screen::Remove: StartRemove();break;
    case Screen::Failure: case Screen::Removed: DestroyWindow(window);break;
    default: break;
  }
}
void Secondary() {
  if(screen==Screen::Existing) {
    auto selected=setup::ReadJSON(root/L"native/current.json")["version"].Int();
    if(selected>BundleVersion()) {
      CloseHandle(single);single=nullptr;
      if(Run(root/L"harmony.exe",L"--reinstall",false)) PostMessageW(window,Error,0,0);
      else DestroyWindow(window);
    } else {reinstall=true;StartInstall();}
  }
  else if(screen!=Screen::Installing) DestroyWindow(window);
}
void Text(HDC dc,const std::wstring& text,int x,int y,int width,int height,int size=15,int weight=500,COLORREF color=RGB(161,161,170),bool centered=false) {
  installerui::PaintText(dc,dpiScale,text,x,y,width,height,size,weight,color,centered);
}
// Exact harmony tokens: style.css and chat-ui.h/titlebar-ui.h.
constexpr COLORREF Accent=RGB(4,133,247), Foreground=RGB(252,252,252), Muted=RGB(161,161,170);
void Box(HDC dc,RECT r,COLORREF color,int radius=8) {
  Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.ScaleTransform(dpiScale,dpiScale);
  Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
  if(!radius) {g.FillRectangle(&brush,int(r.left),int(r.top),int(r.right-r.left),int(r.bottom-r.top));return;}
  Gdiplus::GraphicsPath path;float d=float(radius*2),w=float(r.right-r.left),h=float(r.bottom-r.top),x=float(r.left),y=float(r.top);
  path.AddArc(x,y,d,d,180,90);path.AddArc(x+w-d,y,d,d,270,90);path.AddArc(x+w-d,y+h-d,d,d,0,90);path.AddArc(x,y+h-d,d,d,90,90);path.CloseFigure();g.FillPath(&brush,&path);
}
#include "logo-vector.h"
int PathHeight(const fs::path& path,int width=528) {
  if(!installerui::writing)return 40;
  auto layout=installerui::Layout(path.wstring(),float(width),4096,13,500);
  DWRITE_TEXT_METRICS metrics{};installerui::Check(layout->GetMetrics(&metrics));
  return ((int(metrics.height)+7)/8)*8;
}
bool CanChoose() {return screen==Screen::Welcome&&!installedVersion;}
int LocationWidth() {return CanChoose()?400:528;}
int LocationHeight() {return CanChoose()?std::max(40,PathHeight(root,400)):PathHeight(root);}
int CardHeight() {return 16+24+LocationHeight()+16;}
int OptionsY() {return 192+CardHeight()+24;}
int LocationErrorHeight() {
  if(locationError.empty()||!installerui::writing)return 0;
  auto layout=installerui::Layout(locationError,560,4096,13,500);
  DWRITE_TEXT_METRICS metrics{};installerui::Check(layout->GetMetrics(&metrics));
  return ((int(metrics.height)+7)/8)*8;
}
int FooterY() {
  if(screen==Screen::Welcome)return OptionsY()+80+24+(locationError.empty()?0:8+LocationErrorHeight());
  if(screen==Screen::Existing||screen==Screen::Remove)return 224+CardHeight()+24;
  if(screen==Screen::Failure)return 248+PathHeight(logPath,560)+24;
  return 232;
}
std::wstring StatusText() {
  return applyingUpdate&&!rollbackNotice?L"Etapa "+std::to_wstring(std::clamp(currentStep+1,1,8))+L" de 8 · "+status:status;
}
int StatusHeight() {
  // Reserve two wrapped lines throughout an update, including rollback.
  // Progress messages must never move the bar or resize the window.
  if(applyingUpdate)return 48;
  if(!installerui::writing)return 24;
  auto layout=installerui::Layout(StatusText(),496,4096,15,500);
  DWRITE_TEXT_METRICS metrics{};installerui::Check(layout->GetMetrics(&metrics));
  return ((int(metrics.height)+7)/8)*8;
}
int BarY() {return 176+StatusHeight()+8;}
int Height() {if(applyingUpdate)return BarY()+48;return screen==Screen::Installing?BarY()+32+(removing?6:8)*28+28:FooterY()+44+32;}
void Resize() {
  if(!window)return;
  RECT r{};GetWindowRect(window,&r);
  if(r.bottom-r.top==int(Height()*dpiScale)&&r.right-r.left==int(640*dpiScale))return;
  SetWindowPos(window,nullptr,r.left,r.top,int(640*dpiScale),int(Height()*dpiScale),SWP_NOZORDER|SWP_NOACTIVATE);
  SetWindowRgn(window,CreateRoundRectRgn(0,0,int(640*dpiScale)+1,int(Height()*dpiScale)+1,int(16*dpiScale),int(16*dpiScale)),TRUE);
}
void CheckMark(HDC dc,int x,int y,bool checked,bool focused) {
  // Same vector, 20px box, 6px corners and 8px content gap as HarmonyCheckBox.
  Box(dc,{x,y,x+20,y+20},Accent,6);
  Box(dc,{x+1,y+1,x+19,y+19},checked?Accent:RGB(35,35,37),5);
  if(checked) {
    Gdiplus::Graphics g(dc);g.ScaleTransform(dpiScale,dpiScale);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255,252,252,252),2);pen.SetStartCap(Gdiplus::LineCapSquare);pen.SetEndCap(Gdiplus::LineCapSquare);pen.SetLineJoin(Gdiplus::LineJoinMiter);
    Gdiplus::PointF points[]={{float(x+5),float(y+10)},{float(x+9),float(y+14)},{float(x+16),float(y+7)}};g.DrawLines(&pen,points,3);
  }
  // The native template deliberately has identical Normal/PointerOver colors;
  // Focused also uses the same accent border (no system focus visual).
  (void)focused;
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
  switch(msg) {
    case Rollback:
      if(applyingUpdate && !tray) {
        rollbackNotice=true;screen=Screen::Installing;
        status=L"A atualização falhou e o harmony vai abrir a versão anterior.";
        ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);InvalidateRect(hwnd,nullptr,FALSE);UpdateWindow(hwnd);
      }return 0;
    case Done:
      KillTimer(hwnd,1);progress=100;
      if(!interactive) { DestroyWindow(hwnd);return 0; }
      screen=removing?Screen::Removed:Screen::Complete;InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case Error:
      KillTimer(hwnd,1);failed=true;screen=Screen::Failure;
      {auto record=setup::ReadJSON(progressPath.wstring()+L".failure");if(!record["status"].Str().empty())status=net::Wide(record["status"].Str());}
      if(!interactive) {DestroyWindow(hwnd);return 0;}
      InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_TIMER: {
      if(applyingUpdate) {
        if(fs::exists(progressPath.wstring()+L".rollback")) {
          if(!rollbackNotice)SendMessageW(hwnd,Rollback,0,0);
          return 0;
        }
        if(!nativeOpened && !rollbackNotice && fs::exists(progressPath.wstring()+L".native-ready")) {nativeOpened=true;ShowWindow(hwnd,SW_HIDE);}
      }
      if(rollbackNotice)return 0;
      auto record=ReadProgress(progressPath);int value=int(record["percent"].Int());
      if(value>=0&&value<=100&&!record["status"].Str().empty()) {progress=value;currentStep=int(record["step"].Int());status=net::Wide(record["status"].Str());InvalidateRect(hwnd,nullptr,FALSE);}return 0;
    }
    // The fixture uses the same actions as the visible buttons, only in a marked test root.
    case WM_APP+10:
      if(!setup::TestRoot().empty()) { if(wp==1) Primary(); else if(wp==2) Secondary(); else if(wp==3) desktop=!desktop;else if(wp==4) startup=!startup;InvalidateRect(hwnd,nullptr,FALSE); }return 0;
    case WM_COPYDATA: {
      if(!setup::TestRoot().empty()&&screen==Screen::Welcome&&!installedVersion) {
        auto data=reinterpret_cast<COPYDATASTRUCT*>(lp);
        if(data&&data->dwData==7&&data->cbData>=sizeof(wchar_t)&&data->cbData<65536) {
          auto path=static_cast<const wchar_t*>(data->lpData);
          if(path[data->cbData/sizeof(wchar_t)-1]==0)SelectLocation(fs::path(path).lexically_normal().make_preferred());
        }
      }return TRUE;
    }
    case WM_KEYDOWN:
      if(wp==VK_TAB) {selectedOption=(selectedOption+1)%((screen==Screen::Welcome&&!installedVersion)?5:screen==Screen::Welcome?4:2);InvalidateRect(hwnd,nullptr,FALSE);}
      else if(wp==VK_ESCAPE) {if(screen!=Screen::Installing) DestroyWindow(hwnd);}
      else if(wp==VK_RETURN) {if(selectedOption==4)ChooseLocation();else if(selectedOption==2||selectedOption==3) {if(screen==Screen::Welcome){if(selectedOption==2)desktop=!desktop;else startup=!startup;}}else if(selectedOption==1) Secondary();else Primary();}
      else if(wp==VK_SPACE) {if(screen==Screen::Welcome){if(selectedOption==2)desktop=!desktop;else if(selectedOption==3)startup=!startup;else if(selectedOption==4)ChooseLocation();}InvalidateRect(hwnd,nullptr,FALSE);}return 0;
    case WM_CLOSE: if(screen!=Screen::Installing) DestroyWindow(hwnd); return 0;
    case WM_LBUTTONUP: {
      int x=int(LOWORD(lp)/dpiScale),y=int(HIWORD(lp)/dpiScale);
      if(y<32) {if(x>=594) PostMessageW(hwnd,WM_CLOSE,0,0);else if(x>=548) ShowWindow(hwnd,SW_MINIMIZE);return 0;}
      if(y>=FooterY()&&y<=FooterY()+44) {if(x>=336&&x<=600) Primary();else if(x>=40&&x<=304) Secondary();}
      else if(screen==Screen::Welcome) {
        if(!installedVersion&&x>=472&&x<=584&&y>=232&&y<=268) ChooseLocation();
        if(x>=40&&x<=600&&y>=OptionsY()&&y<OptionsY()+40)desktop=!desktop;
        if(x>=40&&x<=600&&y>=OptionsY()+40&&y<OptionsY()+80)startup=!startup;
        InvalidateRect(hwnd,nullptr,FALSE);
      }return 0;
    }
    case WM_NCHITTEST: { auto hit=DefWindowProcW(hwnd,msg,wp,lp);POINT pt{short(LOWORD(lp)),short(HIWORD(lp))};ScreenToClient(hwnd,&pt);return hit==HTCLIENT&&pt.y<int(32*dpiScale)&&pt.x<int(548*dpiScale)?HTCAPTION:hit; }
    case WM_MOUSEMOVE: pointer={int(LOWORD(lp)/dpiScale),int(HIWORD(lp)/dpiScale)};{TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);}InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_MOUSELEAVE: pointer={-1,-1};InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_DPICHANGED: {dpiScale=HIWORD(wp)/96.f;auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,int(640*dpiScale),int(Height()*dpiScale),SWP_NOZORDER);SetWindowRgn(hwnd,CreateRoundRectRgn(0,0,int(640*dpiScale)+1,int(Height()*dpiScale)+1,int(16*dpiScale),int(16*dpiScale)),TRUE);return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      Resize();
      PAINTSTRUCT paint{};HDC target=BeginPaint(hwnd,&paint);
      HDC dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,int(640*dpiScale),int(Height()*dpiScale));auto oldBitmap=SelectObject(dc,bitmap);
      SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,640,Height(),nullptr);SetViewportExtEx(dc,int(640*dpiScale),int(Height()*dpiScale),nullptr);
      Box(dc,{0,0,640,Height()},RGB(6,6,7),0);SetBkMode(dc,TRANSPARENT);
      Text(dc,L"harmony",16,8,400,24,13,600,Muted);
      for(int i=0;!applyingUpdate&&i<2;++i) {
        int x=548+i*46;bool over=pointer.y>=0&&pointer.y<32&&pointer.x>=x&&pointer.x<x+46;
        Box(dc,{x,0,x+46,32},over?(i?RGB(225,84,81):RGB(39,39,42)):RGB(6,6,7),6);
        Text(dc,i?L"×":L"−",x+16,5,20,24,18,500,Foreground);
      }
      {Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.ScaleTransform(dpiScale,dpiScale);Logo(g,40,64,48);}
      Text(dc,L"harmony",104,73,300,40,20,500,Foreground);
      std::wstring title,body,primary=L"Instalar",secondary=L"Cancelar";
      if(screen==Screen::Welcome) {if(installedVersion)primary=L"Atualizar";title=installedVersion?L"Atualizar para a versão "+std::to_wstring(BundleVersion()):L"Instalar o harmony";}
      if(screen==Screen::Existing) {title=L"harmony já está instalado";body=L"Versão "+std::to_wstring(installedVersion);primary=L"Abrir";secondary=L"Reinstalar";}
      if(screen==Screen::Remove) {title=L"Desinstalar o harmony?";body=L"O aplicativo e seus atalhos serão removidos deste computador.";primary=L"Desinstalar";}
      if(screen==Screen::Complete) {title=L"harmony instalado";body=L"Tudo pronto. Entre na sua conta para começar.";primary=L"Abrir o harmony";secondary=L"Fechar";}
      if(screen==Screen::Removed) {title=L"harmony desinstalado";body=L"O aplicativo e seus atalhos foram removidos.";primary=L"Fechar";secondary=L"";}
      if(screen==Screen::Failure) {title=removing?L"Não foi possível desinstalar":L"Não foi possível instalar";body=L"Falha nesta etapa: "+status+L". Consulte o log:";primary=L"Fechar";secondary=L"";}
      if(screen==Screen::Installing) {
        title=applyingUpdate?(rollbackNotice?L"Voltando para a versão anterior":L"Atualizando o harmony para a versão "+std::to_wstring(BundleVersion())):removing?L"Desinstalando o harmony":L"Instalando o harmony";
        body=StatusText();
      }
      SetWindowTextW(hwnd,(L"harmony — "+title).c_str());
      Text(dc,title,40,136,560,32,20,600,Foreground);
      if(!body.empty()) Text(dc,body,40,176,screen==Screen::Installing?496:560,screen==Screen::Installing?StatusHeight():(screen==Screen::Failure||screen==Screen::Remove)?48:32,15);
      if(screen==Screen::Welcome||screen==Screen::Existing||screen==Screen::Remove) {
        int y=screen==Screen::Welcome?192:224;
        Box(dc,{40,y,600,y+CardHeight()},RGB(24,24,27));
        Text(dc,L"Local da instalação",56,y+16,400,18,13,600);
        Text(dc,root.wstring(),56,y+40,LocationWidth(),LocationHeight(),13,500,Foreground);
        if(screen==Screen::Welcome&&!installedVersion) {
          bool over=pointer.x>=472&&pointer.x<=584&&pointer.y>=232&&pointer.y<=268;
          Box(dc,{472,232,584,268},over?RGB(46,46,49):RGB(39,39,42));
          Text(dc,L"Alterar…",480,240,96,24,13,600,Foreground,true);
          if(selectedOption==4){RECT f{476,236,580,264};auto brush=CreateSolidBrush(Accent);FrameRect(dc,&f,brush);DeleteObject(brush);}
        }
      }
      if(screen==Screen::Welcome) {
        for(int i=0;i<2;++i) {
          bool checked=i?startup:desktop;int y=OptionsY()+8+i*40;
          CheckMark(dc,40,y,checked,selectedOption==i+2);
          Text(dc,i?L"Abrir o harmony quando o Windows iniciar":L"Criar atalho na área de trabalho",68,y,532,24,15,500,Foreground);
        }
        if(!locationError.empty())Text(dc,locationError,40,OptionsY()+88,560,LocationErrorHeight(),13,500,RGB(252,252,252));
      }
      if(screen==Screen::Failure) Text(dc,logPath.wstring(),40,248,560,PathHeight(logPath,560),13);
      if(screen==Screen::Installing) {
        int by=BarY();
        if(!rollbackNotice) {
        Box(dc,{40,by,600,by+8},RGB(39,39,42),4);if(progress)Box(dc,{40,by,40+560*progress/100,by+8},Accent,4);
        Text(dc,std::to_wstring(progress)+L"%",552,176,48,24,13,600,Foreground);
        }
        std::vector<std::wstring> steps=removing?std::vector<std::wstring>{L"Encerrar o aplicativo",L"Remover arquivos",L"Remover atalhos",L"Remover início automático",L"Remover registro do Windows",L"Pronto"}:std::vector<std::wstring>{L"Preparar instalação",L"Extrair arquivos",L"Copiar arquivos",L"Conferir integridade",L"Criar atalhos",L"Registrar no Windows",L"Configurar início automático",L"Pronto"};
        for(int i=0;!applyingUpdate&&i<int(steps.size());++i) {
          int y=by+32+i*28;bool done=i<currentStep,now=i==currentStep;
          Text(dc,done?L"✓":now?L"●":L"○",40,y,24,24,13,600,done?RGB(23,201,100):now?Accent:Muted);
          Text(dc,steps[i],68,y,532,24,13,now?600:500,now?Foreground:Muted);
        }
      } else {
        int fy=FooterY();bool over=pointer.y>=fy&&pointer.y<=fy+44;
        if(!secondary.empty()) {Box(dc,{40,fy,304,fy+44},over&&pointer.x>=40&&pointer.x<=304?RGB(46,46,49):RGB(39,39,42));Text(dc,secondary,56,fy+12,232,28,15,600,Foreground,true);}
        bool primaryOver=over&&pointer.x>=336&&pointer.x<=600;
        Box(dc,{336,fy,600,fy+44},screen==Screen::Remove?(primaryOver?RGB(225,84,81):RGB(219,59,62)):(primaryOver?RGB(53,146,249):Accent));Text(dc,primary,352,fy+12,232,28,15,600,RGB(255,255,255),true);
        if(selectedOption>=0&&selectedOption<2) {RECT focus=selectedOption?RECT{40,fy,304,fy+44}:RECT{336,fy,600,fy+44};InflateRect(&focus,-3,-3);auto brush=CreateSolidBrush(RGB(252,252,252));FrameRect(dc,&focus,brush);DeleteObject(brush);}
      }
      SetMapMode(dc,MM_TEXT);BitBlt(target,0,0,int(640*dpiScale),int(Height()*dpiScale),dc,0,0,SRCCOPY);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);EndPaint(hwnd,&paint);return 0;
    }
    case WM_DESTROY: PostQuitMessage(0);return 0;
  }
  return DefWindowProcW(hwnd,msg,wp,lp);
}

}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
  startup::Mark("launcher-entry");
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput graphicsStartup;ULONG_PTR graphicsToken{};Gdiplus::GdiplusStartup(&graphicsToken,&graphicsStartup,nullptr);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SetCurrentProcessExplicitAppUserModelID(app::AppID);
    auto test=setup::TestRoot();
    auto expected=test.empty()?Known(FOLDERID_UserProgramFiles)/L"harmony":test/L"programs/harmony";
    defaultRoot=fs::absolute(expected).lexically_normal().make_preferred();auto registered=RegisteredRoot();
    root=registered.empty()?expected:registered;bool applying=false,removeUI=false,forceReinstall=false;std::string backgroundID;
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;++i) {
      std::wstring arg=argv[i];
      if(arg==L"--tray"||arg==L"--bandeja")tray=true;
      else if(arg==L"--apply"&&i+1<argc) {applying=true;root=argv[++i];}
      else if(arg==L"--verify-background"&&i+1<argc)backgroundID=net::Narrow(argv[++i]);
      else if(arg==L"--uninstall")removing=true;
      else if(arg==L"--remove-ui") {removing=true;removeUI=true;}
      else if(arg==L"--reinstall") {forceReinstall=true;reinstall=true;}
      else if(arg==L"--quiet"||arg==L"-Embedding"||arg.rfind(L"----AppNotificationActivated:",0)==0) {}
      else {LocalFree(argv);return 2;}
    }
    LocalFree(argv);
    applyingUpdate=applying;
    root=fs::absolute(root).lexically_normal().make_preferred();
    if(!CheckLocation(root,SamePath(root,defaultRoot),false).empty()) return 1;
    if(applying&&!SamePath(root,registered.empty()?defaultRoot:registered))return 2;
    bool download=app::Executable().parent_path()!=root;
    interactive=(download&&!applying)||removing||forceReinstall;
    // Validate existing ancestors before even displaying a destination.
    std::vector<HANDLE> checks;auto ancestor=root;while(!fs::exists(ancestor))ancestor=ancestor.parent_path();
    LockPath(ancestor,false,checks);LockPath(app::Executable().parent_path(),false,checks);for(auto h:checks)CloseHandle(h);
    auto logs=test.empty()?fs::temp_directory_path()/L"harmony-installer":test/L"logs";
    fs::create_directories(logs);logPath=logs/L"installer.log";progressPath=logs/(L"progress-"+std::to_wstring(GetCurrentProcessId())+L".json");
    if(!backgroundID.empty()) {
      std::function<void()> observed;
#ifdef HARMONY_INSTALLER_TEST_ONLY
      if(app::Environment(L"HARMONY_INSTALLER_OBSERVER_TEST")=="1")observed=[&] {
        auto name=setup::NativeGateName(root/L"native");
        setup::NativeHandle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L"-test-ready").c_str()));
        setup::NativeHandle release(OpenEventW(SYNCHRONIZE,FALSE,(name+L"-test-release").c_str()));
        setup::NativeHandle cancel(OpenEventW(SYNCHRONIZE,FALSE,(name+L"-cancel").c_str()));
        SetEvent(ready.value);HANDLE waits[]{release.value,cancel.value};WaitForMultipleObjects(2,waits,FALSE,10000);
      };
#endif
      auto failure=setup::BackgroundNative(root/L"native",backgroundID,observed);
      if(!failure.empty())Trace(failure.c_str());
      // Exit while still owning the observer mutex. A waiting mutation resumes
      // only after this observer process releases its image and read handles.
      ExitProcess(0);
    }
    if(removing&&!removeUI) {
      auto helper=logs/L"harmony-uninstall.exe";
      fs::copy_file(app::Executable(),helper,fs::copy_options::overwrite_existing);
      return int(Run(helper,L"--remove-ui",false));
    }
    auto mutexName=L"Local\\harmony-installer-bridge-"+std::to_wstring(std::hash<std::wstring>{}(RegistryKey()));
    single=CreateMutexW(nullptr,FALSE,mutexName.c_str());if(!single||GetLastError()==ERROR_ALREADY_EXISTS)return 0;
    if(applying && SamePath(app::Executable(),root/L"harmony.next.exe")) CleanupCandidateAfterExit();
    bridgeSource=app::Executable();
    auto pointer=root/L"native/current.json";
    if(fs::is_regular_file(root/L"recovery.json")) {
      if(Run(Powershell(),L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "+Quote(root/L"recover-native.ps1")+L" -ProgressFile "+Quote(progressPath)+L" -Root "+Quote(root)+L" -Recover"))return 1;
    }
    if(fs::is_regular_file(pointer)) {
      setup::AssertNativeAncestors(pointer);
      auto selected=setup::ReadJSON(pointer);auto version=selected["version"].Int();
      if(selected["directory"].Str()!="app-"+std::to_string(version)||!fs::is_regular_file(root/L"native"/net::Wide(selected["directory"].Str())/L"harmony-nativo.exe")) throw std::runtime_error("invalid installed selection");
      auto exe=root/L"native/app/harmony-nativo.exe";
      if(!fs::exists(exe))exe=root/L"native"/net::Wide(selected["directory"].Str())/L"harmony-nativo.exe";
      setup::AssertNativeAncestors(exe);
      DWORD ignored{};auto n=GetFileVersionInfoSizeW(exe.c_str(),&ignored);std::vector<char> data(n);VS_FIXEDFILEINFO* info{};UINT length{};
      if(n&&GetFileVersionInfoW(exe.c_str(),0,n,data.data())&&VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length))installedVersion=LOWORD(info->dwProductVersionLS);
      if(interactive) {screen=BundleVersion()<=version?Screen::Existing:Screen::Welcome;reinstall=BundleVersion()==version;}
      else if(!applying) {
        auto pending=root/L"harmony.next.exe";auto release=setup::ReadJSON(root/L"harmony.next.exe.json");
        auto rejected=setup::ReadJSON(root/L"failed-release.json")["version"].Int();
        if(setup::ReleaseValid(release,int(version))&&release["version_code"].Int()!=rejected&&fs::is_regular_file(pending)&&fs::file_size(pending)==release["size"].Int()&&setup::SHA256(pending)==release["sha256"].Str()&&!NativeRunning()) {
          CloseHandle(single);single=nullptr;return int(Run(pending,L"--apply "+Quote(root)+(tray?L" --tray":L""),false));
        }
        LaunchNative();return 0;
      }
    }
    if(removing)screen=Screen::Remove;
    auto fontPath=logs/L"Inter.ttf";Resource(104,fontPath);installerui::InitializeText(fontPath.c_str());
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpszClassName=L"harmony.installer";wc.lpfnWndProc=WindowProc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    dpiScale=GetDpiForSystem()/96.f;
    window=CreateWindowExW(0,wc.lpszClassName,L"harmony — instalador",WS_POPUP|WS_MINIMIZEBOX,(GetSystemMetrics(SM_CXSCREEN)-int(640*dpiScale))/2,(GetSystemMetrics(SM_CYSCREEN)-int(Height()*dpiScale))/2,int(640*dpiScale),int(Height()*dpiScale),nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));SetWindowRgn(window,CreateRoundRectRgn(0,0,int(640*dpiScale)+1,int(Height()*dpiScale)+1,int(16*dpiScale),int(16*dpiScale)),FALSE);
    if(interactive) {ShowWindow(window,SW_SHOW);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);UpdateWindow(window);}
    if(!interactive||forceReinstall) StartInstall();
    if(applyingUpdate&&!tray) {ShowWindow(window,SW_SHOW);SetForegroundWindow(window);UpdateWindow(window);}
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
    if(removeUI) {
      // A helper outside the installation can remove the locked launcher. Its
      // own file is deleted only after this process exits.
      auto helper=app::Executable();
      Run(Powershell(),L"-NoProfile -WindowStyle Hidden -Command \"Wait-Process -Id "+std::to_wstring(GetCurrentProcessId())+L" -ErrorAction SilentlyContinue; Remove-Item -LiteralPath '"+helper.wstring()+L"' -Force\"",false);
    }
    return failed?1:0;
  } catch(const std::exception& error) {Trace(error.what(),GetLastError());return 1;}
}
