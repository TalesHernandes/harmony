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
#pragma once
#include "internal.h"
#include "nvEncodeAPI.h"

namespace gpu {
struct EncodedPacket { std::vector<uint8_t> bytes; bool key = false; int qp = -1; };
// How many frames' worth of bits one picture may take. At one, a full picture
// (somebody joining, a layer change, a loss) had to fit in the size of an
// ordinary one and arrived blurred, then cleared up: 35 dB against 45 for the
// pictures around it at 15 Mbit/s. At four it arrives 4 to 6 dB better, for a
// 100 ms burst of about 1.3 times the rate (measured 13/11/2026 on two game
// recordings, windows/app/testes/win-codificador-relatorio.md). The screen is
// watched, not played through, so the burst is the cheaper side of the trade.
inline constexpr int kVbvFrames = 4;
// Explicit measurement overrides; omitted by every production caller.
struct NVEncTestOptions {
  int preset = 1;
  int vbv_frames = kVbvFrames;
  bool spatial_aq = false;
  bool low_latency = false;
};
class NVEncoder {
 public:
  NVEncoder(int width, int height, int fps, uint32_t bitrate, bool high_profile,
            NVEncTestOptions test = {});
  ~NVEncoder();
  void SetRate(uint32_t bitrate);
  // Submit hands the picture to the GPU and returns at once; Collect waits
  // for it and takes the bytes. The three layers are submitted together and
  // collected after, so the chip works on them at the same time: one after
  // the other they took 7.9 + 5.0 + 4.3 ms, over the 16.7 ms of a frame at
  // 60 fps (selftest, 06/10/2026). Abort undoes a Submit not collected.
  void Submit(const D3DFrameBuffer& input, int64_t timestamp, bool key, perf::Stage& stage);
  EncodedPacket Collect();
  void Abort();
 private:
  DeviceLease device_lease_;
  void Close();
  void WaitForOutput();
  NV_ENCODE_API_FUNCTION_LIST* api_ = nullptr;
  void* session_ = nullptr;
  NV_ENC_CONFIG config_{};
  NV_ENC_INITIALIZE_PARAMS init_{};
  NV_ENC_REGISTERED_PTR registered_ = nullptr;
  NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
  HANDLE completion_ = nullptr;
  bool event_registered_ = false, completion_ready_ = false;
  ComPtr<ID3D11Texture2D> texture_;
  NV_ENC_INPUT_PTR mapped_ = nullptr;  // between Submit and Collect
  int vbv_frames_ = kVbvFrames;
  bool submitted_ = false;             // EncodePicture accepted, bytes not taken yet
  int64_t submitted_ns_ = 0;
  perf::Stage* stage_ = nullptr;
  int width_, height_;
};
}  // namespace gpu

