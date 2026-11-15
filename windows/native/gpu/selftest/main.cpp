#include "../internal.h"
#include "../../../app/testes/resource-meter.h"
#include "../cadence.h"
#include "api/environment/environment_factory.h"
#include "api/video/render_resolution.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_decoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include <d3d11_1.h>
#include <timeapi.h>
#include <dxgi1_2.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <algorithm>
#include <cstdlib>

namespace gpu { bool DrainDecoder(webrtc::VideoDecoder* decoder); }
namespace {
using gpu::ComPtr;
void Pump() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
  return DefWindowProcW(window, message, w, l);
}
HWND Window(const wchar_t* title, int width, int height, bool popup,bool visible=true) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"HarmonyGpuSelftest"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls)) throw std::runtime_error("RegisterClass failed");
    registered = true;
  }
  DWORD style = popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
  RECT rect{0,0,width,height}; AdjustWindowRect(&rect, style, FALSE);
  auto monitorX=std::getenv("HARMONY_TEST_MONITOR_X");
  HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"HarmonyGpuSelftest", title, style,
      monitorX?std::atoi(monitorX):0, 0, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window) throw std::runtime_error("CreateWindow failed");
  if(visible)ShowWindow(window,SW_SHOWNOACTIVATE);
  return window;
}
class Pattern {
 public:
  explicit Pattern(HWND window) {
    ComPtr<IDXGIDevice> device;
    gpu::Check(gpu::Device()->QueryInterface(IID_PPV_ARGS(&device)), "Pattern device");
    ComPtr<IDXGIAdapter> adapter; gpu::Check(device->GetAdapter(&adapter), "Pattern adapter");
    ComPtr<IDXGIFactory2> factory; gpu::Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Pattern factory");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 1920; desc.Height = 1080;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    gpu::Check(factory->CreateSwapChainForHwnd(gpu::Device(), window, &desc, nullptr, nullptr, &swap_), "Pattern swap chain");
    gpu::Check(gpu::Context().As(&context_), "Pattern D3D11.1 context");
  }
  webrtc::scoped_refptr<gpu::D3DFrameBuffer> Draw(int frame, bool retain = false) {
    std::lock_guard lock(gpu::ContextMutex());
    ComPtr<ID3D11Texture2D> texture; gpu::Check(swap_->GetBuffer(0, IID_PPV_ARGS(&texture)), "Pattern buffer");
    ComPtr<ID3D11RenderTargetView> view;
    gpu::Check(gpu::Device()->CreateRenderTargetView(texture.Get(), nullptr, &view), "Pattern render target");
    float background[]{0.05f + (frame % 120) / 150.0f, 0.12f, 0.3f, 1.0f};
    context_->ClearRenderTargetView(view.Get(), background);
    int x = frame * 13 % 1600, y = frame * 7 % 840;
    D3D11_RECT rect{x,y,x+320,y+240}; float color[]{1.0f, 0.8f, (frame % 60) / 60.0f, 1.0f};
    context_->ClearView(view.Get(), color, &rect, 1);
    webrtc::scoped_refptr<gpu::D3DFrameBuffer> output;
    if (retain) {
      auto detached = gpu::Texture(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
      gpu::Context()->CopyResource(detached.Get(), texture.Get());
      output = webrtc::make_ref_counted<gpu::D3DFrameBuffer>(detached.Get(), 0, perf::Stage::Now());
    }
    gpu::Check(swap_->Present(0, 0), "Pattern Present");
    return output;
  }
 private:
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11DeviceContext1> context_;
};
struct StoredFrame { uint64_t offset; size_t size; uint32_t rtp; int64_t capture_ms; bool key; };
class Encoded : public webrtc::EncodedImageCallback {
 public:
  explicit Encoded(const std::filesystem::path& directory) {
    const char* names[]{"baixa", "media", "alta"};
    for (int i = 0; i < 3; ++i) {
      files[i].open(directory / (std::string(names[i]) + ".h264"), std::ios::binary | std::ios::trunc);
      if (!files[i]) throw std::runtime_error("Cannot open H264 output");
    }
  }
  Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
    int index = image.SimulcastIndex().value_or(-1);
    int layer = image._encodedWidth>=1200?2:image._encodedWidth>=600?1:0;
    if(index<0||index>2){failed=true;return Result(Result::ERROR_SEND_FAILED);}
    if (layer < 0 || layer > 2 || image.size() == 0) { failed = true; return Result(Result::ERROR_SEND_FAILED); }
    if (layer == 2) high.push_back({static_cast<uint64_t>(files[layer].tellp()), image.size(),
        image.RtpTimestamp(), image.capture_time_ms_, image._frameType == webrtc::VideoFrameType::kVideoFrameKey});
    files[layer].write(reinterpret_cast<const char*>(image.data()), image.size());
    if (!files[layer]) { failed = true; return Result(Result::ERROR_SEND_FAILED); }
    ++counts[layer];
    return Result(Result::OK);
  }
  void OnFrameDropped(uint32_t, int, bool) override {}
  void Close() { for (auto& file : files) file.close(); }
  std::array<int,3> counts{};
  std::vector<StoredFrame> high;
  bool failed = false;
 private:
  std::array<std::ofstream,3> files;
};
class Input : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit Input(webrtc::VideoEncoder* encoder) : encoder_(encoder), worker_([this] { Run(); }) {}
  ~Input() override { Stop(); }
  void OnFrame(const webrtc::VideoFrame& frame) override {
    int64_t zero = 0; first_ns.compare_exchange_strong(zero, perf::Stage::Now());
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    if (queue_.size() >= 8) { perf::Get("selftest.queue").Drop(); failed = true; return; }
    queue_.push_back(frame); changed_.notify_one();
  }
  void Stop() {
    { std::lock_guard lock(mutex_); stopped_ = true; changed_.notify_one(); }
    if (worker_.joinable()) worker_.join();
  }
  std::atomic<int64_t> first_ns{0};
  std::atomic<bool> failed{false};
 private:
  void Run() {
    gpu::NameThreadOnce(L"harmony-encode");
    gpu::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Encode COM");
    bool first = true;
    for (;;) {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
      if (queue_.empty() && stopped_) break;
      auto frame = queue_.front(); queue_.pop_front(); lock.unlock();
      std::vector<webrtc::VideoFrameType> types(3, first ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta);
      first = false;
      if (encoder_->Encode(frame, &types) != 0) failed = true;
    }
    CoUninitialize();
  }
  webrtc::VideoEncoder* encoder_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<webrtc::VideoFrame> queue_;
  bool stopped_ = false;
  std::thread worker_;
};
class DecodeSink : public webrtc::DecodedImageCallback {
 public:
  explicit DecodeSink(gpu::Presenter* presenter) : presenter_(presenter) {}
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    ++count;
    if (presenter_) {
      while (perf::Stage::Now() < next_ns_) { Pump(); Sleep(1); }
      presenter_->OnFrame(frame); Pump(); next_ns_ = perf::Stage::Now() + 1000000000LL / 60;
    }
    return 0;
  }
  int count = 0;
 private:
  gpu::Presenter* presenter_;
  int64_t next_ns_ = 0;
};
struct Total { uint64_t out = 0, drop = 0; double busy = 0, maximum = 0; };
class Measurements {
 public:
  void Tick() {
    auto snapshot = perf::TakeSnapshot();
    std::cout << perf::Format(snapshot) << std::flush;
    for (auto& row : snapshot.stages) {
      auto& total = totals[row.name]; total.out += row.out; total.drop += row.dropped;
      total.busy += row.avg_ms * row.out; total.maximum = std::max(total.maximum, row.max_ms);
    }
  }
  uint64_t Count(const char* name) { return totals[name].out; }
  void Print() {
    for (auto& [name, total] : totals)
      std::cout << name << " frames=" << total.out << " drops=" << total.drop
                << " avg_ms=" << (total.out ? total.busy / total.out : 0) << " max_ms=" << total.maximum << "\n";
  }
 private:
  std::map<std::string, Total> totals;
};
}
static int RotationProbe() {
  std::vector<uint8_t> pixels(8*4*4);
  for(int y=0;y<4;++y)for(int x=0;x<8;++x){int label=1+(x>=4)+2*(y>=2);for(int c=0;c<4;++c)pixels[(y*8+x)*4+c]=c==3?255:label*40;}
  auto texture=gpu::Texture(8,4,DXGI_FORMAT_B8G8R8A8_UNORM);gpu::Context()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),32,0);
  auto input=webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get());
  const int expected[4][4]{{1,2,3,4},{3,1,4,2},{4,3,2,1},{2,4,1,3}};
  for(int rotation=0;rotation<4;++rotation){int width=rotation%2?4:8,height=rotation%2?8:4;auto output=gpu::Texture(width,height,DXGI_FORMAT_B8G8R8A8_UNORM);
    gpu::Blit(*input,output.Get(),RECT{0,0,8,4},RECT{0,0,width,height},rotation);
    D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;gpu::Check(gpu::Device()->CreateTexture2D(&desc,nullptr,&staging),"Rotation staging");gpu::Context()->CopyResource(staging.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};gpu::Check(gpu::Context()->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Rotation pixels");bool ok=true;
    for(int quadrant=0;quadrant<4;++quadrant){int x=(quadrant%2?3:1)*width/4,y=(quadrant/2?3:1)*height/4;int value=static_cast<uint8_t*>(mapped.pData)[y*mapped.RowPitch+x*4];ok=ok&&std::abs(value-expected[rotation][quadrant]*40)<=5;}
    gpu::Context()->Unmap(staging.Get(),0);if(!ok){std::cerr<<"rotation pixel orientation failed degrees="<<rotation*90<<"\n";return 1;}
  }
  std::cout<<"rotation pixels 0/90/180/270: PASS\n";return 0;
}
#include "keyframes.h"
#include "encoder-measure.h"
// Headless lifetime measurement, reusing the production device/scale/cache owners.
static int ResourceLifecycle(bool freshWorker) {
  gpu::EnableIdleDeviceRelease();resource_meter::Mark("gpu-baseline");
  for(int i=1;i<=20;++i) {
    auto cycle=[&] {
      gpu::DeviceLease lease;gpu::Device();
      if(i==1)resource_meter::Mark("gpu-device-created");
      auto texture=gpu::Texture(1920,1080,DXGI_FORMAT_B8G8R8A8_UNORM);
      auto frame=webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get());
      auto high=gpu::Scale(*frame,1920,1080,DXGI_FORMAT_NV12);
      auto medium=gpu::Scale(*frame,960,540,DXGI_FORMAT_NV12);
      auto low=gpu::Scale(*frame,480,270,DXGI_FORMAT_NV12);
      auto cache=gpu::VideoProcessorCacheSnapshot();
      std::cout<<"gpu cycle="<<i<<" processors="<<cache.processors<<" views="<<cache.output_views<<" retained_texture_bytes="<<cache.texture_bytes<<'\n';
      Sleep(i==1?3000:100);
      auto label="gpu-cycle-"+std::to_string(i)+"-during";resource_meter::Mark(label.c_str());
    };
    if(freshWorker){std::thread worker(cycle);worker.join();}else cycle();
    Sleep(i==1?3000:100);auto label="gpu-cycle-"+std::to_string(i)+"-released";resource_meter::Mark(label.c_str());
    auto cache=gpu::VideoProcessorCacheSnapshot();
    if(cache.device_loaded || cache.device_users || cache.texture_bytes || cache.processors || cache.output_views)throw std::runtime_error("GPU owners survived release");
  }
  Sleep(3000);resource_meter::Mark("gpu-final-settled");std::cout<<"PASS: GPU lifecycle owners released\n";return 0;
}
// Investigation fixture only: local preview consumes frames, no encoder or
// remote viewer. The source's production constraints change; capture stays open.
static int PreviewCaptureProbe() {
  HWND window=Window(L"Harmony capture rate probe",1920,1080,false);
  Pattern pattern(window);
  struct Sink:webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<uint64_t> frames{0};
    void OnFrame(webrtc::VideoFrame const&) override {++frames;}
  } sink;
  auto source=gpu::CreateWindowSource(window,60);
  webrtc::VideoSinkWants wants;wants.max_framerate_fps=60;source->AddOrUpdateSink(&sink,wants);
  int drawn=0;
  for(int rate:{30,5}) {
    webrtc::VideoTrackSourceConstraints constraints;constraints.max_fps=rate;source->ProcessConstraints(constraints);
    auto settle=perf::Stage::Now(),drawAt=settle;
    while(perf::Stage::Now()-settle<1500000000LL) {Pump();auto now=perf::Stage::Now();if(now>=drawAt){pattern.Draw(drawn++,false);drawAt+=1000000000LL/60;}Sleep(1);}
    auto began=perf::Stage::Now(),next=began;auto previous=sink.frames.load();
    perf::TakeSnapshot();
    while(perf::Stage::Now()-began<6000000000LL) {
      Pump();auto now=perf::Stage::Now();
      if(now>=next) {pattern.Draw(drawn++,false);next+=1000000000LL/60;}
      Sleep(1);
    }
    auto snapshot=perf::TakeSnapshot();double cpu=0;uint64_t copies=0;double busy=0;
    for(auto const& row:snapshot.threads)if(row.name=="harmony-capture")cpu+=row.cpu_percent;
    for(auto const& row:snapshot.stages)if(row.name=="capture"){copies=row.out;busy=row.avg_ms*row.out;}
    std::cout<<"preview_probe rate="<<rate<<" frames="<<sink.frames-previous<<" seconds="<<snapshot.seconds<<" capture_cpu_core_pct="<<cpu<<" capture_stage_frames="<<copies<<" capture_stage_busy_ms="<<busy<<"\n";
    std::cout<<perf::Format(snapshot);
    resource_meter::Mark(rate==30?"preview-30fps":"preview-5fps");
  }
  source->RemoveSink(&sink);source=nullptr;DestroyWindow(window);
  return sink.frames?0:1;
}
static int RunSelftest(int argc, char** argv) {
  // The pattern window must really draw at 60 Hz: with the default 15.6 ms
  // timer, Sleep(1) in its loop would be the thing measured, not the capture.
  timeBeginPeriod(1);
  try {
    bool resource_lifecycle=false,fresh_worker=false;bool fallback = false, present = false, codec_probe = false; int fail_after = 0, arrival_hz=60;bool reverse=false,rotation_probe=false,drop_lower=false,keyframes=false,retry=false;
    for (int i = 1; i < argc; ++i) {
      std::string argument = argv[i];
      if (argument == "--preview-capture-probe") {gpu::Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Capture probe COM");auto result=PreviewCaptureProbe();CoUninitialize();timeEndPeriod(1);return result;}
      else if (argument == "--resource-lifecycle") resource_lifecycle=true;
      else if (argument == "--resource-worker-lifecycle") resource_lifecycle=fresh_worker=true;
      else if (argument == "--keyframes") keyframes=true;
      else if (argument == "--keyframes-retry") keyframes=retry=true;
      else if (argument == "--fallback") fallback = true;
      else if (argument == "--present") present = true;
      else if (argument == "--arrival-hz" && i+1<argc) arrival_hz=std::stoi(argv[++i]);
      else if (argument == "--reverse-layers") reverse=true;
      else if (argument == "--rotation-probe") rotation_probe=true;
      else if (argument == "--drop-lower-layers") drop_lower=true;
      else if (argument == "--codec-probe") codec_probe = true;
      else if (argument == "--fail-after" && i+1 < argc) fail_after = std::stoi(argv[++i]);
      else throw std::runtime_error("Usage: gpu_selftest [--fallback] [--fail-after N] [--present] [--codec-probe] [--keyframes] [--keyframes-retry]");
    }
    gpu::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Selftest COM");
    if(resource_lifecycle){auto code=ResourceLifecycle(fresh_worker);CoUninitialize();timeEndPeriod(1);return code;}
    if(keyframes) {int code=Keyframes(retry);CoUninitialize();timeEndPeriod(1);return code;}
    if(rotation_probe){int code=RotationProbe();CoUninitialize();timeEndPeriod(1);return code;}
    gpu::NameThreadOnce(L"harmony-selftest");
    // Establish the first interval before stages run. The first snapshot has
    // no elapsed interval and must not be printed as a CPU measurement.
    perf::TakeSnapshot();
    if (codec_probe) std::cout << "CODEC PROBE: generated D3D textures, NO WGC; not W1 capture acceptance\n";
    gpu::ConfigureSelftest(fallback, fail_after);
    wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
    auto directory = std::filesystem::path(executable).parent_path() / "selftest";
    std::filesystem::create_directories(directory);
    auto env = webrtc::CreateEnvironment();
    resource_meter::Mark("gpu-before-encoder-factory");
    auto factory = gpu::CreateEncoderFactory();
    resource_meter::Mark("gpu-after-encoder-factory");
    auto format = factory->GetSupportedFormats().front();
    auto encoder = factory->Create(env, format);
    if (!encoder) throw std::runtime_error("Cannot create encoder");
    webrtc::VideoCodec codec{};
    codec.codecType = webrtc::kVideoCodecH264; codec.width = 1920; codec.height = 1080;
    codec.maxFramerate = 60; codec.startBitrate = 19800; codec.maxBitrate = 19800;
    codec.numberOfSimulcastStreams = 3; codec.mode = webrtc::VideoCodecMode::kScreensharing;
    const int widths[]{480,960,1920}, heights[]{270,540,1080}, fps[]{15,30,60}, kbps[]{800,4000,15000};
    for (int i = 0; i < 3; ++i) {
      auto& layer = codec.simulcastStream[reverse?2-i:i]; layer.width = widths[i]; layer.height = heights[i];
      layer.maxFramerate = fps[i]; layer.targetBitrate = kbps[i]; layer.maxBitrate = kbps[i];
      layer.minBitrate = 100; layer.active = true; layer.qpMax = 51;
    }
    if (encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 4, 1200)) != 0)
      throw std::runtime_error("InitEncode failed (see exact driver/MF error above)");
    resource_meter::Mark("gpu-encoder-initialized");
    Encoded encoded(directory);
    encoder->RegisterEncodeCompleteCallback(&encoded);
    webrtc::VideoBitrateAllocation allocation;
    for (int i = 0; i < 3; ++i) allocation.SetBitrate(reverse?2-i:i, 0, kbps[i] * 1000);
    encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, 60));
    HWND capture_window = Window(L"Harmony W1 GPU pattern", 1920, 1080, std::getenv("SELFTEST_POPUP") != nullptr,!codec_probe);
    Pattern pattern(capture_window);
    Input input(encoder.get());
    auto source = codec_probe ? nullptr : gpu::CreateWindowSource(capture_window, 60);
    webrtc::VideoSinkWants wants;
    wants.max_framerate_fps = 60;
    if (source) source->AddOrUpdateSink(&input, wants);
    resource_meter::Mark("gpu-pattern-created");
    Measurements measurements;
    int64_t began = perf::Stage::Now(), next_draw = began, next_tick = began + 1000000000LL;
    int drawn = 0;int64_t next_capture=0;bool rates_changed=false;
    while (!input.first_ns || perf::Stage::Now() - input.first_ns < 10000000000LL) {
      int64_t now = perf::Stage::Now(); Pump();
      if(drop_lower&&!rates_changed&&input.first_ns&&now-input.first_ns>=5000000000LL){webrtc::VideoBitrateAllocation highOnly;highOnly.SetBitrate(reverse?0:2,0,kbps[2]*1000);encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(highOnly,60));rates_changed=true;}
      if (now - began > 15000000000LL) throw std::runtime_error("WGC did not finish capture within 15 seconds");
      if (now >= next_draw) {
        auto generated = pattern.Draw(drawn++, codec_probe);
        if (codec_probe && gpu::CaptureDue(now/1000,60,next_capture)) input.OnFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(generated)
            .set_timestamp_us(now / 1000).set_rtp_timestamp(static_cast<uint32_t>(now / 1000000 * 90)).build());
        next_draw += 1000000000LL / arrival_hz;
      }
      if (now >= next_tick) { measurements.Tick(); next_tick += 1000000000LL; }
      Sleep(1);
    }
    if (source) source->RemoveSink(&input);
    source = nullptr; input.Stop(); encoded.Close(); encoder->Release();
    ShowWindow(capture_window, SW_HIDE);
    std::cout << "pattern drawn=" << drawn << " in " << (perf::Stage::Now() - began) / 1000000 << " ms\n";
    std::cout << "ActiveEncoderName=" << gpu::ActiveEncoderName() << "\n";
    std::cout << "frames alta=" << encoded.counts[2] << " media=" << encoded.counts[1] << " baixa=" << encoded.counts[0] << "\n";
    if(drop_lower)std::cout<<"lower layers stopped after 5 seconds; high remains 60 fps\n";
    resource_meter::Mark("gpu-encoding-finished");
    auto decoder_factory = gpu::CreateDecoderFactory();
    auto decoder = decoder_factory->Create(env, format);
    HWND output_window = present ? Window(L"Harmony W1 decoded high layer", 1280, 720, false) : nullptr;
    auto presenter = present ? gpu::Presenter::Create(output_window) : nullptr;
    if (present && !presenter) throw std::runtime_error("Presenter creation failed");
    DecodeSink decoded(presenter.get());
    decoder->RegisterDecodeCompleteCallback(&decoded);
    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(webrtc::kVideoCodecH264);
    settings.set_max_render_resolution(webrtc::RenderResolution(1920,1080));
    if (!decoder->Configure(settings)) throw std::runtime_error("DXVA decoder Configure failed");
    resource_meter::Mark("gpu-decoder-configured");
    std::ifstream high(directory / "alta.h264", std::ios::binary);
    bool decode_failed = false;
    for (auto& record : encoded.high) {
      high.seekg(static_cast<std::streamoff>(record.offset));
      std::vector<uint8_t> bytes(record.size); high.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
      if (!high) throw std::runtime_error("Cannot read alta.h264");
      webrtc::EncodedImage image;
      image.SetEncodedData(webrtc::EncodedImageBuffer::Create(bytes.data(), bytes.size()));
      image.SetRtpTimestamp(record.rtp); image.capture_time_ms_ = record.capture_ms;
      image._frameType = record.key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
      if (decoder->Decode(image, false, 0) != 0) { decode_failed = true; break; }
      if (perf::Stage::Now() >= next_tick) { measurements.Tick(); next_tick = perf::Stage::Now() + 1000000000LL; }
    }
    if (!gpu::DrainDecoder(decoder.get())) decode_failed = true;
    resource_meter::Mark("gpu-decoding-finished");
    decoder->Release();
    measurements.Tick(); measurements.Print();
    auto copies = measurements.Count("gpu.cpu_copies"), falls = measurements.Count("encoder.fallback");
    std::cout << "decoded=" << decoded.count << " gpu.cpu_copies=" << copies << " encoder.fallback=" << falls << "\n";
    bool nvenc = gpu::ActiveEncoderName().find("NVENC") == 0;
    bool failed = input.failed || encoded.failed || decode_failed || decoded.count < encoded.counts[2] * 0.9 || decoded.count == 0 ||
        encoded.counts[0] < (drop_lower?67:135) || encoded.counts[1] < (drop_lower?135:270) || encoded.counts[2] < 570 || (nvenc && copies > 0) ||
        (fail_after > 0 && (falls != 1 || nvenc)) || (fallback && (copies == 0 || nvenc));
    std::cout << (failed ? "FAIL\n" : "PASS\n");
    if (output_window) { presenter.reset(); DestroyWindow(output_window); }
    DestroyWindow(capture_window);
    CoUninitialize();
    return failed ? 1 : 0;
  } catch (const std::exception& error) { std::cerr << "SELFTEST FAILED: " << error.what() << "\n"; return 2; }
}


int main(int argc,char** argv) {
  if(argc>1 && std::string(argv[1])=="--vbv-probe") {
    try {return argc>4?encoder_measure::VbvProbe(std::stoi(argv[2]),std::stoi(argv[3])!=0,std::stoi(argv[4])):encoder_measure::VbvProbe();}
    catch(const std::exception& error){std::cerr<<"VBV PROBE FAILED: "<<error.what()<<"\n";return 2;}
  }
  if(argc>1 && std::string(argv[1])=="--measure-encoder") {
    try {return encoder_measure::Run(argc,argv);}
    catch(const std::exception& error){std::cerr<<"MEASURE FAILED: "<<error.what()<<"\n";return 2;}
  }
  if(argc!=2 || std::string(argv[1])!="--resource-media-cycles")return RunSelftest(argc,argv);
  // Match the app's idle release policy, including real NVENC and DXVA owners.
  gpu::EnableIdleDeviceRelease();resource_meter::Mark("media-baseline");
  char program[]="gpu_selftest",probe[]="--codec-probe";char* arguments[]{program,probe};
  int cadenceFailures=0;
  for(int i=1;i<=5;++i){
    auto code=RunSelftest(2,arguments);timeEndPeriod(1);if(code)++cadenceFailures;
    Sleep(3000);auto label="media-cycle-"+std::to_string(i)+"-released";resource_meter::Mark(label.c_str());
    auto cache=gpu::VideoProcessorCacheSnapshot();
    std::cout<<"media cycle="<<i<<" device_loaded="<<cache.device_loaded<<" device_users="<<cache.device_users<<" retained_texture_bytes="<<cache.texture_bytes<<'\n';
    if(cache.device_loaded || cache.device_users || cache.texture_bytes || cache.output_views || cache.processors)return 1;
  }
  Sleep(3000);resource_meter::Mark("media-final-settled");std::cout<<"PASS: media owners released across five cycles; cadence_failures="<<cadenceFailures<<"\n";return cadenceFailures?1:0;
}
