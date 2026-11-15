package sfu

import (
	"fmt"
	"testing"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// The cost of offering one screen packet to every subscriber, which is what the
// read loop does for each of the three layers. Seven subscribers is a room of
// eight; two of them on each small layer and three on the big one, so most
// offers are to somebody who is not on that layer.
func BenchmarkScreenFanout(b *testing.B) {
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000}
	rids := []string{"alta", "media", "baixa"}
	p := &publication{owner: &session{id: "publisher"}, layers: map[string]*layer{
		"alta": {ssrc: 11}, "media": {ssrc: 22}, "baixa": {ssrc: 33}}}
	ssrc := map[string]uint32{"alta": 11, "media": 22, "baixa": 33}
	var outs []*forwarder
	for i := 0; i < 7; i++ {
		local, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", fmt.Sprint("screen", i))
		if err != nil {
			b.Fatal(err)
		}
		f := &forwarder{pub: p, local: local, want: rids[i%3], bound: true}
		outs = append(outs, f)
	}
	payload := make([]byte, 1100)
	pkt := rtp.Packet{Header: rtp.Header{Version: 2}, Payload: payload}
	// Everybody gets a first full picture on their own layer.
	for _, rid := range rids {
		pkt.SSRC = ssrc[rid]
		for _, f := range outs {
			f.write(rid, &pkt, true)
		}
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		rid := rids[i%3]
		pkt.SSRC = ssrc[rid]
		pkt.SequenceNumber++
		pkt.Timestamp += 1500
		for _, f := range outs {
			f.write(rid, &pkt, false)
		}
	}
}
