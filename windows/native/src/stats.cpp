#include "stats.h"

#include <cstdio>

#include "api/stats/rtcstats_objects.h"

namespace stats {

namespace {
// "video/H264" -> "H264", as the bench writes it.
std::string Bare(const std::string& mime) {
  size_t slash = mime.find('/');
  return slash == std::string::npos ? mime : mime.substr(slash + 1);
}
}  // namespace

void Poller::Request(webrtc::PeerConnectionInterface* pc) {
  if (pc) pc->GetStats(this);
}

std::string Poller::Text() const {
  std::lock_guard<std::mutex> lock(mu_);
  return text_;
}

json::Value Poller::Counters() const {
  std::lock_guard<std::mutex> lock(mu_);
  return counters_;
}

void Poller::OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  double t = double(report->timestamp().us()) / 1e6;
  std::map<std::string, std::string> codecs;
  for (const auto* c : report->GetStatsOfType<webrtc::RTCCodecStats>())
    codecs[c->id()] = c->mime_type.value_or("");
  std::string out;
  char line[512];
  json::Value raw = json::Value::Object();
  raw["t"] = t * 1000;
  raw["out"] = json::Value::Array();
  raw["entra"] = json::Value::Array();

  for (const auto* p : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
    if (!p->nominated.value_or(false) || p->state.value_or("") != "succeeded") continue;
    std::snprintf(line, sizeof line, "rede rtt=%.0fms banda-estimada=%.0fkbit/s\n",
                  p->current_round_trip_time.value_or(0) * 1000, p->available_outgoing_bitrate.value_or(0) / 1000);
    out += line;
    raw["rtt"] = p->current_round_trip_time.value_or(0);
    raw["banda"] = p->available_outgoing_bitrate.value_or(0);
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto* s : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
    if (s->kind.value_or("") != "video") continue;
    Prev& p = prev_["out" + s->id()];
    uint64_t bytes = s->bytes_sent.value_or(0);
    uint32_t frames = s->frames_encoded.value_or(0);
    double enc = s->total_encode_time.value_or(0);
    double dt = p.t > 0 ? t - p.t : 0;
    double kbps = dt > 0 ? double(bytes - p.bytes) * 8 / dt / 1000 : 0;
    double fps = dt > 0 ? double(frames - p.frames) / dt : 0;
    double ms = frames > p.frames ? (enc - p.encode_s) * 1000 / double(frames - p.frames) : 0;
    p = {t, bytes, frames, enc};
    std::snprintf(line, sizeof line,
                  "manda %s %-5s %ux%u %.1fq/s %.0fkbit/s cod=%.2fms %s ativa=%d limite=%s chave=%u pli=%u\n",
                  codecs[s->codec_id.value_or("")].c_str(), s->rid.value_or("-").c_str(), s->frame_width.value_or(0),
                  s->frame_height.value_or(0), fps, kbps, ms, s->encoder_implementation.value_or("?").c_str(),
                  s->active.value_or(true) ? 1 : 0, s->quality_limitation_reason.value_or("?").c_str(),
                  s->key_frames_encoded.value_or(0), s->pli_count.value_or(0));
    out += line;
    json::Value o = json::Value::Object();
    o["chave"] = s->id();
    o["fonte"] = "tela";  // the only video this program sends
    o["rid"] = s->rid.value_or("");
    o["codec"] = Bare(codecs[s->codec_id.value_or("")]);
    o["encoder"] = s->encoder_implementation.value_or("");
    o["eficiente"] = s->power_efficient_encoder.value_or(false);
    o["ativa"] = s->active.value_or(true);
    o["largura"] = int64_t(s->frame_width.value_or(0));
    o["altura"] = int64_t(s->frame_height.value_or(0));
    o["quadros"] = int64_t(s->frames_sent.value_or(frames));
    if (s->frames_per_second) o["fpsNavegador"] = *s->frames_per_second;
    o["tempoCod"] = enc;
    o["bytes"] = int64_t(bytes);
    o["limitacao"] = s->quality_limitation_reason.value_or("");
    raw["out"].Push(o);
  }
  for (const auto* s : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
    std::string kind = s->kind.value_or("");
    Prev& p = prev_["in" + s->id()];
    uint64_t bytes = s->bytes_received.value_or(0);
    double dt = p.t > 0 ? t - p.t : 0;
    double kbps = dt > 0 ? double(bytes - p.bytes) * 8 / dt / 1000 : 0;
    if (kind == "video") {
      uint32_t frames = s->frames_decoded.value_or(0);
      double dec = s->total_decode_time.value_or(0);
      double fps = dt > 0 ? double(frames - p.frames) / dt : 0;
      double ms = frames > p.frames ? (dec - p.encode_s) * 1000 / double(frames - p.frames) : 0;
      uint64_t emitted = s->jitter_buffer_emitted_count.value_or(0);
      double jb = emitted ? s->jitter_buffer_delay.value_or(0) * 1000 / double(emitted) : 0;
      p = {t, bytes, frames, dec};
      if (frames == 0) continue;
      std::snprintf(line, sizeof line,
                    "recebe %s mid=%s %ux%u %.1fq/s %.0fkbit/s dec=%.2fms %s espera=%.0fms travadas=%u perdidos=%u\n",
                    codecs[s->codec_id.value_or("")].c_str(), s->mid.value_or("-").c_str(), s->frame_width.value_or(0),
                    s->frame_height.value_or(0), fps, kbps, ms, s->decoder_implementation.value_or("?").c_str(), jb,
                    s->freeze_count.value_or(0), s->frames_dropped.value_or(0));
      out += line;
      json::Value o = json::Value::Object();
      o["chave"] = s->id();
      o["codec"] = Bare(codecs[s->codec_id.value_or("")]);
      o["decoder"] = s->decoder_implementation.value_or("");
      o["eficiente"] = s->power_efficient_decoder.value_or(false);
      o["largura"] = int64_t(s->frame_width.value_or(0));
      o["altura"] = int64_t(s->frame_height.value_or(0));
      o["quadros"] = int64_t(frames);
      if (s->frames_per_second) o["fpsNavegador"] = *s->frames_per_second;
      o["tempoDec"] = dec;
      o["bytes"] = int64_t(bytes);
      o["perdidos"] = int64_t(s->frames_dropped.value_or(0));
      o["travadas"] = int64_t(s->freeze_count.value_or(0));
      o["esperaMs"] = jb;
      raw["entra"].Push(o);
    } else if (kind == "audio") {
      p = {t, bytes, 0, 0};
      if (bytes == 0) continue;
      std::snprintf(line, sizeof line, "recebe audio mid=%s %.0fkbit/s nivel=%.3f\n", s->mid.value_or("-").c_str(),
                    kbps, s->audio_level.value_or(0));
      out += line;
    }
  }
  text_ = out;
  counters_ = raw;
}

}  // namespace stats
