#pragma once
#include "media-sources.h"
#include <cstdint>
namespace app {
struct PreviewImage { int width=0,height=0; std::vector<uint8_t> pixels; };
PreviewImage Preview(ScreenChoice choice);
PreviewImage WindowIcon(HWND window);
}
