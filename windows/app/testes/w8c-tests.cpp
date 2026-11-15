#include "../../native/src/network-stats.h"
#include "../src/diagnostic-history.h"
#include <iostream>
#include <stdexcept>

void Check(bool value,char const* label){if(!value)throw std::runtime_error(label);std::cout<<"PASS: "<<label<<'\n';}
auto Report(uint64_t bytes,uint32_t packets,int64_t lost) {
  auto now=webrtc::Timestamp::Millis(1000);
  auto report=webrtc::RTCStatsReport::Create(now);
  auto inbound=std::make_unique<webrtc::RTCInboundRtpStreamStats>("in",now);
  inbound->bytes_received=bytes;inbound->packets_received=packets;inbound->packets_lost=lost;inbound->jitter=.025;report->AddStats(std::move(inbound));
  auto outbound=std::make_unique<webrtc::RTCOutboundRtpStreamStats>("out",now);outbound->bytes_sent=bytes;report->AddStats(std::move(outbound));
  auto pair=std::make_unique<webrtc::RTCIceCandidatePairStats>("pair",now);pair->nominated=true;pair->state="succeeded";pair->current_round_trip_time=.05;pair->available_outgoing_bitrate=1500000;pair->local_candidate_id="local";pair->remote_candidate_id="remote";report->AddStats(std::move(pair));
  auto local=std::make_unique<webrtc::RTCLocalIceCandidateStats>("local",now);local->candidate_type="relay";report->AddStats(std::move(local));
  auto remote=std::make_unique<webrtc::RTCRemoteIceCandidateStats>("remote",now);remote->candidate_type="host";report->AddStats(std::move(remote));return report;
}
int main(){try{
  network::Reducer reducer(false);reducer.Report(*Report(1000,100,10));auto sample=network::Latest(false).Read();
  Check(sample.send<0 && sample.receive<0 && sample.loss<0,"first network counters are unknown rather than invented zero rates");
  Check(sample.rtt==50 && sample.jitter==25 && sample.available==1500 && sample.route==network::Sample::Turn,"media units and TURN route come from actual stats fields");
  Sleep(1020);reducer.Report(*Report(3000,110,12));sample=network::Latest(false).Read();
  Check(sample.send>12 && sample.send<17 && sample.receive>12 && sample.receive<17,"bitrate uses byte deltas over elapsed time");
  Check(sample.loss>16.6 && sample.loss<16.7,"loss is interval packets lost divided by received plus lost");
  Sleep(1020);reducer.Report(*Report(10,1,0));sample=network::Latest(false).Read();
  Check(sample.send<0 && sample.receive<0,"counter resets never create unsigned bitrate spikes");
  network::Reducer replacement(false);replacement.Report(*Report(100,1,0));auto fresh=network::Latest(false).Read();
  reducer.Report(*Report(9999,999,99));Check(network::Latest(false).Read().generation==fresh.generation,"retired connection reports cannot overwrite a new connection");
  app::DiagnosticHistory diagnostics;
  Sleep(2300);Check(diagnostics.Read().empty(),"idle diagnostics does not poll every two seconds");
  diagnostics.InCall(true);Sleep(2300);auto history=diagnostics.Read();Check(history.size()==1 && history.back().process.working_set_bytes>0,"room diagnostics collects every two seconds");
  diagnostics.InCall(false);diagnostics.Active(true);Sleep(2300);Check(diagnostics.Read().size()==2,"open diagnostics panel retains two-second cadence");
  diagnostics.Active(true);Sleep(2100);history=diagnostics.Read();Check(history.size()==3,"opening diagnostics preserves background cadence and existing history");
  diagnostics.Active(false);Sleep(2100);Check(diagnostics.Read().size()==3,"closing diagnostics switches to the ten-second cadence");
  app::HistoryRing<app::DiagnosticRow,10800> timed;
  for(int i=0;i<10800;++i) {app::DiagnosticRow row;row.at=i*2;timed.Push(row);}
  timed.TrimBefore(0);Check(timed.Read().size()==10799,"six-hour retention uses elapsed time");
  app::HistoryRing<int> ring;for(int i=0;i<10000;++i)ring.Push(i);auto retained=ring.Read();Check(retained.size()==300 && retained.front()==9700 && retained.back()==9999,"ten minute ring stays fixed and chronological after many wraps");
  return 0;
}catch(std::exception const& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
