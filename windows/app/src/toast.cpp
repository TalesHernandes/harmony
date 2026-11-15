#include "toast.h"
#include "platform.h"
#include "../../native/src/log.h"
#include <notificationactivationcallback.h>
#include <wrl.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <future>

namespace app {
namespace {
using namespace winrt::Windows::UI::Notifications;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;
bool Identity(const std::wstring& id) { return id == AppID || id == L"Harmony.Native.Windows.Audit"; }
std::wstring Wide(const std::string& s) { return std::wstring(winrt::to_hstring(s)); }
std::wstring Escape(const std::wstring& text) {
  std::wstring out;
  for (auto c : text) switch(c) {
    case L'&': out += L"&amp;"; break;
    case L'<': out += L"&lt;"; break;
    case L'>': out += L"&gt;"; break;
    case L'\"': out += L"&quot;"; break;
    case L'\'': out += L"&apos;"; break;
    default: if (c >= 32 || c == 9 || c == 10 || c == 13) out += c;
  }
  return out;
}
std::string Encode(const std::string& s) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
bool Decode(const std::string& s, std::string& out) {
  auto hex=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
  for(size_t i=0;i<s.size();++i) {
    unsigned char c=s[i];
    if(c=='%') { if(i+2>=s.size())return false; int a=hex(s[++i]),b=hex(s[++i]); if(a<0||b<0)return false; c=static_cast<unsigned char>((a<<4)|b); }
    if(c<32 || c==127) return false;
    out+=char(c);
  }
  return true;
}
struct CallbackState {
  std::mutex mutex;
  std::wstring aumid;
  std::function<void(ToastAction)> invoked;
};
class Activation final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback, FtmBase> {
 public:
  std::shared_ptr<CallbackState> state;
  HRESULT STDMETHODCALLTYPE Activate(LPCWSTR aumid, LPCWSTR args, const NOTIFICATION_USER_INPUT_DATA*, ULONG) noexcept override {
    try {
      if(!aumid || !args || state->aumid != aumid) return E_INVALIDARG;
      auto action=toast_detail::Arguments(args);
      if(action.values.empty()) return E_INVALIDARG;
      std::function<void(ToastAction)> callback;
      { std::lock_guard lock(state->mutex); callback=state->invoked; }
      if(callback) callback(std::move(action));
      return S_OK;
    } catch(...) { return E_FAIL; }
  }
};
class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
 public:
  std::shared_ptr<CallbackState> state;
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override {
    if(!result) return E_POINTER;
    *result=nullptr;
    if(outer) return CLASS_E_NOAGGREGATION;
    auto activation=Microsoft::WRL::Make<Activation>();
    if(!activation) return E_OUTOFMEMORY;
    activation->state=state;
    return activation->QueryInterface(iid,result);
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};
// One worker owns all history operations, including removal after a pending Show.
class ToastQueue {
 public:
  ToastQueue() : worker_([this] { Run(); }) {}
  ~ToastQueue() { { std::lock_guard lock(mu_); stop_ = true; } wake_.notify_one(); worker_.join(); }
  void Post(std::function<void()> task) {
    { std::lock_guard lock(mu_); tasks_.push_back(std::move(task)); }
    wake_.notify_one();
  }
  // Leaving the account or the program: a notice not yet shown must not appear
  // afterwards, with nobody left to receive its click, nor delay the exit.
  void Drop() { std::lock_guard lock(mu_); tasks_.clear(); }
 private:
  void Run() {
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      std::function<void()> task;
      { std::unique_lock lock(mu_); wake_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (tasks_.empty()) break;
        task = std::move(tasks_.front()); tasks_.pop_front(); }
      try { task(); } catch (...) { logf("toast completion failed"); }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
  }
  std::mutex mu_; std::condition_variable wake_; std::deque<std::function<void()>> tasks_;
  bool stop_ = false; std::thread worker_;
};
ToastQueue& Queue() { static ToastQueue queue; return queue; }
std::wstring current;
DWORD cookie=0;
std::shared_ptr<CallbackState> callbacks;
bool Has(const std::wstring& tag, const std::wstring& identity) {
  for(auto const& toast : ToastNotificationManager::History().GetHistory(identity))
    if(toast.Tag()==tag && toast.Group()==toast_detail::Group) return true;
  return false;
}
bool SetString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key{};
  if(RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return false;
  auto result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));
  RegCloseKey(key); return result==ERROR_SUCCESS;
}
}
namespace toast_detail {
const CLSID& Activator(const std::wstring& aumid) { return aumid==L"Harmony.Native.Windows.Audit" ? ToastAuditActivator : ToastActivator; }
std::wstring ClassKey(const std::wstring& aumid) {
  wchar_t id[40]{}; StringFromGUID2(Activator(aumid),id,40);
  return std::wstring(L"Software\\Classes\\CLSID\\")+id;
}
bool Register(HKEY root, const std::wstring& aumid, const std::wstring& executable) {
  if(!Identity(aumid) || executable.empty() || executable.find(L'\"')!=std::wstring::npos) return false;
  return SetString(root,ClassKey(aumid)+L"\\LocalServer32",nullptr,L"\""+executable+L"\"");
}
bool Unregister(HKEY root, const std::wstring& aumid) {
  if(!Identity(aumid)) return false;
  auto result=RegDeleteTreeW(root,ClassKey(aumid).c_str());
  return result==ERROR_SUCCESS || result==ERROR_FILE_NOT_FOUND;
}
std::wstring Xml(const Notice& notice) {
  const bool call=notice.target==Notice::Target::Call;
  const std::string target=call?"call":notice.target==Notice::Target::Room?"room":"conversation";
  const auto args=Wide("target="+target+"&id="+Encode(notice.id));
  std::wstring xml=L"<toast launch=\""+Escape(args)+L"\""+(call?L" scenario=\"incomingCall\"":L"")+L"><visual><binding template=\"ToastGeneric\"><text>";
  xml+=Escape(Wide(notice.title))+L"</text><text>"+Escape(Wide(notice.body))+L"</text></binding></visual>";
  if(call) {
    xml+=L"<actions><action content=\"Atender\" activationType=\"foreground\" arguments=\""+Escape(args+L"&action=accept")+L"\"/>";
    xml+=L"<action content=\"Recusar\" activationType=\"foreground\" arguments=\""+Escape(args+L"&action=decline")+L"\"/></actions>";
  }
  return xml+L"<audio silent=\"true\"/></toast>";
}
ToastAction Arguments(const std::wstring& arguments) {
  if(arguments.size()>4096) return {};
  auto s=winrt::to_string(arguments); ToastAction result;
  for(size_t pos=0;pos<s.size();) {
    auto end=s.find('&',pos); if(end==std::string::npos)end=s.size();
    auto pair=s.substr(pos,end-pos); auto eq=pair.find('=');
    if(eq==std::string::npos) return {};
    std::string key,value;
    if(!Decode(pair.substr(0,eq),key)||!Decode(pair.substr(eq+1),value)||value.empty()||!result.values.emplace(key,value).second) return {};
    pos=end+1;
  }
  auto& v=result.values;
  if(!v.count("target")||!v.count("id"))return {};
  auto target=v.at("target");
  if(target!="call"&&target!="conversation"&&target!="room")return {};
  for(auto const& [key,value]:v) if(key!="target"&&key!="id"&&key!="action")return {};
  if(v.count("action")&&(target!="call"||(v.at("action")!="accept"&&v.at("action")!="decline")))return {};
  return result;
}
}
bool ToastsStart(const std::wstring& aumid, std::function<void(ToastAction)> invoked) {
  // An installer harness may never replace the owner's COM server or history.
  if (!Environment(L"HARMONY_INSTALLER_TEST_ROOT").empty()) return false;
  if(callbacks || !Identity(aumid) || !invoked) return false;
  try {
    auto executable = Executable();
    auto launcher = executable.parent_path().parent_path().parent_path() / L"harmony.exe";
    if (aumid == AppID && std::filesystem::exists(launcher.parent_path() / L".harmony-installer")) executable = launcher;
    if(!toast_detail::Register(HKEY_CURRENT_USER,aumid,executable.wstring())) return false;
    auto state=std::make_shared<CallbackState>(); state->aumid=aumid; state->invoked=std::move(invoked);
    auto factory=Microsoft::WRL::Make<Factory>(); if(!factory)return false;
    factory->state=state;
    DWORD registered=0;
    auto hr=CoRegisterClassObject(toast_detail::Activator(aumid),static_cast<IClassFactory*>(factory.Get()),CLSCTX_LOCAL_SERVER,REGCLS_MULTIPLEUSE,&registered);
    if(FAILED(hr)) { logf("toast factory failed hr=0x%08lx",hr);return false; }
    current=aumid; callbacks=std::move(state); cookie=registered; return true;
  } catch(winrt::hresult_error const& e) { logf("toast start failed hr=0x%08lx",int32_t(e.code()));return false; }
    catch(...) { logf("toast start failed");return false; }
}
void ToastsStop(bool removeNotices) {
  if(callbacks) { std::lock_guard lock(callbacks->mutex); callbacks->invoked={}; }
  // CoRegisterClassObject may return cookie zero; success owns registration.
  if(callbacks) { CoRevokeClassObject(cookie); cookie=0; }
  Queue().Drop();
  if(removeNotices && !current.empty()) Queue().Post([id = current] { try { ToastNotificationManager::History().Clear(id); } catch(...) {} });
  callbacks.reset(); current.clear();
}
static bool ShowNow(const Notice& notice, const std::wstring& identity) {
  if(identity.empty() || notice.id.empty() || notice.tag.empty())return false;
  const char* stage="xml";
  try {
    auto tag=Wide(notice.tag); if(tag.size()>64)return false;
    winrt::Windows::Data::Xml::Dom::XmlDocument xml; xml.LoadXml(toast_detail::Xml(notice));
    stage="properties";
    ToastNotification toast(xml); toast.Tag(tag); toast.Group(toast_detail::Group);
    if(notice.target==Notice::Target::Call)toast.ExpirationTime(winrt::clock::now()+std::chrono::seconds(45));
    // Remove an older copy first: history must confirm THIS Show, not a stale tag.
    stage="remove";
    if(Has(tag,identity)) ToastNotificationManager::History().Remove(tag,toast_detail::Group,identity);
    for(int i=0;i<20 && Has(tag,identity);++i)std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if(Has(tag,identity)) { logf("toast previous tag could not be removed");return false; }
    stage="notifier";
    auto notifier=ToastNotificationManager::CreateToastNotifier(identity);
    auto setting=notifier.Setting();
    if(setting!=NotificationSetting::Enabled) { logf("toast disabled setting=%d",int(setting));return false; }
    stage="show"; notifier.Show(toast);
    stage="history";
    for(int i=0;i<20;++i) { if(Has(tag,identity))return true; std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    if(Has(tag,identity)) ToastNotificationManager::History().Remove(tag,toast_detail::Group,identity);
    logf("toast missing from history after Show");
  } catch(winrt::hresult_error const& e) { logf("toast show failed stage=%s hr=0x%08lx",stage,int32_t(e.code())); }
    catch(...) { logf("toast show failed"); }
  return false;
}
void ToastShowAsync(const Notice& notice, std::function<void(bool)> completed) {
  auto id = callbacks ? current : std::wstring();
  Queue().Post([notice, id, completed = std::move(completed)] { completed(ShowNow(notice, id)); });
}
bool ToastShow(const Notice& notice) {
  auto result = std::make_shared<std::promise<bool>>(); auto future = result->get_future();
  ToastShowAsync(notice, [result](bool shown) { result->set_value(shown); });
  return future.get();
}
void ToastRemove(const std::string& tag) {
  if(current.empty()||tag.empty())return;
  Queue().Post([tag, id = current] {
    try { ToastNotificationManager::History().Remove(Wide(tag),toast_detail::Group,id); }
    catch(winrt::hresult_error const& e) { logf("toast remove failed hr=0x%08lx",int32_t(e.code())); }
    catch(...) { logf("toast remove failed"); }
  });
}
bool ToastsUnregister(const std::wstring& aumid) {
  if(!Identity(aumid) || (callbacks && current==aumid))return false;
  auto result = std::make_shared<std::promise<bool>>(); auto future = result->get_future();
  Queue().Post([aumid, result] {
    try { ToastNotificationManager::History().Clear(aumid); result->set_value(true); }
    catch(...) { result->set_value(false); }
  });
  bool cleared = future.get();
  return toast_detail::Unregister(HKEY_CURRENT_USER,aumid) && cleared;
}
}
