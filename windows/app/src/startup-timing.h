#pragma once
#include <windows.h>
#include "../../native/src/log.h"
#ifndef HARMONY_PUBLISH
#include "../testes/resource-meter.h"
#endif
namespace startup {
inline void Mark(char const* stage) {
#ifndef HARMONY_PUBLISH
  resource_meter::Mark(stage);
#endif
  struct Origin {
    ULONGLONG tick=GetTickCount64();double loader=0;
    Origin(){FILETIME created{},exited{},kernel{},user{},now{};GetSystemTimePreciseAsFileTime(&now);
      if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
        ULARGE_INTEGER a{},b{};a.LowPart=created.dwLowDateTime;a.HighPart=created.dwHighDateTime;b.LowPart=now.dwLowDateTime;b.HighPart=now.dwHighDateTime;
        if(b.QuadPart>=a.QuadPart)loader=double(b.QuadPart-a.QuadPart)/10000;
      }
    }
  };
  static Origin origin;
  logf("startup stage=%s process_ms=%.1f tick_ms=%llu",stage,origin.loader+double(GetTickCount64()-origin.tick),GetTickCount64());
}
}
