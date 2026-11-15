#pragma once
#include <windows.h>
#include <functional>
#include <map>
#include <string>
#include "notices.h"

namespace app {
struct ToastAction { std::map<std::string, std::string> values; };
// Start/Show/Remove/Stop are submitted by one owner. History operations run
// in FIFO order on a COM worker. Start requires an initialized COM apartment.
// Callbacks are agile: enqueue them onto the UI.
bool ToastsStart(const std::wstring& aumid, std::function<void(ToastAction)> invoked);
// A short-lived -Embedding relay uses false to preserve the UI owner's history.
void ToastsStop(bool removeNotices = true);
// Synchronous confirmation for non-UI callers (tests).
bool ToastShow(const Notice& notice);
// Completion preserves Show's history-confirmed result, never queue acceptance.
void ToastShowAsync(const Notice& notice, std::function<void(bool)> completed);
void ToastRemove(const std::string& tag);
bool ToastsUnregister(const std::wstring& aumid);

namespace toast_detail {
// The web has tags, but no Windows group. Keep a stable group for History.Remove.
inline constexpr wchar_t Group[] = L"harmony";
std::wstring Xml(const Notice& notice);
ToastAction Arguments(const std::wstring& arguments);
const CLSID& Activator(const std::wstring& aumid);
std::wstring ClassKey(const std::wstring& aumid);
// Injectable root for isolated HKCU tests; production passes HKEY_CURRENT_USER.
bool Register(HKEY root, const std::wstring& aumid, const std::wstring& executable);
bool Unregister(HKEY root, const std::wstring& aumid);
}
}
