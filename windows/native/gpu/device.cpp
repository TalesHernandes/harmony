#include "internal.h"
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include "api/video/i420_buffer.h"
#include "libyuv.h"
#include "../src/log.h"

namespace gpu {
namespace {
struct DeviceState {
  ComPtr<ID3D11Device> device;
  DeviceState() {
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{};
      Check(adapter->GetDesc1(&desc), "GetDesc1");
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      ComPtr<ID3D11Device> candidate;
      D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
          levels, 2, D3D11_SDK_VERSION, &candidate, nullptr, nullptr))) continue;
      ComPtr<ID3D11VideoDevice> video;
      if (FAILED(candidate.As(&video))) continue;
      device = candidate;
      logf("gpu device vendor=0x%04x device=0x%04x adapter_index=%u",desc.VendorId,desc.DeviceId,i);
      break;
    }
    if (!device) throw std::runtime_error("No hardware D3D11 video device");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D10Multithread> protection;
    Check(context.As(&protection), "ID3D10Multithread");
    protection->SetMultithreadProtected(TRUE);
  }
};
std::unique_ptr<DeviceState>& SharedDevice() {static std::unique_ptr<DeviceState> state;return state;}
size_t& DeviceUsers() {static size_t users=0;return users;}
bool& IdleReleaseEnabled() {
#ifdef HARMONY_W13_LIFETIME
  static bool enabled=true;
#else
  static bool enabled=false;
#endif
  return enabled;
}
}
ID3D11Device* Device() {
  std::lock_guard lock(ContextMutex());
  auto& state=SharedDevice();if(!state)state=std::make_unique<DeviceState>();
  return state->device.Get();
}
std::recursive_mutex& ContextMutex() { static std::recursive_mutex lock; return lock; }
ComPtr<ID3D11DeviceContext> Context() {
  ComPtr<ID3D11DeviceContext> result;
  Device()->GetImmediateContext(&result);
  return result;
}
ComPtr<ID3D11Texture2D> Texture(int width, int height, DXGI_FORMAT format) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width; desc.Height = height;
  desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format;
  desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> result;
  Check(Device()->CreateTexture2D(&desc, nullptr, &result), "CreateTexture2D");
  return result;
}
D3DFrameBuffer::D3DFrameBuffer(ID3D11Texture2D* texture, UINT subresource,
                             int64_t captured_ns, IUnknown* owner,
                             int visible_width, int visible_height)
    : texture_(texture), owner_(owner), subresource_(subresource), captured_ns_(captured_ns) {
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  width_ = visible_width ? visible_width : desc.Width;
  height_ = visible_height ? visible_height : desc.Height;
  format_ = desc.Format;
}
namespace {
// The video processor and its views, kept between frames. Made anew for every
// layer of every frame they were three processors a frame, and the NVIDIA
// driver's thread was the program's most expensive (06/10/2026). Keyed by the
// sizes and formats; the output views by texture (an encoder layer's texture
// lives as long as the layer). Small, and only touched under ContextMutex.
struct ProcessorEntry {
  UINT in_w, in_h, out_w, out_h;
  DXGI_FORMAT in_f, out_f;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
};
struct OutputViewEntry {
  ComPtr<ID3D11Texture2D> texture;  // held, so the pointer cannot be reused
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessorOutputView> view;
};
std::vector<ProcessorEntry>& Processors() { static std::vector<ProcessorEntry> v; return v; }
std::vector<OutputViewEntry>& OutputViews() { static std::vector<OutputViewEntry> v; return v; }
constexpr size_t kCacheSize = 8;
}  // namespace

VideoProcessorCacheUsage VideoProcessorCacheSnapshot() {
  std::lock_guard lock(ContextMutex());
  VideoProcessorCacheUsage result{Processors().size(),OutputViews().size()};
  result.device_users=DeviceUsers();result.device_loaded=bool(SharedDevice());
  for(auto& entry:OutputViews()) {
    D3D11_TEXTURE2D_DESC d{};entry.texture->GetDesc(&d);
    result.texture_bytes+=uint64_t(d.Width)*d.Height*d.ArraySize*(d.Format==DXGI_FORMAT_NV12?3:8)/2;
  }
  return result;
}
void ReleaseVideoProcessorCache() {
  // The same lock serializes Blit on all encoder/presenter threads. Other
  // active users can rebuild their entries on their next frame.
  std::lock_guard lock(ContextMutex());
  OutputViews().clear();Processors().clear();
}
void ForgetOutputViews(ID3D11Texture2D* texture) {
  std::lock_guard lock(ContextMutex());
  std::erase_if(OutputViews(), [texture](auto const& v) { return v.texture.Get() == texture; });
}
DeviceLease::DeviceLease() {std::lock_guard lock(ContextMutex());++DeviceUsers();}
void EnableIdleDeviceRelease() {std::lock_guard lock(ContextMutex());IdleReleaseEnabled()=true;}
DeviceLease::~DeviceLease() {
  std::lock_guard lock(ContextMutex());
  if(--DeviceUsers()==0) {ReleaseVideoProcessorCache();if(IdleReleaseEnabled())SharedDevice().reset();}
}

void Blit(const D3DFrameBuffer& input, ID3D11Texture2D* output,
          const RECT& source, const RECT& destination, int rotation, bool keep_view) {
  std::lock_guard lock(ContextMutex());
  ComPtr<ID3D11VideoDevice> device;
  Check(Device()->QueryInterface(IID_PPV_ARGS(&device)), "ID3D11VideoDevice");
  ComPtr<ID3D11VideoContext> context;
  Check(Context().As(&context), "ID3D11VideoContext");
  D3D11_TEXTURE2D_DESC out{}; output->GetDesc(&out);
  auto& processors = Processors();
  ProcessorEntry* entry = nullptr;
  for (auto& e : processors)
    if (e.in_w == UINT(input.width()) && e.in_h == UINT(input.height()) && e.out_w == out.Width &&
        e.out_h == out.Height && e.in_f == input.format() && e.out_f == out.Format) { entry = &e; break; }
  if (!entry) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = input.width(); content.InputHeight = input.height();
    content.OutputWidth = out.Width; content.OutputHeight = out.Height;
    content.InputFrameRate = {60, 1}; content.OutputFrameRate = {60, 1};
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ProcessorEntry made{UINT(input.width()), UINT(input.height()), out.Width, out.Height, input.format(), out.Format};
    Check(device->CreateVideoProcessorEnumerator(&content, &made.enumerator), "VideoProcessorEnumerator");
    UINT support = 0;
    Check(made.enumerator->CheckVideoProcessorFormat(input.format(), &support), "Input format");
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) throw std::runtime_error("Video processor input unsupported");
    Check(made.enumerator->CheckVideoProcessorFormat(out.Format, &support), "Output format");
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) throw std::runtime_error("Video processor output unsupported");
    Check(device->CreateVideoProcessor(made.enumerator.Get(), 0, &made.processor), "VideoProcessor");
    if (processors.size() >= kCacheSize) processors.erase(processors.begin());
    processors.push_back(std::move(made));
    entry = &processors.back();
  }
  ID3D11VideoProcessorEnumerator* enumerator = entry->enumerator.Get();
  ID3D11VideoProcessor* processor_ptr = entry->processor.Get();
  D3D11_TEXTURE2D_DESC in{}; input.texture()->GetDesc(&in);
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  iv.Texture2D.MipSlice = input.subresource() % in.MipLevels;
  iv.Texture2D.ArraySlice = input.subresource() / in.MipLevels;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  Check(device->CreateVideoProcessorInputView(input.texture(), enumerator, &iv, &input_view), "ProcessorInputView");
  auto& views = OutputViews();
  ID3D11VideoProcessorOutputView* output_view = nullptr;
  ComPtr<ID3D11VideoProcessorOutputView> once;
  if (keep_view) for (auto& v : views)
    if (v.texture.Get() == output && v.enumerator.Get() == enumerator) { output_view = v.view.Get(); break; }
  if (!output_view && !keep_view) {
    // A texture made for this one frame. Caching its view held the last eight
    // such frames alive in video memory (66 MB of 1080p while sharing) and
    // pushed out the views that do repeat, the encoder's and the swap chains'.
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Check(device->CreateVideoProcessorOutputView(output, enumerator, &ov, &once), "ProcessorOutputView");
    output_view = once.Get();
  }
  if (!output_view) {
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    OutputViewEntry made{output, entry->enumerator};
    Check(device->CreateVideoProcessorOutputView(output, enumerator, &ov, &made.view), "ProcessorOutputView");
    if (views.size() >= kCacheSize) views.erase(views.begin());
    views.push_back(std::move(made));
    output_view = views.back().view.Get();
  }
  context->VideoProcessorSetStreamFrameFormat(processor_ptr, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  context->VideoProcessorSetStreamAutoProcessingMode(processor_ptr, 0, FALSE);
  context->VideoProcessorSetStreamSourceRect(processor_ptr, 0, TRUE, &source);
  context->VideoProcessorSetStreamDestRect(processor_ptr, 0, TRUE, &destination);
  RECT target{0, 0, static_cast<LONG>(out.Width), static_cast<LONG>(out.Height)};
  context->VideoProcessorSetOutputTargetRect(processor_ptr, TRUE, &target);
  D3D11_VIDEO_COLOR black{};
  black.RGBA.A = 1.0f;
  context->VideoProcessorSetOutputBackgroundColor(processor_ptr, FALSE, &black);
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{}, yuv{};
  rgb.RGB_Range = 0; yuv.YCbCr_Matrix = 1; yuv.Nominal_Range = 1;
  context->VideoProcessorSetStreamColorSpace(processor_ptr, 0, input.format() == DXGI_FORMAT_NV12 ? &yuv : &rgb);
  context->VideoProcessorSetOutputColorSpace(processor_ptr, out.Format == DXGI_FORMAT_NV12 ? &yuv : &rgb);
  ComPtr<ID3D11VideoContext1> rotation_context;
  if(SUCCEEDED(context.As(&rotation_context)))rotation_context->VideoProcessorSetStreamRotation(processor_ptr,0,rotation!=0,static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(rotation));
  else if(rotation)throw std::runtime_error("Desktop rotation requires D3D11.1 video context");
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE; stream.pInputSurface = input_view.Get();
  Check(context->VideoProcessorBlt(processor_ptr, output_view, 0, 1, &stream), "VideoProcessorBlt");
}
webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3DFrameBuffer::CropAndScale(
    int x, int y, int width, int height, int out_width, int out_height) {
  // Capture detaches pool textures before publishing; decoder frames retain
  // their sample owner. Downstream consumers only read published textures.
  if (x == 0 && y == 0 && width == width_ && height == height_ &&
      out_width == width_ && out_height == height_) return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(this);
  static perf::Stage& stageScale = perf::Get("scale");
  auto timer = stageScale.Begin();
  auto result = Texture(out_width, out_height, format_);
  Blit(*this, result.Get(), RECT{x,y,x+width,y+height}, RECT{0,0,out_width,out_height}, 0, false);
  timer.Done();
  return webrtc::make_ref_counted<D3DFrameBuffer>(result.Get(), 0, captured_ns_);
}
webrtc::scoped_refptr<D3DFrameBuffer> Scale(const D3DFrameBuffer& input, int width, int height, DXGI_FORMAT format) {
  static perf::Stage& stageScale = perf::Get("scale");
  auto timer = stageScale.Begin();
  auto texture = Texture(width, height, format);
  Blit(input, texture.Get(), RECT{0,0,input.width(),input.height()}, RECT{0,0,width,height}, 0, false);
  timer.Done();
  return webrtc::make_ref_counted<D3DFrameBuffer>(texture.Get(), 0, input.captured_ns());
}
webrtc::scoped_refptr<webrtc::I420BufferInterface> D3DFrameBuffer::ToI420() {
  static perf::Stage& cpuCopies=perf::Get("gpu.cpu_copies");cpuCopies.Record(0);
  std::lock_guard lock(ContextMutex());
  D3D11_TEXTURE2D_DESC desc{}; texture_->GetDesc(&desc);
  desc.ArraySize = 1; desc.MipLevels = 1; desc.BindFlags = 0;
  desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(Device()->CreateTexture2D(&desc, nullptr, &staging))) return nullptr;
  auto context = Context();
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture_.Get(), subresource_, nullptr);
  auto output = webrtc::I420Buffer::Create(width_, height_);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return nullptr;
  int status = -1;
  auto bytes = static_cast<const uint8_t*>(mapped.pData);
  if (format_ == DXGI_FORMAT_NV12)
    status = libyuv::NV12ToI420(bytes, mapped.RowPitch, bytes + mapped.RowPitch * desc.Height, mapped.RowPitch,
        output->MutableDataY(), output->StrideY(), output->MutableDataU(), output->StrideU(),
        output->MutableDataV(), output->StrideV(), width_, height_);
  else if (format_ == DXGI_FORMAT_B8G8R8A8_UNORM)
    status = libyuv::ARGBToI420(bytes, mapped.RowPitch, output->MutableDataY(), output->StrideY(),
        output->MutableDataU(), output->StrideU(), output->MutableDataV(), output->StrideV(), width_, height_);
  context->Unmap(staging.Get(), 0);
  if (status != 0) return nullptr;
  return output;
}
}  // namespace gpu
