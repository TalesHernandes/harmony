// Sonda 2: o navegador oferta simulcast, o Pion responde — quantos OnTrack?
//
// Esta e a pergunta que o Codex nao achou validada em lugar nenhum, e e a que
// decide o desenho inteiro: se o Pion entrega as tres camadas separadas, com
// RID, entao o SFU pode escolher uma por assinante. Se entrega uma so, nao ha
// simulcast a implementar.
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/pion/webrtc/v4"
)

type sinal struct {
	Type string `json:"type"`
	SDP  string `json:"sdp"`
}

func main() {
	m := &webrtc.MediaEngine{}
	if err := m.RegisterDefaultCodecs(); err != nil {
		panic(err)
	}
	// Sem isto o Pion nao le a extensao que carrega o RID, e sem o RID nao ha
	// como dizer uma camada da outra.
	if err := webrtc.ConfigureSimulcastExtensionHeaders(m); err != nil {
		panic(err)
	}
	api := webrtc.NewAPI(webrtc.WithMediaEngine(m))

	var mu sync.Mutex
	vistos := map[string]int{}
	pronto := make(chan struct{})
	var uma sync.Once

	http.HandleFunc("/offer", func(w http.ResponseWriter, r *http.Request) {
		var oferta sinal
		if err := json.NewDecoder(r.Body).Decode(&oferta); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		pc, err := api.NewPeerConnection(webrtc.Configuration{})
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		pc.OnTrack(func(t *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
			rid := t.RID()
			if rid == "" {
				rid = "(sem rid)"
			}
			mu.Lock()
			vistos[rid]++
			n := len(vistos)
			mu.Unlock()
			fmt.Printf("  OnTrack: rid=%-8q ssrc=%d codec=%s\n", rid, t.SSRC(), t.Codec().MimeType)
			if n >= 3 {
				uma.Do(func() { close(pronto) })
			}
			// Drenar, senao o Pion nao continua lendo essa camada.
			buf := make([]byte, 1500)
			for {
				if _, _, err := t.Read(buf); err != nil {
					return
				}
			}
		})
		if err := pc.SetRemoteDescription(webrtc.SessionDescription{
			Type: webrtc.SDPTypeOffer, SDP: oferta.SDP,
		}); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		resposta, err := pc.CreateAnswer(nil)
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		feito := webrtc.GatheringCompletePromise(pc)
		if err := pc.SetLocalDescription(resposta); err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		<-feito
		_ = json.NewEncoder(w).Encode(sinal{
			Type: "answer", SDP: pc.LocalDescription().SDP,
		})
	})

	// Abrir a porta ANTES de dizer que esta pronto, e gritar se nao abrir.
	// Engolir este erro custou uma rodada inteira: a 8099 estava ocupada por um
	// processo esquecido de outra medicao, o servidor nunca subiu, e o
	// navegador ficou conversando com aquilo.
	lis, err := net.Listen("tcp", "127.0.0.1:8123")
	if err != nil {
		panic("nao consegui abrir a porta: " + err.Error())
	}
	srv := &http.Server{}
	go func() {
		if err := srv.Serve(lis); err != nil && err != http.ErrServerClosed {
			fmt.Println("servidor morreu:", err)
		}
	}()
	fmt.Println("servidor de sonda em", lis.Addr(), "— esperando o navegador...")

	select {
	case <-pronto:
		fmt.Println("\n=== TRES CAMADAS CHEGARAM ===")
	case <-time.After(25 * time.Second):
		fmt.Println("\n=== tempo esgotado ===")
	}
	mu.Lock()
	fmt.Printf("camadas distintas vistas: %d\n", len(vistos))
	for rid, n := range vistos {
		fmt.Printf("  %-10s %d faixa(s)\n", rid, n)
	}
	mu.Unlock()
}
