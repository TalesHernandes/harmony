#pragma once
// Plays the harmony's own sounds (sons/LISTA.md): the files ship inside the
// .exe (app.rc, 301–307), and when each is heard is sound-rules.h.
//
// The short ones go out through their own wave output on a thread, as the
// message blip does, so a pop never stops the ring; the ring loops through
// PlaySound from memory. Nothing plays while the program measures itself.
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include "sound-rules.h"
#include "../../native/src/log.h"

#define HARMONY_SOUND_ENTRAR 301
#define HARMONY_SOUND_SAIR 302
#define HARMONY_SOUND_MUTAR 303
#define HARMONY_SOUND_DESMUTAR 304
#define HARMONY_SOUND_TELA_COMECOU 305
#define HARMONY_SOUND_TELA_ACABOU 306
#define HARMONY_SOUND_TOQUE 307

namespace sounds {
// A WAV inside the .exe: its format and its samples, found by the chunks.
struct Wave { WAVEFORMATEX format{}; const uint8_t* data = nullptr; uint32_t bytes = 0; const uint8_t* file = nullptr; uint32_t fileBytes = 0; };

inline Wave Load(int id) {
  Wave w;
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
  if (!r) return w;
  HGLOBAL g = LoadResource(nullptr, r); if (!g) return w;
  auto p = static_cast<const uint8_t*>(LockResource(g)); uint32_t n = SizeofResource(nullptr, r);
  if (!p || n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return w;
  w.file = p; w.fileBytes = n;
  for (uint32_t at = 12; at + 8 <= n;) {
    uint32_t size; memcpy(&size, p + at + 4, 4);
    if (at + 8 + size > n) break;
    if (!memcmp(p + at, "fmt ", 4) && size >= 16) memcpy(&w.format, p + at + 8, 16);
    if (!memcmp(p + at, "data", 4)) { w.data = p + at + 8; w.bytes = size; }
    at += 8 + size + (size & 1);
  }
  if (w.format.wFormatTag != WAVE_FORMAT_PCM) w.data = nullptr;
  return w;
}

class Player {
 public:
  // post runs a block on the UI thread, where the rules live.
  Player(std::function<bool()> silent, std::function<void(std::function<void()>)> post)
      : silent_(std::move(silent)), post_(std::move(post)),
        rules_([this](Sound s) { Play(s); },
            [] { return int64_t(GetTickCount64()); },
            [this](int64_t ms, std::function<void()> block) -> std::function<void()> {
              auto cancelled = std::make_shared<std::atomic<bool>>(false);
              std::thread([ms, block = std::move(block), cancelled, post = post_] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                if (!*cancelled) post([block, cancelled] { if (!*cancelled) block(); });
              }).detach();
              return [cancelled] { *cancelled = true; };
            }) {}

  Rules& rules() { return rules_; }

  // After every change of the direct call: the ring follows it.
  void RingFor() {
    int want = silent_() ? 0 : rules_.Ring() ? 2 : rules_.Ringback() ? 1 : 0;
    if (want == ringing_) return;
    ringing_ = want;
    if (!want) { PlaySoundW(nullptr, nullptr, 0); return; }
    // The person called hears the ring as it is; the person calling hears it
    // from the other end of the line, about 10 dB lower (owner, 29/10/2026).
    // PlaySound has no volume, so the quieter one is a copy made once.
    auto& w = want == 2 ? Ring() : Ringback();
    if (w.empty()) { logf("sound: ring unavailable"); return; }
    PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
  }
  void StopRing() { ringing_ = 0; PlaySoundW(nullptr, nullptr, 0); }

 private:
  static int Id(Sound s) {
    switch (s) {
      case Sound::Entrar: return HARMONY_SOUND_ENTRAR;
      case Sound::Sair: return HARMONY_SOUND_SAIR;
      case Sound::Mutar: return HARMONY_SOUND_MUTAR;
      case Sound::Desmutar: return HARMONY_SOUND_DESMUTAR;
      case Sound::TelaComecou: return HARMONY_SOUND_TELA_COMECOU;
      case Sound::TelaAcabou: return HARMONY_SOUND_TELA_ACABOU;
    }
    return 0;
  }
  void Play(Sound s) {
    if (silent_()) return;
    logf("sound: %d", int(s));
    Wave w = Load(Id(s));
    if (!w.data) { logf("sound: %d unavailable", int(s)); return; }
    std::thread([w] {
      HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!ready) return;
      HWAVEOUT out{}; WAVEFORMATEX f = w.format; f.cbSize = 0;
      if (waveOutOpen(&out, WAVE_MAPPER, &f, DWORD_PTR(ready), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { CloseHandle(ready); return; }
      // The samples are read straight from the .exe's image, never written.
      WAVEHDR h{}; h.lpData = reinterpret_cast<LPSTR>(const_cast<uint8_t*>(w.data)); h.dwBufferLength = w.bytes;
      if (waveOutPrepareHeader(out, &h, sizeof(h)) == MMSYSERR_NOERROR) {
        ResetEvent(ready);
        if (waveOutWrite(out, &h, sizeof(h)) == MMSYSERR_NOERROR)
          while (!(h.dwFlags & WHDR_DONE)) if (WaitForSingleObject(ready, 2000) != WAIT_OBJECT_0) { waveOutReset(out); break; }
        waveOutUnprepareHeader(out, &h, sizeof(h));
      }
      waveOutClose(out); CloseHandle(ready);
    }).detach();
  }
  static std::vector<uint8_t> const& Ring() {
    static std::vector<uint8_t> w = [] { Wave r = Load(HARMONY_SOUND_TOQUE); return r.file ? std::vector<uint8_t>(r.file, r.file + r.fileBytes) : std::vector<uint8_t>{}; }();
    return w;
  }
  static std::vector<uint8_t> const& Ringback() {
    static std::vector<uint8_t> w = [] {
      auto copy = Ring();
      Wave r = Load(HARMONY_SOUND_TOQUE);
      if (copy.empty() || !r.data || r.format.wBitsPerSample != 16) return std::vector<uint8_t>{};
      auto* s = reinterpret_cast<int16_t*>(copy.data() + (r.data - r.file));
      for (uint32_t i = 0; i < r.bytes / 2; ++i) s[i] = int16_t(s[i] * 0.32);
      return copy;
    }();
    return w;
  }
  std::function<bool()> silent_;
  std::function<void(std::function<void()>)> post_;
  Rules rules_;
  int ringing_ = 0;
};
}  // namespace sounds
