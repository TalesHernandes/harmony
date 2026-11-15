#include "perf.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <iomanip>

namespace perf {
namespace {

struct Prev {
  uint64_t in = 0, out = 0, dropped = 0;
  int64_t busy_ns = 0;
};

std::mutex& Mu() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::unique_ptr<Stage>>& Stages() {
  static auto* m = new std::map<std::string, std::unique_ptr<Stage>>;
  return *m;
}
uint64_t FileTime100ns(const FILETIME& f) {
  return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
uint64_t ThreadCpu(HANDLE h,uint64_t* created=nullptr) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  if(created)*created=FileTime100ns(c);
  return FileTime100ns(k) + FileTime100ns(u);
}
uint64_t ProcessCpu() {
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  return FileTime100ns(k) + FileTime100ns(u);
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// Where an unnamed thread started: the module its start address is in
// ("nvEncodeAPI64.dll", "AudioSes.dll"), read through ntdll's
// NtQueryInformationThread (ThreadQuerySetWin32StartAddress = 9). The
// threads Windows and the drivers start in our process do not name
// themselves, and "(sem nome)" was a fifth of the processor (06/10/2026).
std::string StartModule(HANDLE thread) {
  using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static auto query =
      reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  void* start = nullptr;
  if (!query || query(thread, 9, &start, sizeof start, nullptr) != 0 || !start) return {};
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(start), &mod))
    return {};
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
  if (!n) return {};
  std::wstring w(path, n);
  return Narrow(w.substr(w.find_last_of(L"\\/") + 1));
}

// Enumerate only this process. Toolhelp walks every thread of every process
// on this machine, which cost ~40ms per sample on the owner's desktop.
std::vector<HANDLE> ProcessThreads() {
  using Next=LONG(NTAPI*)(HANDLE,HANDLE,ACCESS_MASK,ULONG,ULONG,PHANDLE);
  static auto next=reinterpret_cast<Next>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtGetNextThread"));
  std::vector<HANDLE> result;
  if(next){HANDLE previous=nullptr,current=nullptr;
    while(next(GetCurrentProcess(),previous,THREAD_QUERY_INFORMATION,0,0,&current)==0){result.push_back(current);previous=current;}
    if(!result.empty())return result;
  }
  HANDLE list=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
  if(list!=INVALID_HANDLE_VALUE){THREADENTRY32 entry{sizeof entry};for(BOOL ok=Thread32First(list,&entry);ok;ok=Thread32Next(list,&entry))if(entry.th32OwnerProcessID==GetCurrentProcessId())if(auto h=OpenThread(THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID))result.push_back(h);CloseHandle(list);}
  return result;
}

}  // namespace

struct Reader {
  static Prev Read(const Stage& s) {
    return {s.in_.load(), s.out_.load(), s.dropped_.load(), s.busy_ns_.load()};
  }
  static int64_t TakeMax(Stage& s) { return s.max_ns_.exchange(0); }
  static int64_t Value(const Stage& s) { return s.value_.load(); }
};

Stage& Get(const std::string& name) {
  std::lock_guard<std::mutex> lock(Mu());
  auto& slot = Stages()[name];
  if (!slot) slot = std::make_unique<Stage>(name);
  return *slot;
}

void NameThread(const std::wstring& name) {
  SetThreadDescription(GetCurrentThread(), name.c_str());
}

Snapshot TakeSnapshot() {
  static std::map<std::string, Prev> prev;
  static int64_t last_ns = Stage::Now();
  static uint64_t last_proc = ProcessCpu();

  Snapshot snap;
  int64_t now = Stage::Now();
  snap.seconds = double(now - last_ns) / 1e9;
  last_ns = now;
  double interval_100ns = snap.seconds * 1e7;

  std::vector<Stage*> stages;
  {
    std::lock_guard<std::mutex> lock(Mu());
    stages.reserve(Stages().size());
    for (auto& [name, stage] : Stages()) stages.push_back(stage.get());
  }
  // Stages live for the process lifetime. Registry users never wait for Windows queries.
  for (auto* stage : stages) {
    const auto& name = stage->name();
    Prev cur = Reader::Read(*stage);
    Prev& p = prev[name];
    StageRow row;
    row.name = name;
    row.in = cur.in - p.in;
    row.out = cur.out - p.out;
    row.dropped = cur.dropped - p.dropped;
    int64_t busy = cur.busy_ns - p.busy_ns;
    row.avg_ms = row.out ? double(busy) / double(row.out) / 1e6 : 0;
    row.max_ms = double(Reader::TakeMax(*stage)) / 1e6;
    row.value = Reader::Value(*stage);
    p = cur;
    snap.stages.push_back(row);
  }
  // Every thread of the process, not only ours: libwebrtc's encoder, pacer
  // and network queues and Windows' audio threads are where the processor
  // goes, and they name themselves (SetThreadDescription). Grouped by name,
  // most expensive first.
  {
    static std::map<DWORD, uint64_t> last;  // thread id -> cpu at the previous snapshot
    static std::map<std::string,uint64_t> lifetime;
    std::map<DWORD, uint64_t> now_cpu;
    std::map<std::string, double> by_name;
    static std::map<std::pair<DWORD,uint64_t>,std::string> names;
    std::map<std::pair<DWORD,uint64_t>,std::string> liveNames;
    {
      for (auto h:ProcessThreads()) {
        auto id=GetThreadId(h);
        ++snap.thread_count;
        uint64_t created=0,cpu=ThreadCpu(h,&created);
        auto key=std::make_pair(id,created);auto cached=names.find(key);
        std::string name=cached==names.end()?"":cached->second;
        if(name.empty()){
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
          name = Narrow(desc);
          LocalFree(desc);
        }
        if (name.empty()) {
          std::string mod = StartModule(h);
          name = mod.empty() ? "(sem nome)" : "(sem nome) " + mod;
        }
        }
        liveNames[key]=name;
        CloseHandle(h);
        now_cpu[id] = cpu;
        auto it = last.find(id);
        // A thread id Windows gave to a new thread since the last reading
        // has a smaller count than the old one: counted from here.
        uint64_t before = cached==names.end() || it == last.end() || it->second > cpu ? cpu : it->second;
        if (interval_100ns > 0) by_name[name] += 100.0 * double(cpu - before) / interval_100ns;
        lifetime[name]+=cached==names.end()?cpu:cpu-before;
      }
    }
    names.swap(liveNames);
    last.swap(now_cpu);
    for (auto& [name, pct] : by_name) snap.threads.push_back({name, pct});
    std::sort(snap.threads.begin(), snap.threads.end(),
              [](const ThreadRow& a, const ThreadRow& b) { return a.cpu_percent > b.cpu_percent; });
    for(auto& [name,cpu]:lifetime)snap.top_threads.push_back({name,0,double(cpu)/10000.});
    std::sort(snap.top_threads.begin(),snap.top_threads.end(),[](auto& a,auto& b){return a.total_cpu_ms>b.total_cpu_ms;});
    if(snap.top_threads.size()>5)snap.top_threads.resize(5);
  }
  uint64_t proc = ProcessCpu();
  snap.process_cpu_percent = interval_100ns > 0 ? 100.0 * double(proc - last_proc) / interval_100ns : 0;
  last_proc = proc;
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {snap.working_set_bytes = pmc.WorkingSetSize;snap.private_bytes=pmc.PrivateUsage;}
  DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);snap.handles=handles;
  snap.gdi_objects=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);snap.user_objects=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
  return snap;
}

std::string Format(const Snapshot& s) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(1);
  o << "perf cpu=" << s.process_cpu_percent << "% ram=" << (s.working_set_bytes >> 20) << "MB\n";
  for (const auto& r : s.stages) {
    o << "  " << r.name << " in=" << r.in << " out=" << r.out << " drop=" << r.dropped
      << " avg=" << std::setprecision(2) << r.avg_ms << "ms max=" << r.max_ms << "ms"
      << std::setprecision(1);
    if (r.value) o << " v=" << r.value;
    o << "\n";
  }
  for (const auto& t : s.threads)
    if (t.cpu_percent >= 0.5) o << "  tarefa " << t.name << " cpu=" << t.cpu_percent << "%\n";
  return o.str();
}

}  // namespace perf
