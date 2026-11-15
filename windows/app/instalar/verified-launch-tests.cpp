#include "verified-launch.h"
#include <iostream>
#include <winioctl.h>
namespace fs=std::filesystem;
void Junction(const fs::path& link,const fs::path& target) {
  fs::create_directory(link);
  struct MountPoint { DWORD tag; WORD length,reserved,subOffset,subLength,printOffset,printLength; wchar_t paths[8192]; } data{};
  auto substitute=L"\\??\\"+target.wstring();auto print=target.wstring();
  data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subLength=WORD(substitute.size()*2);data.printOffset=data.subLength+2;data.printLength=WORD(print.size()*2);
  memcpy(data.paths,substitute.c_str(),data.subLength+2);memcpy(reinterpret_cast<char*>(data.paths)+data.printOffset,print.c_str(),data.printLength+2);
  data.length=8+data.printOffset+data.printLength+2;
  auto handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  DWORD returned{};auto ok=handle!=INVALID_HANDLE_VALUE&&DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&returned,nullptr);
  if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
  if(!ok)throw std::runtime_error("junction fixture failed");
}
int main(int argc,char** argv){
  if(argc!=2)return 2;auto root=fs::absolute(argv[1]);fs::create_directories(root/L"app-26");
  auto write=[](fs::path const& p,std::string const& s){std::ofstream(p,std::ios::binary)<<s;};
  write(root/L".harmony-native-install","");write(root/L"current.json",R"({"version":26,"directory":"app-26"})");
  auto exe=root/L"app-26/harmony-nativo.exe";write(exe,"fixture");
  auto manifest=json::Value::Object();manifest["complete"]=true;manifest["version"]=26;manifest["files"]=json::Value::Array();
  auto entry=json::Value::Object();entry["path"]="harmony-nativo.exe";entry["bytes"]=7;entry["sha256"]=app::installer::SHA256(exe);manifest["files"].Push(entry);
  auto save=[&]{write(root/L"app-26/manifest.json",manifest.Dump());};
  int checks=0;auto refused=[&]{save();bool failed=false;try{app::installer::VerifiedNative(root);}catch(...){failed=true;}if(!failed)throw std::runtime_error("unsafe manifest accepted");++checks;};
  try{
    save();if(app::installer::VerifiedNative(root)!=exe)throw std::runtime_error("valid install refused");++checks;
    write(exe,"changed");refused();write(exe,"fixture");
    manifest["complete"]=false;refused();manifest["complete"]=true;
    manifest["version"]=27;refused();manifest["version"]=26;
    manifest["files"].Push(entry);refused();manifest["files"]=json::Value::Array();
    auto bad=entry;bad["path"]="../harmony-nativo.exe";manifest["files"].Push(bad);refused();
    for(auto path:{"C:\\outside.exe","app:stream","missing.exe",".. /harmony-nativo.exe","harmony-nativo.exe.","harmony-nativo.exe "}){manifest["files"]=json::Value::Array();bad["path"]=path;manifest["files"].Push(bad);refused();}
    manifest["files"]=json::Value::Array();refused();
    manifest["files"].Push(entry);bad=entry;bad["path"]="HARMONY-NATIVO.EXE";manifest["files"].Push(bad);refused();
    manifest["files"]=json::Value::Array();manifest["files"].Push(entry);save();
    fs::create_directory(root/L"app-26/assets");auto nested=root/L"app-26/assets/ação.txt";write(nested,"fixture");
    auto asset=entry;asset["path"]="assets/ação.txt";asset["sha256"]=app::installer::SHA256(nested);manifest["files"].Push(asset);save();
    if(app::installer::VerifiedNative(root)!=exe)throw std::runtime_error("nested UTF-8 manifest path refused");++checks;
    app::installer::PromoteNative(root,root/L"app-26",manifest);
    auto live=root/L"app/harmony-nativo.exe";
    write(root/L"current.json",R"({"version":26,"directory":"app-26","live":"app"})");
    if(app::installer::VerifiedNative(root)!=live)throw std::runtime_error("fixed path not selected");++checks;
    write(live,"gixture");refused();write(exe,"fixture");
    write(root/L"app/extra.dll","unlisted");refused();
    if(fs::exists(root/L"app/extra.dll"))throw std::runtime_error("tampered live tree was not repaired");++checks;
    Junction(root/L"app/junction",root/L"app-26");refused();fs::remove(root/L"app/junction");
    fs::rename(root/L"app",root/L".retired-app-crash");
    if(app::installer::VerifiedNative(root)!=live)throw std::runtime_error("missing live tree not recovered");++checks;
    fs::create_directory(root/L"app-27");auto next=root/L"app-27/harmony-nativo.exe";write(next,"updated");
    auto updated=manifest;updated["version"]=27;updated["files"]=json::Value::Array();auto nextEntry=entry;nextEntry["sha256"]=app::installer::SHA256(next);updated["files"].Push(nextEntry);
    write(root/L"app-27/manifest.json",updated.Dump());
    app::installer::PromoteNative(root,root/L"app-27",updated);
    // The new tree was renamed but the old pointer survived the crash.
    if(app::installer::VerifiedNative(root)!=live||app::installer::SHA256(live)!=entry["sha256"].Str())throw std::runtime_error("uncommitted live tree not recovered");++checks;
    app::installer::PromoteNative(root,root/L"app-27",updated);
    write(root/L"current.json",R"({"version":27,"directory":"app-27","live":"app"})");
    if(app::installer::VerifiedNative(root)!=live||app::installer::SHA256(live)!=nextEntry["sha256"].Str())throw std::runtime_error("update changed the executable path");++checks;
    app::installer::PromoteNative(root,root/L"app-26",manifest);
    write(root/L"current.json",R"({"version":26,"directory":"app-26","live":"app"})");
    if(app::installer::VerifiedNative(root)!=live||app::installer::SHA256(live)!=entry["sha256"].Str())throw std::runtime_error("rollback did not repromote");++checks;
    auto held=CreateFileW(live.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(held==INVALID_HANDLE_VALUE)throw std::runtime_error("busy fixture failed");
    bool busy=false;try{app::installer::PromoteNative(root,root/L"app-27",updated);}catch(...){busy=true;}CloseHandle(held);
    if(!busy)throw std::runtime_error("promotion accepted locked live files");++checks;
    std::cout<<"PASS: "<<checks<<" native launcher integrity checks\n";
  }catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
