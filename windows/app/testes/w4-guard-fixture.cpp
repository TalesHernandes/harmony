#include "../src/test-process-guard.h"
#include <cstdio>
#include <string>
int wmain(int argc,wchar_t** argv) {
  if(argc==2&&std::wstring(argv[1])==L"--parent") {
    wchar_t path[MAX_PATH]{},pid[32]{};GetModuleFileNameW(nullptr,path,MAX_PATH);swprintf_s(pid,L"%lu",GetCurrentProcessId());SetEnvironmentVariableW(L"HARMONY_TEST_PARENT_PID",pid);
    std::wstring command=L"\""+std::wstring(path)+L"\"";STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 4;
    CloseHandle(child.hThread);CloseHandle(child.hProcess);char byte;DWORD read;ReadFile(GetStdHandle(STD_INPUT_HANDLE),&byte,1,&read,nullptr);ExitProcess(0);
  }
  if(argc==4&&std::wstring(argv[1])==L"--watch") {
    HANDLE handles[2]={OpenProcess(SYNCHRONIZE,FALSE,wcstoul(argv[2],nullptr,10)),OpenProcess(SYNCHRONIZE,FALSE,wcstoul(argv[3],nullptr,10))};
    if(!handles[0]||!handles[1])return 5;
    printf("watching\n");fflush(stdout);
    DWORD status=WaitForMultipleObjects(2,handles,TRUE,5000);
    CloseHandle(handles[0]);CloseHandle(handles[1]);return status==WAIT_OBJECT_0?0:1;
  }
  if(argc>1&&std::wstring(argv[1])==L"--child"){Sleep(12000);return 0;}
  if(!app::GuardTestParent())return 3;
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
  std::wstring command=L"\""+std::wstring(path)+L"\" --child";
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
  if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return 4;
  printf("%lu %lu\n",GetCurrentProcessId(),child.dwProcessId);fflush(stdout);
  CloseHandle(child.hThread);CloseHandle(child.hProcess);Sleep(12000);return 0;
}
