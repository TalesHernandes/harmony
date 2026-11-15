#pragma once
#include "network-state.h"
#include "api/stats/rtcstats_objects.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include <map>
#include <functional>
#include <algorithm>

namespace network {
// No candidate addresses or identifiers cross this boundary. Counters are
// differenced per SSRC report, so streams appearing/disappearing cannot spike.
class Reducer {
  bool direct_;uint64_t generation_,last_=0;
  struct Counters {uint64_t bytes=0,packets=0;int64_t lost=0;};
  std::map<std::string,Counters> previous_;
 public:
  explicit Reducer(bool direct):direct_(direct),generation_(Latest(direct).Begin()){}
  void Report(webrtc::RTCStatsReport const& report) {
    auto now=GetTickCount64();if(last_ && now-last_<950)return;
    Sample sample;sample.at=now;sample.generation=generation_;
    std::string selected;
    for(auto* transport:report.GetStatsOfType<webrtc::RTCTransportStats>())if(transport->selected_candidate_pair_id)selected=*transport->selected_candidate_pair_id;
    for(auto* pair:report.GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
      if(selected.empty()?(!pair->nominated.value_or(false)||pair->state.value_or("")!="succeeded"):pair->id()!=selected)continue;
      if(pair->current_round_trip_time)sample.rtt=*pair->current_round_trip_time*1000;
      if(pair->available_outgoing_bitrate)sample.available=*pair->available_outgoing_bitrate/1000;
      auto local=pair->local_candidate_id?report.Get(*pair->local_candidate_id):nullptr;
      auto remote=pair->remote_candidate_id?report.Get(*pair->remote_candidate_id):nullptr;
      if(local && remote) {
        auto& l=local->cast_to<webrtc::RTCLocalIceCandidateStats>();auto& r=remote->cast_to<webrtc::RTCRemoteIceCandidateStats>();
        sample.route=l.candidate_type.value_or("")=="relay"||r.candidate_type.value_or("")=="relay"?Sample::Turn:Sample::Direct;
      }
      break;
    }
    uint64_t sent=0,received=0,packets=0,lost=0;bool sendKnown=false,receiveKnown=false,lossKnown=false;
    std::map<std::string,Counters> next;
    for(auto* stream:report.GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      if(!stream->bytes_sent)continue;Counters current{*stream->bytes_sent};next[stream->id()]=current;
      auto prev=previous_.find(stream->id());if(prev!=previous_.end() && current.bytes>=prev->second.bytes){sent+=current.bytes-prev->second.bytes;sendKnown=true;}
    }
    for(auto* stream:report.GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      if(stream->jitter)sample.jitter=std::max(sample.jitter,*stream->jitter*1000);
      if(!stream->bytes_received)continue;
      Counters current{*stream->bytes_received,stream->packets_received.value_or(0),stream->packets_lost.value_or(0)};next[stream->id()]=current;
      auto prev=previous_.find(stream->id());if(prev==previous_.end())continue;
      if(current.bytes>=prev->second.bytes){received+=current.bytes-prev->second.bytes;receiveKnown=true;}
      if(stream->packets_received && stream->packets_lost && current.packets>=prev->second.packets) {
        packets+=current.packets-prev->second.packets;lost+=uint64_t(std::max<int64_t>(0,current.lost-prev->second.lost));lossKnown=true;
      }
    }
    double seconds=last_?double(now-last_)/1000:0;
    if(seconds>0){if(sendKnown)sample.send=sent*8/seconds/1000;if(receiveKnown)sample.receive=received*8/seconds/1000;}
    if(lossKnown && packets+lost)sample.loss=100.*lost/(packets+lost);
    previous_=std::move(next);last_=now;Latest(direct_).Set(sample);
  }
};
class Poller : public webrtc::RTCStatsCollectorCallback {
  Reducer reducer_;
 public:
  explicit Poller(bool direct):reducer_(direct){}
  // The same report for a second reader (the call recording).
  std::function<void(webrtc::RTCStatsReport const&)> also;
  void OnStatsDelivered(webrtc::scoped_refptr<const webrtc::RTCStatsReport> const& report) override {reducer_.Report(*report);if(also)also(*report);}
};
}
