/*
 * This copyright notice applies to this header file only:
 *
 * Copyright (c) 2010-2024 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the software, and to permit persons to whom the
 * software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// This software contains source code provided by NVIDIA Corporation.
#include "nvenc.h"
#include <cstdio>
#include "../src/log.h"
#include <atomic>

namespace gpu {
namespace {
std::atomic<bool> synchronous_selftest{false};
void NVCheck(NVENCSTATUS status, const char* operation) {
  if (status != NV_ENC_SUCCESS) {
    std::ostringstream error;
    error << operation << " NVENCSTATUS=" << static_cast<int>(status);
    throw std::runtime_error(error.str());
  }
}
struct Driver {
  HMODULE module = nullptr;
  NV_ENCODE_API_FUNCTION_LIST api{};
  Driver() {
    // Keep the driver loaded until exit: every live session retains function pointers.
    module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("nvEncodeAPI64.dll unavailable");
    auto create = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
        GetProcAddress(module, "NvEncodeAPICreateInstance"));
    if (!create) { FreeLibrary(module); module = nullptr; throw std::runtime_error("NvEncodeAPICreateInstance missing"); }
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    auto result = create(&api);
    if (result != NV_ENC_SUCCESS) { FreeLibrary(module); module = nullptr; NVCheck(result, "NvEncodeAPICreateInstance"); }
  }
  ~Driver() { if (module) FreeLibrary(module); }
};
}
NVEncoder::NVEncoder(int width, int height, int fps, uint32_t bitrate, bool high_profile, NVEncTestOptions test)
    : width_(width), height_(height), vbv_frames_(test.vbv_frames) {
  try {
    static Driver driver;
    api_ = &driver.api;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = Device(); open.apiVersion = NVENCAPI_VERSION;
    NVCheck(api_->nvEncOpenEncodeSessionEx(&open, &session_), "OpenEncodeSession");
    NV_ENC_CAPS_PARAM caps{};caps.version=NV_ENC_CAPS_PARAM_VER;
    caps.capsToQuery=NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;int async=0;
    NVCheck(api_->nvEncGetEncodeCaps(session_,NV_ENC_CODEC_H264_GUID,&caps,&async),"AsyncEncodeCaps");
    if(!async&&!synchronous_selftest.load())throw std::runtime_error("NVENC asynchronous D3D11 encoding unavailable");
    if ((test.preset != 1 && test.preset != 3 && test.preset != 4) ||
        test.vbv_frames < 1 || test.vbv_frames > 4)
      throw std::runtime_error("Invalid NVENC measurement options");
    const GUID preset_guid = test.preset == 3 ? NV_ENC_PRESET_P3_GUID :
        test.preset == 4 ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P1_GUID;
    const auto tuning = test.low_latency ? NV_ENC_TUNING_INFO_LOW_LATENCY :
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NVCheck(api_->nvEncGetEncodePresetConfigEx(session_, NV_ENC_CODEC_H264_GUID,
        preset_guid, tuning, &preset), "GetPresetConfig");
    config_ = preset.presetCfg;
    config_.profileGUID = high_profile ? NV_ENC_H264_PROFILE_HIGH_GUID : NV_ENC_H264_PROFILE_BASELINE_GUID;
    config_.gopLength = NVENC_INFINITE_GOPLENGTH; config_.frameIntervalP = 1;
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = bitrate; config_.rcParams.maxBitRate = bitrate;
    config_.rcParams.vbvBufferSize = std::max<uint32_t>(1, bitrate / fps) * test.vbv_frames;
    config_.rcParams.vbvInitialDelay = config_.rcParams.vbvBufferSize;
    config_.rcParams.enableLookahead = 0; config_.rcParams.enableAQ = test.spatial_aq ? 1 : 0;
    if (test.spatial_aq) config_.rcParams.aqStrength = 0;
    config_.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    config_.encodeCodecConfig.h264Config.outputAUD = 1;
    config_.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_42;
    config_.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    init_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_.encodeGUID = NV_ENC_CODEC_H264_GUID; init_.presetGUID = preset_guid;
    init_.encodeWidth = width; init_.encodeHeight = height;
    init_.darWidth = width; init_.darHeight = height;
    init_.frameRateNum = fps; init_.frameRateDen = 1;
    // DXGI duplication runs on the capture thread. Synchronous LockBitstream
    // can contend for this same D3D device while AcquireNextFrame is running
    // (NVENC programming guide, threading model). Wait outside the driver.
    init_.enableEncodeAsync = synchronous_selftest.load()?0:1; init_.enablePTD = 1;
    init_.tuningInfo = tuning;
    init_.encodeConfig = &config_;
    NVCheck(api_->nvEncInitializeEncoder(session_, &init_), "InitializeEncoder");
    if(init_.enableEncodeAsync) {
    completion_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!completion_)throw std::runtime_error("NVENC completion event creation failed");
    NV_ENC_EVENT_PARAMS event{};event.version=NV_ENC_EVENT_PARAMS_VER;event.completionEvent=completion_;
    NVCheck(api_->nvEncRegisterAsyncEvent(session_,&event),"RegisterAsyncEvent");event_registered_=true;
    }
    logf("screen encoder NVENC async=%u resolution=%dx%d fps=%d",init_.enableEncodeAsync,width,height,fps);
    texture_ = Texture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.resourceToRegister = texture_.Get(); resource.width = width; resource.height = height;
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVCheck(api_->nvEncRegisterResource(session_, &resource), "RegisterResource");
    registered_ = resource.registeredResource;
    NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
    buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    NVCheck(api_->nvEncCreateBitstreamBuffer(session_, &buffer), "CreateBitstreamBuffer");
    bitstream_ = buffer.bitstreamBuffer;
  } catch (...) { Close(); throw; }
}
NVEncoder::~NVEncoder() { Close(); }
void NVEncoder::Close() {
  if (!session_) return;
  Abort();
  if (bitstream_) api_->nvEncDestroyBitstreamBuffer(session_, bitstream_);
  if (registered_) api_->nvEncUnregisterResource(session_, registered_);
  if(event_registered_) {
    NV_ENC_EVENT_PARAMS event{};event.version=NV_ENC_EVENT_PARAMS_VER;event.completionEvent=completion_;
    api_->nvEncUnregisterAsyncEvent(session_,&event);event_registered_=false;
  }
  api_->nvEncDestroyEncoder(session_);
  if(completion_) {CloseHandle(completion_);completion_=nullptr;}
  session_ = nullptr; bitstream_ = nullptr; registered_ = nullptr;
}
void NVEncoder::SetRate(uint32_t bitrate) {
  if(config_.rcParams.averageBitRate==bitrate)return;
  static perf::Stage& stageEncodeReconfigure = perf::Get("encode.reconfigure");
  auto timer=stageEncodeReconfigure.Begin();
  config_.rcParams.averageBitRate = bitrate; config_.rcParams.maxBitRate = bitrate;
  // The same allowance as at creation: the rate moves all the time, and a
  // buffer reset to one frame here would undo it within the first second.
  config_.rcParams.vbvBufferSize = std::max<uint32_t>(1, bitrate / init_.frameRateNum) * vbv_frames_;
  config_.rcParams.vbvInitialDelay = config_.rcParams.vbvBufferSize;
  NV_ENC_RECONFIGURE_PARAMS parameters{};
  parameters.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  parameters.reInitEncodeParams = init_;
  NVCheck(api_->nvEncReconfigureEncoder(session_, &parameters), "ReconfigureEncoder");
  timer.Done();
}
void NVEncoder::Submit(const D3DFrameBuffer& input, int64_t timestamp, bool key, perf::Stage& stage) {
  {
    static perf::Stage& stageScale = perf::Get("scale");
    auto timer = stageScale.Begin();
    Blit(input, texture_.Get(), RECT{0,0,input.width(),input.height()}, RECT{0,0,width_,height_});
    timer.Done();
  }
  stage_ = &stage;
  submitted_ns_ = perf::Stage::Now();
  NV_ENC_MAP_INPUT_RESOURCE mapped{}; mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  mapped.registeredResource = registered_;
  NVCheck(api_->nvEncMapInputResource(session_, &mapped), "MapInputResource");
  mapped_ = mapped.mappedResource;
  NV_ENC_PIC_PARAMS picture{}; picture.version = NV_ENC_PIC_PARAMS_VER;
  picture.inputBuffer = mapped.mappedResource; picture.bufferFmt = mapped.mappedBufferFmt;
  picture.inputWidth = width_; picture.inputHeight = height_;
  picture.outputBitstream = bitstream_; picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  picture.completionEvent=completion_;
  picture.inputTimeStamp = timestamp;
  if (key) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  NVCheck(api_->nvEncEncodePicture(session_, &picture), "EncodePicture");
  submitted_ = true;
  completion_ready_=false;
}
void NVEncoder::WaitForOutput() {
  if(!completion_||!submitted_||completion_ready_)return;
  static perf::Stage& stageEncodeCompletionWait = perf::Get("encode.completion_wait");
  auto timer=stageEncodeCompletionWait.Begin();
  if(WaitForSingleObject(completion_,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("NVENC completion wait failed");
  completion_ready_=true;timer.Done();
}
EncodedPacket NVEncoder::Collect() {
  bool locked = false;
  try {
    WaitForOutput();
    NV_ENC_LOCK_BITSTREAM output{}; output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = bitstream_; output.doNotWait = 0;
    NVCheck(api_->nvEncLockBitstream(session_, &output), "LockBitstream");
    locked = true;
    submitted_ = false;
    EncodedPacket packet;
    auto bytes = static_cast<const uint8_t*>(output.bitstreamBufferPtr);
    packet.bytes.assign(bytes, bytes + output.bitstreamSizeInBytes);
    packet.key = output.pictureType == NV_ENC_PIC_TYPE_IDR;
    packet.qp = output.frameAvgQP;
    NVCheck(api_->nvEncUnlockBitstream(session_, bitstream_), "UnlockBitstream"); locked = false;
    // Forgotten only once unmapped, so a failure still leaves it to Abort.
    NVCheck(api_->nvEncUnmapInputResource(session_, mapped_), "UnmapInputResource");
    mapped_ = nullptr;
    // Submit to bytes: with the layers in flight together this is how long
    // each one took on the chip, waiting included.
    if (stage_) stage_->Record(perf::Stage::Now() - submitted_ns_);
    return packet;
  } catch (...) {
    if (locked) api_->nvEncUnlockBitstream(session_, bitstream_);
    Abort();
    throw;
  }
}
void NVEncoder::Abort() {
  // Work the chip accepted is drained before its input is unmapped (the
  // SDK unmaps after a successful lock): another layer failed, and this one's
  // picture is thrown away. Errors here are not news — the caller is already
  // falling back.
  if (submitted_) {
    try {WaitForOutput();} catch(...) {}
    NV_ENC_LOCK_BITSTREAM output{}; output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = bitstream_; output.doNotWait = 0;
    if (api_->nvEncLockBitstream(session_, &output) == NV_ENC_SUCCESS)
      api_->nvEncUnlockBitstream(session_, bitstream_);
    submitted_ = false;
  }
  if (mapped_ && api_->nvEncUnmapInputResource(session_, mapped_) == NV_ENC_SUCCESS) mapped_ = nullptr;
}
void ConfigureNVEncSelftest(bool synchronous) {synchronous_selftest=synchronous;}
}  // namespace gpu

