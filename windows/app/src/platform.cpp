#include "platform.h"
#include "installer-context.h"
#include "../../native/src/net.h"
#include "../../native/src/log.h"
#include <wincrypt.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <fstream>
#include <vector>

namespace app {
std::string Environment(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring result(size, 0);
  DWORD n = GetEnvironmentVariableW(name, result.data(), size);
  if (!n || n >= size) return {};
  result.resize(n);
  return net::Narrow(result);
}
std::filesystem::path Executable() {
  wchar_t path[32768]{};
  DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (!n || n == ARRAYSIZE(path)) throw std::runtime_error("executable path unavailable");
  return path;
}
std::filesystem::path DataDirectory() {
  if (auto test = installer::TestRoot(); !test.empty()) return test / L"data" / L"native";
  // The migrated installation must not borrow or overwrite a standalone
  // prototype's session. Storage still uses the same user/origin-bound DPAPI.
  if (auto installed = installer::InstallRoot(); !installed.empty()) return installed.parent_path() / L"native-data";
  auto executable = Executable();
  if ((executable.filename() == L"harmony.exe" || executable.filename() == L"harmony.next.exe") &&
      std::filesystem::is_regular_file(executable.parent_path() / L".harmony-installer")) return executable.parent_path() / L"native-data";
  auto root = Environment(L"LOCALAPPDATA");
  if (root.empty()) throw std::runtime_error("local data directory unavailable");
  return std::filesystem::path(net::Wide(root)) / L"harmony-nativo";
}
namespace {
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RunName() { return installer::InstallRoot().empty() ? L"harmony-nativo" : L"harmony"; }

DATA_BLOB Blob(std::string const& text) {
  return {static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))};
}
}
bool SaveSession(const std::string& base, const std::string& cookie) {
  // Bind to both Windows user and server; never send a restored cookie to
  // another HARMONY_URL. DPAPI supplies integrity and user-bound encryption.
  auto input = Blob(cookie), entropy = Blob(base);
  DATA_BLOB encrypted{};
  if (!CryptProtectData(&input, L"harmony nativo", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
    logf("session protection failed, Windows error 0x%lx", GetLastError()); return false;
  }
  bool ok = false;
  try {
    auto root = DataDirectory();
    std::filesystem::create_directories(root);
    auto pending = root / L"session.pending";
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char*>(encrypted.pbData), encrypted.cbData);
    out.close();
    if (out) ok = MoveFileExW(pending.c_str(), (root / L"session.dpapi").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(pending.c_str());
  } catch (...) { logf("session file write failed"); }
  LocalFree(encrypted.pbData);
  return ok;
}
std::string LoadSession(const std::string& base) {
  try {
    auto path = DataDirectory() / L"session.dpapi";
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 16384) return {};
    std::string bytes(static_cast<size_t>(size), 0);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(bytes.data(), bytes.size())) return {};
    auto input = Blob(bytes), entropy = Blob(base);
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return {};
    std::string result(reinterpret_cast<char*>(plain.pbData), plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
    return result;
  } catch (...) { return {}; }
}
bool DeleteSession() {
  try {
    auto path = DataDirectory() / L"session.dpapi";
    return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
  } catch (...) { return false; }
}
std::wstring StartupCommand(const std::filesystem::path& executable) {
  auto version = executable.parent_path();
  auto root = version.parent_path();
  if (std::filesystem::exists(root.parent_path() / L".harmony-installer"))
    return L"\"" + (root.parent_path() / L"harmony.exe").wstring() + L"\" --tray";
  // An old running version must always register the stable launcher. An
  // unpacked build has no installer-owned launcher and cannot enable startup.
  if (!std::filesystem::exists(root / L".harmony-native-install") ||
      !std::filesystem::exists(root / L"launch.ps1")) return {};
  return L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
      (root / L"launch.ps1").wstring() + L"\" -Bandeja";
}
bool StartupEnabled() {
  if (!installer::TestRoot().empty()) return false;
  DWORD bytes = 0;
  return RegGetValueW(HKEY_CURRENT_USER, RunKey, RunName(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}
bool SetStartup(bool enabled) {
  if (!installer::TestRoot().empty()) return false;
  auto command = StartupCommand(Executable());
  if (enabled && command.empty()) return false;
  HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  LSTATUS result = enabled ? RegSetValueExW(key, RunName(), 0, REG_SZ, reinterpret_cast<BYTE const*>(command.c_str()),
                                          static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)))
                           : RegDeleteValueW(key, RunName());
  RegCloseKey(key);
  return result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND);
}
std::string DiagnosticLog(size_t limit) {
  std::string text;
  for (auto const& line : LogLines()) text += FilterLog(line) + "\n";
  // Keep the newest lines: they hold what happened just before sending.
  if (text.size() > limit) { text.erase(0, text.size() - limit); if (auto cut = text.find('\n'); cut != std::string::npos) text.erase(0, cut + 1); }
  return text;
}
std::string DiagnosticLogSince(double seconds, size_t limit) {
  auto lines = LogLines();
  SYSTEMTIME now{}; GetLocalTime(&now);
  int today = now.wHour * 3600 + now.wMinute * 60 + now.wSecond;
  // Walk back from the newest line; ages only grow, and one that suddenly
  // shrinks is a line from the day before: stop there too.
  size_t first = lines.size(); int previous = -1;
  for (size_t i = lines.size(); i-- > 0;) {
    auto& line = lines[i]; int h, m, s;
    if (line.size() < 8 || sscanf_s(line.c_str(), "%2d:%2d:%2d", &h, &m, &s) != 3) { first = i; continue; }
    int age = (today - (h * 3600 + m * 60 + s) + 86400) % 86400;
    if (age > seconds || (previous >= 0 && age + 3600 < previous)) break;
    previous = std::max(previous, age); first = i;
  }
  std::string text;
  for (size_t i = first; i < lines.size(); ++i) text += FilterLog(lines[i]) + "\n";
  if (text.size() > limit) { text.erase(0, text.size() - limit); if (auto cut = text.find('\n'); cut != std::string::npos) text.erase(0, cut + 1); }
  return text;
}
bool CopyDiagnostic(HWND owner) {
  std::string text;
  for (auto const& line : LogLines()) text += FilterLog(line) + "\r\n";
  auto wide = net::Wide(text);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size()+1)*sizeof(wchar_t));
  if (!memory) return false;
  auto ptr = GlobalLock(memory);
  if (!ptr) { GlobalFree(memory); return false; }
  memcpy(ptr, wide.c_str(), (wide.size()+1)*sizeof(wchar_t)); GlobalUnlock(memory);
  if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
  bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  CloseClipboard();
  if (!ok) GlobalFree(memory);
  return ok;
}
bool CreateShortcut(const std::filesystem::path& link, const std::filesystem::path& target,
                    const std::wstring& aumid, const CLSID& toastActivator) {
  IShellLinkW* shortcut = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&shortcut)))) return false;
  bool ok = SUCCEEDED(shortcut->SetPath(target.c_str())) &&
            SUCCEEDED(shortcut->SetWorkingDirectory(target.parent_path().c_str())) &&
            SUCCEEDED(shortcut->SetDescription(L"harmony")) &&
            SUCCEEDED(shortcut->SetIconLocation(target.c_str(),0));
  IPropertyStore* properties = nullptr; PROPVARIANT id{};
  if (ok && SUCCEEDED(shortcut->QueryInterface(IID_PPV_ARGS(&properties)))) {
    // Pinning policy must precede ID; Windows ignores later policy changes.
    PROPVARIANT flag{}; InitPropVariantFromBoolean(FALSE,&flag);
    PROPVARIANT activator{};
    ok = SUCCEEDED(InitPropVariantFromCLSID(toastActivator,&activator)) &&
         SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID,activator)) &&
         SUCCEEDED(properties->SetValue(PKEY_AppUserModel_PreventPinning,flag)) &&
         SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ExcludeFromShowInNewInstall,flag)) &&
         SUCCEEDED(InitPropVariantFromString(aumid.c_str(),&id)) &&
         SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ID,id)) && SUCCEEDED(properties->Commit());
    PropVariantClear(&flag);
    PropVariantClear(&activator);
    PropVariantClear(&id); properties->Release();
  } else ok = false;
  IPersistFile* file = nullptr;
  if (ok && SUCCEEDED(shortcut->QueryInterface(IID_PPV_ARGS(&file)))) {
    ok = SUCCEEDED(file->Save(link.c_str(),TRUE)); file->Release();
  } else ok = false;
  shortcut->Release();
  // Notify the Shell only after the complete shortcut has been persisted.
  if(ok) SHChangeNotify(SHCNE_CREATE,SHCNF_PATHW | SHCNF_FLUSH,link.c_str(),nullptr);
  return ok;
}
}
