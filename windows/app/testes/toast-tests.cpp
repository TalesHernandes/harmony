#include "../src/toast.h"
#include "../src/platform.h"
#include <notificationactivationcallback.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
#include <future>
#include <vector>

namespace {
void Check(bool ok,const char* name) { if(!ok)throw std::runtime_error(name); std::cout<<"PASS: "<<name<<'\n'; }
std::wstring Value(HKEY root,const std::wstring& path) {
  wchar_t value[32768]{}; DWORD bytes=sizeof(value);
  return RegGetValueW(root,path.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS?value:L"";
}
void Unit() {
  using namespace app;
  Check(!ToastShow({Notice::Target::Conversation,"x","Title","Metadata","x"}),"unstarted transport requests balloon fallback");
  std::promise<void> completed; auto finished = completed.get_future();
  std::vector<int> order; bool workerOnly = true; auto owner = std::this_thread::get_id();
  for (int i = 0; i < 3; ++i) ToastShowAsync({}, [&, i](bool shown) {
    workerOnly = workerOnly && std::this_thread::get_id() != owner && !shown;
    order.push_back(i); if (i == 2) completed.set_value();
  });
  Check(finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "async confirmations complete without a UI pump");
  Check(workerOnly && order == std::vector<int>{0,1,2}, "toast confirmations run off caller thread in submission order and retain failure result");
  Notice call{Notice::Target::Call,"id&=\"á", "A < B & \"C\"", "Chamada direta", "call-test"};
  auto xml=toast_detail::Xml(call);
  winrt::Windows::Data::Xml::Dom::XmlDocument document; document.LoadXml(xml);
  Check(xml.find(L"A &lt; B &amp; &quot;C&quot;")!=xml.npos,"XML escapes names without markup injection");
  Check(xml.find(L"scenario=\"incomingCall\"")!=xml.npos && xml.find(L"<audio silent=\"true\"/>")!=xml.npos,"incoming call scenario is silent");
  Check(document.SelectNodes(L"/toast/actions/action").Size()==2,"call has two foreground actions");
  for(auto node:document.SelectNodes(L"/toast/actions/action")) {
    auto arguments=winrt::unbox_value<winrt::hstring>(node.Attributes().GetNamedItem(L"arguments").NodeValue());
    auto action=toast_detail::Arguments(std::wstring(arguments));
    Check(action.values.at("target")=="call" && action.values.at("id")==call.id,"button arguments round trip UTF-8 and reserved characters");
    auto label=winrt::unbox_value<winrt::hstring>(node.Attributes().GetNamedItem(L"content").NodeValue());
    Check(action.values.at("action")== (label==L"Atender"?"accept":"decline") && winrt::unbox_value<winrt::hstring>(node.Attributes().GetNamedItem(L"activationType").NodeValue())==L"foreground","button label matches action");
  }
  Notice message{Notice::Target::Conversation,"conversation-a","Pessoa <&\"", "Te mencionou em #canal <&\"", "conversation-a"};
  document.LoadXml(toast_detail::Xml(message));
  Check(document.SelectNodes(L"/toast/actions").Size()==0,"message has no call buttons");
  auto launch=document.DocumentElement().GetAttribute(L"launch");
  auto body=toast_detail::Arguments(std::wstring(launch));
  Check(body.values.at("target")=="conversation" && body.values.at("id")==message.id,"message body opens its own conversation");
  Check(document.SelectNodes(L"//text").Size()==2,"only notice metadata is rendered");
  for(auto text:{L"target=call&id=x&action=erase",L"target=call&id=x&id=y",L"target=conversation&id=x&action=accept",L"target=call&id=%00",L"target=call&id=%GG",L"target=call",L"target=unknown&id=x",L"target=call&id=x&extra=y"})
    Check(toast_detail::Arguments(text).values.empty(),"malformed or unrecognized activation rejected");
  auto path=L"Software\\HarmonyToastTests\\"+std::to_wstring(GetCurrentProcessId());
  HKEY root{};
  Check(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&root,nullptr)==ERROR_SUCCESS,"isolated HKCU root created");
  try {
    const std::wstring audit=L"Harmony.Native.Windows.Audit";
    auto key=toast_detail::ClassKey(audit);
    Check(toast_detail::Register(root,audit,L"C:\\Program Files\\Harmony\\app.exe"),"test-root COM registration");
    Check(Value(root,key+L"\\LocalServer32")==L"\"C:\\Program Files\\Harmony\\app.exe\"","LocalServer32 quotes executable without shell arguments");
    Check(toast_detail::Register(root,audit,L"C:\\updated\\app.exe") && Value(root,key+L"\\LocalServer32")==L"\"C:\\updated\\app.exe\"","upgrade refreshes COM server path");
    Check(toast_detail::ClassKey(AppID)!=key,"audit has independent CLSID");
    Check(!toast_detail::Register(root,L"other",L"x") && !toast_detail::Unregister(root,L"other"),"cleanup refuses foreign identities");
    Check(toast_detail::Unregister(root,audit)&&toast_detail::Unregister(root,audit)&&Value(root,key+L"\\LocalServer32").empty(),"unregister removes own COM tree idempotently");
  } catch(...) { RegCloseKey(root); RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str()); throw; }
  RegCloseKey(root); Check(RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str())==ERROR_SUCCESS,"isolated HKCU root removed");
}
bool History(const std::wstring& tag) {
  for(auto toast:winrt::Windows::UI::Notifications::ToastNotificationManager::History().GetHistory(L"Harmony.Native.Windows.Audit"))if(toast.Tag()==tag)return true;
  return false;
}
void Audit() {
  using namespace app;
  const std::wstring audit=L"Harmony.Native.Windows.Audit";
  PWSTR programs{}; winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs,0,nullptr,&programs));
  auto link=std::filesystem::path(programs)/L"Harmony Native Windows Audit.lnk"; CoTaskMemFree(programs);
  Check(!std::filesystem::exists(link),"audit never overwrites an existing shortcut");
  Check(Value(HKEY_CURRENT_USER,toast_detail::ClassKey(audit)+L"\\LocalServer32").empty(),"audit never overwrites an existing COM server");
  try {
    Check(CreateShortcut(link,Executable(),audit,ToastAuditActivator),"audit Start Menu shortcut created");
    Microsoft::WRL::ComPtr<IShellLinkW> shell; Microsoft::WRL::ComPtr<IPersistFile> file; Microsoft::WRL::ComPtr<IPropertyStore> props;
    winrt::check_hresult(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&shell)));
    winrt::check_hresult(shell.As(&file));winrt::check_hresult(file->Load(link.c_str(),STGM_READWRITE));winrt::check_hresult(shell.As(&props));
    PROPVARIANT value{};
    Check(SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID,&value))&&value.vt==VT_LPWSTR&&std::wstring(value.pwszVal)==audit,"audit shortcut never uses installed AUMID");PropVariantClear(&value);
    Check(SUCCEEDED(props->GetValue(PKEY_AppUserModel_ToastActivatorCLSID,&value))&&value.vt==VT_CLSID&&IsEqualGUID(*value.puuid,ToastAuditActivator),"audit shortcut uses separate activator");PropVariantClear(&value);
    std::atomic<int> invoked=0;
    Check(ToastsStart(audit,[&](ToastAction a){if(a.values.at("id")=="audit-call")++invoked;}),"real factory registered");
    Microsoft::WRL::ComPtr<INotificationActivationCallback> activation;
    winrt::check_hresult(CoCreateInstance(ToastAuditActivator,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&activation)));
    Check(SUCCEEDED(activation->Activate(audit.c_str(),L"target=call&id=audit-call&action=accept",nullptr,0)) && invoked==1,"real COM activation routes arguments to callback");
    Check(FAILED(activation->Activate(AppID,L"target=call&id=audit-call",nullptr,0)),"COM rejects another sender");
    std::atomic<bool> threaded=false;
    std::thread other([&] {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      Microsoft::WRL::ComPtr<INotificationActivationCallback> callback;
      threaded=SUCCEEDED(CoCreateInstance(ToastAuditActivator,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&callback))) &&
          SUCCEEDED(callback->Activate(audit.c_str(),L"target=call&id=audit-call&action=decline",nullptr,0));
      callback.Reset(); winrt::uninit_apartment();
    }); other.join();
    Check(threaded && invoked==2,"factory callback works from another COM thread");
    Notice call{Notice::Target::Call,"audit-call","harmony — teste W6","Chamada de teste", "call-audit-call"};
    Check(ToastShow(call),"real Show confirmed in Audit history");
    Check(History(L"call-audit-call"),"independent GetHistory finds call");
    for(auto toast:winrt::Windows::UI::Notifications::ToastNotificationManager::History().GetHistory(audit))if(toast.Tag()==L"call-audit-call") {
      auto expiration=toast.ExpirationTime();
      auto remaining=expiration?std::chrono::duration_cast<std::chrono::seconds>(expiration.Value()-winrt::clock::now()).count():0;
      Check(remaining>=40 && remaining<=45,"real call expires within 45 seconds");
      Check(toast.Group()==toast_detail::Group,"real call keeps stable removable group");
    }
    ToastRemove(call.tag);
    for(int i=0;i<40 && History(L"call-audit-call");++i)std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Check(!History(L"call-audit-call"),"real Remove clears call history");
    Notice message{Notice::Target::Conversation,"audit-a","Nome < & \"teste\"","Mensagem direta","audit-a"};
    Check(ToastShow(message),"real message with escaped name enters history");
    message.id="audit-b";message.tag="audit-b";
    Check(ToastShow(message)&&History(L"audit-a")&&History(L"audit-b"),"real independent conversations coexist");
    Check(ToastShow(message),"repeated tag replaces its own notice");
    ToastRemove("audit-a");
    for(int i=0;i<40 && History(L"audit-a");++i)std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Check(!History(L"audit-a")&&History(L"audit-b"),"removing one conversation preserves another");
    ToastsStop(false);
    Check(History(L"audit-b"),"relay Stop preserves UI owner history");
    Check(ToastsStart(audit,[&](ToastAction){++invoked;}),"factory restarts after relay stop");
    ToastsStop();
    for(int i=0;i<40 && History(L"audit-b");++i)std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Check(!History(L"audit-b"),"Stop removes remaining program notices");
    activation->Activate(audit.c_str(),L"target=call&id=audit-call&action=decline",nullptr,0);
    Check(invoked==2,"stopped factory callback cannot reach UI");
    Check(ToastsUnregister(audit),"real Audit history and CLSID cleaned");
    Check(Value(HKEY_CURRENT_USER,toast_detail::ClassKey(audit)+L"\\LocalServer32").empty(),"real Audit COM key absent");
  } catch(...) { ToastsStop();ToastsUnregister(audit);std::filesystem::remove(link);SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW|SHCNF_FLUSH,link.c_str(),nullptr);throw; }
  std::filesystem::remove(link); SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW|SHCNF_FLUSH,link.c_str(),nullptr);
  std::cout<<"PASS: Audit shortcut removed\n";
}
}
int main(int argc,char** argv) {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  try { Unit(); if(argc>1 && std::string(argv[1])=="--audit")Audit(); std::cout<<"PASS: classic toast tests\n"; }
  catch(winrt::hresult_error const& e) { std::cerr<<"FAIL HRESULT 0x"<<std::hex<<uint32_t(e.code())<<" "<<winrt::to_string(e.message())<<'\n';return 1; }
  catch(std::exception const& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
}
