#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace app {
std::filesystem::path DataDirectory();
std::filesystem::path Executable();
std::string Environment(const wchar_t* name);
bool SaveSession(const std::string& base, const std::string& cookie);
std::string LoadSession(const std::string& base);
bool DeleteSession();
std::wstring StartupCommand(const std::filesystem::path& executable);
bool StartupEnabled();
bool SetStartup(bool enabled);
bool CopyDiagnostic(HWND owner);
// The same filtered log as text, newest last, at most `limit` bytes.
std::string DiagnosticLog(size_t limit);
// The log's lines of the last `seconds` (by their clock), newest kept when
// they pass `limit`: a report that covers a whole call.
std::string DiagnosticLogSince(double seconds, size_t limit);
constexpr wchar_t AppID[] = L"taleshernandes.harmony";
// Stable per-user COM identity; Audit must never replace the installed server.
inline constexpr CLSID ToastActivator{0x549d187e,0x812e,0x46f4,{0x9a,0xe0,0xd7,0x54,0xc8,0x9f,0x43,0xa1}};
inline constexpr CLSID ToastAuditActivator{0xb9588a4c,0x5728,0x454e,{0x84,0x28,0xb6,0x70,0xba,0xf8,0x0e,0x76}};
bool CreateShortcut(const std::filesystem::path& link, const std::filesystem::path& target,
                    const std::wstring& aumid = AppID, const CLSID& activator = ToastActivator);
}
