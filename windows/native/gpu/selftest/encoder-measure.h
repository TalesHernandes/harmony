// Headless, video-only benchmark. Never persists source pixels or encoded media.
#pragma once
#include "../nvenc.h"
#include "../mf.h"
#include "libyuv.h"
#include "api/video/i420_buffer.h"
#include <mfreadwrite.h>
#include <emmintrin.h>
#include <cmath>
#include <iomanip>
#include <numeric>
#pragma comment(lib, "mfreadwrite.lib")
namespace encoder_measure {
using gpu::ComPtr;
struct Source {
  ComPtr<IMFSourceReader> reader;
  int width=0,height=0,storage_height=0,out_width=0,out_height=0; UINT fps_n=0,fps_d=0;
  LONGLONG duration=0, time=0; long stride=0; int samples=0;
  ComPtr<ID3D11Texture2D> texture, alternate; bool flip=false;
  explicit Source(const std::filesystem::path& path) {
    gpu::StartMF();
    ComPtr<IMFAttributes> attrs; gpu::Check(MFCreateAttributes(&attrs,2),"Reader attributes");
    gpu::Check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE),"Video processing");
    gpu::Check(MFCreateSourceReaderFromURL(path.c_str(),attrs.Get(),&reader),"Open video");
    gpu::Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Disable ALL streams");
    gpu::Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"Select video only");
    ComPtr<IMFMediaType> native;gpu::Check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native),"Native video type");
    UINT w=0,h=0;gpu::Check(MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&w,&h),"Source dimensions");width=w;height=h;storage_height=h;
    gpu::Check(MFGetAttributeRatio(native.Get(),MF_MT_FRAME_RATE,&fps_n,&fps_d),"Source fps");
    PROPVARIANT value{};gpu::Check(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&value),"Duration");duration=value.uhVal.QuadPart;PropVariantClear(&value);
    ComPtr<IMFMediaType> rgb;gpu::Check(MFCreateMediaType(&rgb),"RGB type");
    gpu::Check(rgb->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Video major");gpu::Check(rgb->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32),"RGB32");
    gpu::Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,rgb.Get()),"Reader RGB32");
    gpu::Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&rgb),"Reader current type");
    stride=static_cast<LONG>(MFGetAttributeUINT32(rgb.Get(),MF_MT_DEFAULT_STRIDE,width*4));
    double scale=std::min(1.0,std::min(1920.0/width,1080.0/height));out_width=int(width*scale)/2*2;out_height=int(height*scale)/2*2;
    texture=gpu::Texture(width,height,DXGI_FORMAT_B8G8R8A8_UNORM);
    alternate=gpu::Texture(width,height,DXGI_FORMAT_B8G8R8A8_UNORM);
  }
  webrtc::scoped_refptr<gpu::D3DFrameBuffer> Next() {
    for(;;){DWORD flags=0;ComPtr<IMFSample> sample;
      gpu::Check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&time,&sample),"Read video sample");
      if(flags&MF_SOURCE_READERF_ENDOFSTREAM)return nullptr;
      if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED){
        ComPtr<IMFMediaType> updated;gpu::Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&updated),"Updated RGB type");
        UINT w=0,h=0;GUID subtype{};gpu::Check(MFGetAttributeSize(updated.Get(),MF_MT_FRAME_SIZE,&w,&h),"Updated size");
        gpu::Check(updated->GetGUID(MF_MT_SUBTYPE,&subtype),"Updated subtype");
        if(w!=width||h<height||h>UINT((height+15)/16*16)||subtype!=MFVideoFormat_RGB32){std::ostringstream message;message<<"Source format changed to "<<w<<"x"<<h<<" subtype="<<std::hex<<subtype.Data1;throw std::runtime_error(message.str());}
        storage_height=h;
        stride=static_cast<LONG>(MFGetAttributeUINT32(updated.Get(),MF_MT_DEFAULT_STRIDE,width*4));
      }
      if(!sample)continue;
      ComPtr<IMFMediaBuffer> buffer;gpu::Check(sample->ConvertToContiguousBuffer(&buffer),"Source buffer");
      BYTE* data=nullptr;LONG pitch=stride;ComPtr<IMF2DBuffer> two;
      if(SUCCEEDED(buffer.As(&two)))gpu::Check(two->Lock2D(&data,&pitch),"Source Lock2D");
      else {gpu::Check(buffer->Lock(&data,nullptr,nullptr),"Source Lock");if(pitch<0)data+=(storage_height-1)*(-pitch);}
      std::vector<uint8_t> upright;
      if(pitch<0){upright.resize(size_t(width)*height*4);for(int y=0;y<height;++y)memcpy(upright.data()+size_t(y)*width*4,data+y*pitch,width*4);data=upright.data();pitch=width*4;}
      auto target=flip?alternate:texture;flip=!flip;
      gpu::Context()->UpdateSubresource(target.Get(),0,nullptr,data,pitch,0);
      if(two)two->Unlock2D();else buffer->Unlock();++samples;
      auto frame=webrtc::make_ref_counted<gpu::D3DFrameBuffer>(target.Get());
      if(width!=out_width||height!=out_height)return gpu::Scale(*frame,out_width,out_height);
      // Ping-pong: current and next remain distinct, without per-sample GPU allocation.
      return frame;
    }
  }
};
uint64_t SquareError(const uint8_t* a,int sa,const uint8_t* b,int sb,int w,int h){
  uint64_t total=0;const auto zero=_mm_setzero_si128();
  for(int y=0;y<h;++y){int x=0;for(;x+16<=w;x+=16){
    auto aa=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+x));auto bb=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+x));
    auto lo=_mm_sub_epi16(_mm_unpacklo_epi8(aa,zero),_mm_unpacklo_epi8(bb,zero));auto hi=_mm_sub_epi16(_mm_unpackhi_epi8(aa,zero),_mm_unpackhi_epi8(bb,zero));
    auto sum=_mm_add_epi32(_mm_madd_epi16(lo,lo),_mm_madd_epi16(hi,hi));alignas(16) uint32_t v[4];_mm_store_si128(reinterpret_cast<__m128i*>(v),sum);total+=uint64_t(v[0])+v[1]+v[2]+v[3];
  }for(;x<w;++x){int d=int(a[x])-b[x];total+=d*d;}a+=sa;b+=sb;}return total;
}
// The benchmark reuses staging textures; production ToI420 allocates one per call.
// Mapping before the timer also waits for source upload/scale to finish.
D3D11_MAPPED_SUBRESOURCE ReadPixels(const gpu::D3DFrameBuffer& frame,ComPtr<ID3D11Texture2D>& staging){
  if(!staging){D3D11_TEXTURE2D_DESC desc{};frame.texture()->GetDesc(&desc);desc.ArraySize=1;desc.MipLevels=1;desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    gpu::Check(gpu::Device()->CreateTexture2D(&desc,nullptr,&staging),"Measurement staging");}
  gpu::Context()->CopySubresourceRegion(staging.Get(),0,0,0,0,frame.texture(),frame.subresource(),nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped{};gpu::Check(gpu::Context()->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Measurement map");return mapped;
}
webrtc::scoped_refptr<webrtc::I420BufferInterface> ReadReference(const gpu::D3DFrameBuffer& frame,ComPtr<ID3D11Texture2D>& staging){
  auto mapped=ReadPixels(frame,staging);auto output=webrtc::I420Buffer::Create(frame.width(),frame.height());
  int status=libyuv::ARGBToI420(static_cast<const uint8_t*>(mapped.pData),mapped.RowPitch,output->MutableDataY(),output->StrideY(),output->MutableDataU(),output->StrideU(),output->MutableDataV(),output->StrideV(),frame.width(),frame.height());
  gpu::Context()->Unmap(staging.Get(),0);if(status)throw std::runtime_error("Reference ARGBToI420");return output;
}
struct Reference {int index;webrtc::scoped_refptr<webrtc::I420BufferInterface> yuv;};
class Sink : public webrtc::DecodedImageCallback {
 public:
  std::map<uint32_t,Reference> pending;
  std::map<int,double> psnr;
  int w, h;
  Sink(int width, int height) : w(width), h(height) {}
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    auto found = pending.find(frame.rtp_timestamp());
    if (found == pending.end()) throw std::runtime_error("Unmatched decode timestamp");
    auto buffer = frame.video_frame_buffer();
    if (buffer->type() != webrtc::VideoFrameBuffer::Type::kNative || buffer->width() != w ||
        buffer->height() < h || buffer->height() > (h+15)/16*16)
      throw std::runtime_error("Decoded dimensions");
    auto* decoded = static_cast<gpu::D3DFrameBuffer*>(buffer.get());
    if (decoded->format() != DXGI_FORMAT_NV12) throw std::runtime_error("Decoded format");
    if (!staging_[slot_]) {
      D3D11_TEXTURE2D_DESC desc{}; decoded->texture()->GetDesc(&desc);
      desc.ArraySize = 1; desc.MipLevels = 1; desc.BindFlags = 0; desc.MiscFlags = 0;
      desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      gpu::Check(gpu::Device()->CreateTexture2D(&desc,nullptr,&staging_[slot_]), "Decode staging");
    }
    gpu::Context()->CopySubresourceRegion(staging_[slot_].Get(),0,0,0,0,
        decoded->texture(),decoded->subresource(),nullptr);
    jobs_.push_back({found->second,buffer,slot_});
    pending.erase(found);
    slot_ = (slot_+1)%staging_.size();
    // Let DXVA run several pictures ahead before mapping old copies. Mapping
    // every callback would serialize GPU decode on a CPU readback fence.
    if (jobs_.size() == staging_.size()) Retire();
    return 0;
  }
  void Flush() { while (!jobs_.empty()) Retire(); }
 private:
  struct Job {
    Reference reference;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> owner;
    size_t slot;
  };
  void Retire() {
    auto& job = jobs_.front();
    auto texture = staging_[job.slot].Get();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    gpu::Check(gpu::Context()->Map(texture,0,D3D11_MAP_READ,0,&mapped), "Decode readback");
    auto& ref = job.reference;
    uint64_t sse = SquareError(ref.yuv->DataY(),ref.yuv->StrideY(),
        static_cast<const uint8_t*>(mapped.pData),mapped.RowPitch,w,h);
    gpu::Context()->Unmap(texture,0);
    psnr[ref.index] = sse ? 10*std::log10(65025.0*w*h/double(sse)) : 128.0;
    jobs_.pop_front();
  }
  std::array<ComPtr<ID3D11Texture2D>,8> staging_;
  std::deque<Job> jobs_;
  size_t slot_ = 0;
};
struct Record {
  int index;
  double ms, brightness, motion;
  size_t bytes;
  bool key;
};
gpu::NVEncTestOptions Options(const std::string& name) {
  gpu::NVEncTestOptions options;
  // "hoje" is the baseline the report was measured against: one frame.
  options.vbv_frames = 1;
  if (name == "producao") options.vbv_frames = gpu::kVbvFrames;
  else if (name == "vbv2") options.vbv_frames = 2;
  else if (name == "vbv4") options.vbv_frames = 4;
  else if (name == "p3") options.preset = 3;
  else if (name == "p4") options.preset = 4;
  else if (name == "aq") options.spatial_aq = true;
  else if (name == "low") options.low_latency = true;
  else if (name.rfind("combined-", 0) == 0) {
    options.preset = name.find("p4") != std::string::npos ? 4 : name.find("p3") != std::string::npos ? 3 : 1;
    options.vbv_frames = name.find("vbv4") != std::string::npos ? 4 : name.find("vbv2") != std::string::npos ? 2 : 1;
    options.spatial_aq = name.find("-aq") != std::string::npos;
    options.low_latency = name.find("-low") != std::string::npos;
  } else if (name != "hoje") throw std::runtime_error("Unknown configuration");
  return options;
}
void CheckArithmetic() {
  // Scalar oracle: row padding, a non-vector tail, and maximum pixel error.
  for (int kind = 0; kind < 3; ++kind) {
    std::vector<uint8_t> a(29*7), b(31*7);
    uint64_t expected = 0;
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 23; ++x) {
      a[y*29+x] = kind == 2 ? 255 : uint8_t(x*17+y*29);
      b[y*31+x] = kind == 0 ? a[y*29+x] : kind == 2 ? 0 : uint8_t(x*41+y*13);
      int d = int(a[y*29+x])-b[y*31+x];
      expected += d*d;
    }
    if (SquareError(a.data(),29,b.data(),31,23,7) != expected)
      throw std::runtime_error("PSNR square-error self-check failed");
  }
}
struct Trial {
  std::string name;
  std::unique_ptr<gpu::NVEncoder> encoder;
  std::unique_ptr<webrtc::VideoDecoder> decoder;
  std::unique_ptr<Sink> sink;
  std::vector<Record> records;
  std::vector<webrtc::EncodedImage> ready;
};
// The allowance has to survive a rate change, which is where it would be lost
// without anybody noticing: a still picture leaves the buffer full, so a forced
// full picture takes everything it is allowed, and its size says how much that
// was.
static int VbvProbe(int frames=gpu::kVbvFrames,bool change=true,int block=10) {
  const int w = 1920, h = 1080, fps = 60;
  std::vector<uint8_t> noise(size_t(w)*h*4);
  uint32_t seed = 12345;
  // A gradient with grain, `block` levels either way: enough that a full
  // picture wants more than four frames' worth, and little enough that it
  // still fits in one when one is all it may have (measured: 1.0 frames at
  // an allowance of one, 3.1 at four; coarser patterns sat on the
  // encoder's quality floor and showed no difference).
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    seed = seed*1664525u + 1013904223u;
    int base = 96 + (x*64/w) + (y*32/h), grain = int((seed >> 24) % unsigned(2*block + 1)) - block;
    auto value = uint8_t(std::clamp(base + grain, 0, 255));
    auto* pixel = &noise[(size_t(y)*w + x)*4]; pixel[0] = pixel[1] = pixel[2] = value; pixel[3] = 255;
  }
  auto texture = gpu::Texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
  gpu::Context()->UpdateSubresource(texture.Get(), 0, nullptr, noise.data(), w*4, 0);
  auto frame = webrtc::make_ref_counted<gpu::D3DFrameBuffer>(texture.Get());
  auto& stage = perf::Get("measure.encode");
  gpu::NVEncTestOptions options; options.vbv_frames=frames;
  gpu::NVEncoder encoder(w, h, fps, change?8000000:4000000, true, options);
  const uint32_t rate = 4000000;
  size_t key = 0;
  for (int index = 0; index < 180; ++index) {
    if (index == 60 && change) encoder.SetRate(rate);
    encoder.Submit(*frame, LONGLONG(index)*1000000/fps, index == 150, stage);
    auto packet = encoder.Collect();
    if (index == 150) { if (!packet.key) throw std::runtime_error("forced picture was not a key frame"); key = packet.bytes.size(); }
  }
  const double one = double(rate)/fps/8;
  std::cout << "VBV key_bytes=" << key << " frame_budget=" << one << " frames=" << key/one << "\n";
  // The picture takes all it may: well over what a lost allowance would give,
  // and not past the four frames it has.
  if (key < one*2.0 || key > one*(gpu::kVbvFrames + 0.5)) { std::cerr << "FAIL: key frame allowance after a rate change\n"; return 1; }
  std::cout << "PASS: a full picture keeps its four-frame allowance after a rate change\n";
  return 0;
}
static int Run(int argc, char** argv) {
  if (argc < 5) throw std::runtime_error(
      "Usage: --measure-encoder VIDEO OUTPUT_PREFIX CONFIG[,CONFIG] [Mbit/s] [max_frames]");
  CheckArithmetic();
  uint32_t bitrate = (argc > 5 ? std::stoul(argv[5]) : 15)*1000000;
  if (bitrate != 4000000 && bitrate != 8000000 && bitrate != 15000000)
    throw std::runtime_error("Expected 4, 8 or 15 Mbit/s");
  int limit = argc > 6 ? std::stoi(argv[6]) : INT_MAX;
  if (limit < 6) throw std::runtime_error("Too few frames");
  gpu::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Measure COM");
  Source source(argv[2]);
  int w = source.out_width, h = source.out_height;
  std::cout << "SOURCE " << source.width << 'x' << source.height << " fps=" << source.fps_n
            << '/' << source.fps_d << " duration_s=" << source.duration/1e7
            << " output=" << w << 'x' << h << "\n" << std::flush;
  auto env = webrtc::CreateEnvironment();
  auto factory = gpu::CreateDecoderFactory();
  std::vector<Trial> trials;
  std::istringstream names(argv[4]);
  std::string name;
  while (std::getline(names, name, ',')) {
    if (trials.size() == 7) throw std::runtime_error("At most seven resident measurement sessions");
    Trial trial;
    trial.name = name;
    trial.decoder = factory->Create(env, webrtc::SdpVideoFormat("H264",
        {{"packetization-mode", "1"}, {"profile-level-id", "64002a"}}));
    if (!trial.decoder) throw std::runtime_error("Project decoder unavailable");
    trial.sink = std::make_unique<Sink>(w,h);
    trial.decoder->RegisterDecodeCompleteCallback(trial.sink.get());
    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(webrtc::kVideoCodecH264);
    settings.set_max_render_resolution(webrtc::RenderResolution(w,h));
    if (!trial.decoder->Configure(settings)) throw std::runtime_error("Project decoder Configure failed");
    trial.encoder = std::make_unique<gpu::NVEncoder>(w,h,60,bitrate,true,Options(name));
    trials.push_back(std::move(trial));
  }
  if (trials.empty()) throw std::runtime_error("No configurations");
  if (trials.size() > 1 && std::string(argv[3]).find("{config}") == std::string::npos)
    throw std::runtime_error("Batch output prefix must contain {config}");
  auto& stage = perf::Get("measure.encode");
  auto current = source.Next();
  if (!current) throw std::runtime_error("No video frames");
  LONGLONG origin = source.time;
  ComPtr<ID3D11Texture2D> reference_staging;
  auto next = source.Next();
  auto reference = ReadReference(*current, reference_staging);
  std::vector<uint8_t> previous;
  int repeated = 0;
  for (int index = 0; index < limit && LONGLONG(index)*10000000/60 < source.duration; ++index) {
    LONGLONG t = origin + LONGLONG(index)*10000000/60;
    bool advance = false;
    while (next && source.time <= t) {
      current = next;
      next = source.Next();
      advance = true;
    }
    if (advance) reference = ReadReference(*current, reference_staging);
    else if (index) ++repeated;
    double bright = 0, motion = 0;
    std::vector<uint8_t> probe;
    probe.reserve((w/8+1)*(h/8+1));
    for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8)
      probe.push_back(reference->DataY()[y*reference->StrideY()+x]);
    for (size_t i = 0; i < probe.size(); ++i) {
      bright += probe[i];
      if (previous.size() == probe.size()) motion += std::abs(int(probe[i])-previous[i]);
    }
    bright /= probe.size(); motion /= probe.size(); previous = std::move(probe);
    // Only one encode/decode runs at a time. Rotate order to avoid a first-session bias.
    for (size_t rank = 0; rank < trials.size(); ++rank) {
      auto& trial = trials[(rank+index)%trials.size()];
      auto& sink = *trial.sink;
      uint32_t rtp = (index+1)*1500;
      sink.pending.emplace(rtp, Reference{index,reference});
      auto start = perf::Stage::Now();
      trial.encoder->Submit(*current, LONGLONG(index)*1000000/60, index%600 == 0, stage);
      auto packet = trial.encoder->Collect();
      double ms = (perf::Stage::Now()-start)/1e6;
      if (packet.bytes.empty() || packet.key != (index%600 == 0))
        throw std::runtime_error("Empty packet or unexpected IDR schedule");
      trial.records.push_back({index,ms,bright,motion,packet.bytes.size(),packet.key});
      webrtc::EncodedImage image;
      image.SetEncodedData(webrtc::EncodedImageBuffer::Create(packet.bytes.data(),packet.bytes.size()));
      image.SetRtpTimestamp(rtp);
      image.capture_time_ms_ = (index+1)*1000LL/60;
      image._frameType = packet.key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
      trial.ready.push_back(std::move(image));
    }
    if ((index+1)%300 == 0) {
      // Keep at most five seconds of source references/packets in RAM. Decode
      // one stream at a time to avoid a driver context switch on every frame.
      for (auto& trial : trials) {
        for (auto& image : trial.ready)
          if (trial.decoder->Decode(image,false,0)) throw std::runtime_error("Project decoder Decode failed");
        trial.ready.clear();
        if (trial.sink->pending.size() > 32) throw std::runtime_error("Decoder backlog");
      }
    }
    if (index%600 == 0) std::cout << "frame=" << index << "\n" << std::flush;
  }
  for (auto& trial : trials) {
    for (auto& image : trial.ready)
      if (trial.decoder->Decode(image,false,0)) throw std::runtime_error("Project decoder Decode failed");
    trial.ready.clear();
    auto& sink = *trial.sink;
    if (!gpu::DrainDecoder(trial.decoder.get())) throw std::runtime_error("Decoder drain failed");
    sink.Flush();
    if (!sink.pending.empty() || sink.psnr.size() != trial.records.size())
      throw std::runtime_error("Missing decoded frames");
    trial.decoder->Release();
    std::string prefix = argv[3];
    auto marker = prefix.find("{config}");
    if (marker != std::string::npos) prefix.replace(marker,8,trial.name);
    std::ofstream csv(prefix+".csv");
    if (!csv) throw std::runtime_error("CSV open");
    csv << "frame,psnr_y,encode_ms,bytes,key,brightness,motion\n" << std::setprecision(10);
    for (auto& r : trial.records) csv << r.index << ',' << sink.psnr.at(r.index) << ',' << r.ms
        << ',' << r.bytes << ',' << r.key << ',' << r.brightness << ',' << r.motion << '\n';
    csv.close();
    if (!csv) throw std::runtime_error("CSV write");
    std::ofstream meta(prefix+".meta");
    meta << "source=" << std::filesystem::path(argv[2]).filename().string()
         << "\nsource_width=" << source.width << "\nsource_height=" << source.height
         << "\nfps_n=" << source.fps_n << "\nfps_d=" << source.fps_d
         << "\nduration_s=" << source.duration/1e7 << "\nwidth=" << w << "\nheight=" << h
         << "\nframes=" << trial.records.size() << "\nrepeated=" << repeated
         << "\nconfig=" << trial.name << "\nbitrate=" << bitrate
         << "\nresident_sessions=" << trials.size() << "\n";
    meta.close();
    if (!meta) throw std::runtime_error("Metadata write");
    std::cout << "PASS config=" << trial.name << " frames=" << trial.records.size()
              << " repeated=" << repeated << "\n";
  }
  return 0;
}
}  // namespace encoder_measure
