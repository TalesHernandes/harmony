// Read-only timing of what the launcher does before starting the program.
// Calls the real functions (verified-launch.h); never promotes or writes.
#include "../instalar/verified-launch.h"
#include <iostream>
namespace setup = app::installer;
static double Ms(LARGE_INTEGER a, LARGE_INTEGER b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart); }
template <class F> static double Time(F f) { LARGE_INTEGER a, b; QueryPerformanceCounter(&a); f(); QueryPerformanceCounter(&b); return Ms(a, b); }
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  std::filesystem::path root = argv[1];           // ...\Programs\harmony
  auto native = root / L"native";
  try {
    auto pointer = setup::ReadJSON(native / L"current.json");
    auto depot = native / net::Wide(pointer["directory"].Str());
    auto manifest = setup::ReadJSON(depot / L"manifest.json");
    auto live = native / L"app";
    std::cout << "files_in_manifest=" << manifest["files"].Size() << "\n";
    // Persist only in the harness, never in the measured installation.
    std::filesystem::path recordPath=argv[2];
    auto snapshot=setup::ScanNative(live,depot,manifest,root);
    setup::HashNativeFiles(live,manifest);
    {std::ofstream out(recordPath);out<<setup::NativeRecord(pointer["version"].Int(),snapshot).Dump();}
    for (int round = 0; round < 3; ++round) {
      double quick=Time([&] {
        auto selected=setup::ReadJSON(native/L"current.json");
        auto currentManifest=setup::ReadJSON(depot/L"manifest.json");
        auto current=setup::ScanNative(live,depot,currentManifest,root);
        if(!setup::NativeRecordMatches(setup::ReadJSON(recordPath),selected["version"].Int(),current))throw std::runtime_error("metadata changed");
      });
      double verify=Time([&] {
        auto selected=setup::ReadJSON(native/L"current.json");
        auto currentManifest=setup::ReadJSON(depot/L"manifest.json");
        auto current=setup::ScanNative(live,depot,currentManifest,root);
        setup::HashNativeFiles(live,currentManifest);
        for(auto const& [path,stat]:current.files)if(setup::NativeStat(live/path)!=stat)throw std::runtime_error("metadata changed");
      });
      double next = -1;
      if (std::filesystem::is_regular_file(root / L"harmony.next.exe")) next = Time([&] { (void)setup::SHA256(root / L"harmony.next.exe"); });
      std::cout << "round=" << round << " quick_ms=" << quick << " full_ms=" << verify << " pending_update_hash_ms=" << next << "\n";
    }
  } catch (const std::exception& e) { std::cout << "FAILED: " << e.what() << "\n"; return 1; }
  return 0;
}
