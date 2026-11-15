#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
namespace audio {
class NoiseFilter {
 public:
  virtual ~NoiseFilter() = default;
  virtual bool Available() const { return true; }
  virtual bool Process(std::vector<int16_t>& block, int rate, size_t channels, int strengthDb) = 0;
};
// Synchronous results always belong to this quantum. Preserve APM input on failure.
class FilterSlot {
 public:
  explicit FilterSlot(std::unique_ptr<NoiseFilter> backend = {}) : backend_(std::move(backend)) {}
  std::atomic<bool> enabled{true}, active{false}, nsApplied{true};
  std::atomic<bool> echo{true}, noise{true}, agc{true};
  enum Reason { Warming, Running, Disabled, Unavailable, Rejected, Exception, Shape, Deadline, Format, Apm, Alone };
  std::atomic<Reason> reason{Warming};
  static const char* ReasonText(Reason r) {
    switch(r) {
      case Alone: return "DFN em pausa: ninguém ouve (sozinho na sala ou no mudo)";
      case Running: return "ativo"; case Disabled: return "desligado nos Ajustes";
      case Unavailable: return "DLL ou modelo indisponível";
      case Rejected: return "DFN recusou o bloco"; case Exception: return "exceção no DFN";
      case Shape: return "saída DFN inválida"; case Deadline: return "DFN excedeu 10 ms";
      case Format: return "formato ADM inválido"; case Apm: return "falha no processamento APM";
      default: return "verificando 20 blocos estáveis";
    }
  }
  std::atomic<double> micGain{1};
  std::atomic<int> strength{50};
  std::atomic<uint64_t> fallbacks{0};
  // How long the backend took on the last block it was handed, probes
  // included (0 when it was not called): the call recording prints the
  // slowest of each second, since "DFN excedeu 10 ms" is a matter of how far.
  std::atomic<uint32_t> lastMicros{0};
  // The last block was over 10 ms but kept: the caller allowed it with
  // `tolerateLate`, since DFN's output is still right, only late.
  std::atomic<bool> lastLate{false};
  bool Process(std::vector<int16_t>& samples, int rate, size_t channels, int overrideStrength = -1, bool publish = true,
               bool tolerateLate = false) {
    lastMicros=0; lastLate=false;
    if (!enabled) { active=false; reason=Disabled; return false; }
    block_.assign(samples.begin(), samples.end());
    auto& block = block_;
    auto start=std::chrono::steady_clock::now(); bool ok=false;
    reason=backend_ && backend_->Available() ? Rejected : Unavailable;
    try { if (backend_) ok=backend_->Process(block,rate,channels,std::clamp(overrideStrength<0?strength.load():overrideStrength,10,100)); } catch (...) { reason=Exception; }
    auto took=std::chrono::steady_clock::now()-start;
    lastMicros=uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(took).count());
    const bool late = took >= std::chrono::milliseconds(10);
    if (ok && block.size()==samples.size() && late && tolerateLate) {
      samples.swap(block); active=publish; reason=Deadline; lastLate=true; return true;
    }
    if (!ok || block.size()!=samples.size() || late) {
      if (ok) reason=block.size()!=samples.size() ? Shape : Deadline;
      active=false; ++fallbacks; return false;
    }
    samples.swap(block); active=publish; reason=publish ? Running : Warming; return true;
  }
 private:
  // Process is serialized by the capture transport, including recovery probes.
  std::vector<int16_t> block_;
  std::unique_ptr<NoiseFilter> backend_;
};
}
