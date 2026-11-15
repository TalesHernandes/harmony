#pragma once
#include "gpu.h"
#include "diagnostic-state.h"
#include "../src/perf.h"
#include "api/make_ref_counted.h"
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace gpu {
using Microsoft::WRL::ComPtr;
inline void NameThreadOnce(const wchar_t* name) {
  // perf registers a handle on every call, so naming per frame would leak one
  // handle per frame and report the same thread many times in the panel.
  thread_local bool named = false;
  if (!named) { perf::NameThread(name); named = true; }
}
inline void Check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    std::ostringstream text;
    text << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    throw std::runtime_error(text.str());
  }
}
// Multithread protection covers individual context calls. This lock also keeps
// state-setting and the following video blit together across capture/present.
std::recursive_mutex& ContextMutex();
ComPtr<ID3D11DeviceContext> Context();
ComPtr<ID3D11Texture2D> Texture(int width, int height, DXGI_FORMAT format);
// Drop strong references held for reuse between frames when an encoder ends.
void ReleaseVideoProcessorCache();
// Lets go of the cached views onto one texture: a swap chain cannot resize
// its buffers while anything still holds them.
void ForgetOutputViews(ID3D11Texture2D* texture);

void Blit(const D3DFrameBuffer& input, ID3D11Texture2D* output,
          const RECT& source, const RECT& destination, int rotation = 0,
          // false for an output texture made for one frame: its view is not cached.
          bool keep_view = true);
webrtc::scoped_refptr<D3DFrameBuffer> Scale(const D3DFrameBuffer& input, int width, int height, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM);
void SetActiveEncoderName(std::string name);
// Explicit test injection; production factory behavior is unchanged by default.
void ConfigureSelftest(bool force_fallback, int fail_after);
bool ForceFallback();
int FailAfter();
}  // namespace gpu

