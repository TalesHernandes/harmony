// The web's existing WOFF2 asset, unpacked by Windows DirectWrite, without
// downloading a font or adding a build dependency.
#include <windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  std::ifstream in(std::filesystem::path(argv[1]), std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  if (bytes.empty()) return 1;
  Microsoft::WRL::ComPtr<IDWriteFactory5> factory;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
              reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) return 1;
  Microsoft::WRL::ComPtr<IDWriteFontFileStream> stream;
  if (FAILED(factory->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, bytes.data(), static_cast<UINT32>(bytes.size()), &stream))) return 1;
  UINT64 size{}; if (FAILED(stream->GetFileSize(&size))) return 1;
  const void* fragment{}; void* context{};
  if (FAILED(stream->ReadFileFragment(&fragment, 0, size, &context))) return 1;
  std::ofstream out(std::filesystem::path(argv[2]), std::ios::binary);
  out.write(static_cast<char const*>(fragment), size); out.close(); stream->ReleaseFileFragment(context);
  return out ? 0 : 1;
}
