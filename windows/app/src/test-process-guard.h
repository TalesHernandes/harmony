#pragma once
#include <windows.h>
#include <thread>
namespace app {
// Only the disposable harness sets this variable. Each fixture owns a job so
// camera workers die even when Node or the fixture is terminated abruptly.
inline bool GuardTestParent() {
  wchar_t value[32]{};
  if(!GetEnvironmentVariableW(L"HARMONY_TEST_PARENT_PID",value,32))return true;
  DWORD pid=wcstoul(value,nullptr,10);
  HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,pid);
  HANDLE job=CreateJobObjectW(nullptr,nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if(!parent||!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||!AssignProcessToJobObject(job,GetCurrentProcess())) {
    if(parent)CloseHandle(parent);if(job)CloseHandle(job);return false;
  }
  std::thread([parent,job]{WaitForSingleObject(parent,INFINITE);TerminateJobObject(job,1);}).detach();
  // Deliberately retained until process death (closing it kills our own job).
  return true;
}
}
