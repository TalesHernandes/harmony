#pragma once
#include <windows.h>
#include "../../native/src/media-policy.h"
#include <string>
#include <vector>
namespace app {
struct ScreenChoice {
  HWND window=nullptr; HMONITOR monitor=nullptr; uint32_t pid=0;
  std::wstring title,executable; int width=0,height=0; bool minimized=false,primary=false;
};
std::vector<ScreenChoice> ScreenSources();
}
