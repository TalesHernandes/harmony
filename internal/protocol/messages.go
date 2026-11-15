// Package protocol holds the wire types of the signaling protocol.
//
// docs/PROTOCOL.md is the source of truth. Every message type constant here
// must appear there and vice versa; contract_test.go enforces that.
package protocol

import "encoding/json"

// Message type tags (the "t" field).
const (
	// Client → server only.
	TypeHello = "hello"
	TypePing  = "ping"

	// Relayed 1:1 between the two clients, byte for byte.
	TypeRing   = "ring"
	TypeAccept = "accept"
	TypeReject = "reject"
	TypeHangup = "hangup"
	TypeOffer  = "offer"
	TypeAnswer = "answer"
	TypeICE    = "ice"
	TypeState  = "state"

	// Server → client only.
	TypeWelcome = "welcome"
	// TypePushSent answers a ring that found nobody in the room but did
	// reach a registered device by Web Push: the peer's app is closed, not
	// missing. Without it the caller got the no_peer error and showed a red
	// banner while the other phone was actually ringing.
	TypePushSent   = "push-sent"
	TypePeerJoined = "peer-joined"
	TypePeerLeft   = "peer-left"
	TypeError      = "error"
	TypePong       = "pong"
)

// AllTypes lists every message type, for the contract test.
var AllTypes = []string{
	TypeHello, TypePing,
	TypeRing, TypeAccept, TypeReject, TypeHangup, TypeOffer, TypeAnswer, TypeICE, TypeState,
	TypeWelcome, TypePushSent, TypePeerJoined, TypePeerLeft, TypeError, TypePong,
}

// Relayed reports whether a message of type t is forwarded to the peer as-is.
func Relayed(t string) bool {
	switch t {
	case TypeRing, TypeAccept, TypeReject, TypeHangup,
		TypeOffer, TypeAnswer, TypeICE, TypeState:
		return true
	}
	return false
}

// SessionMaxBytes caps the client-chosen session id (docs/PROTOCOL.md §2).
// It is opaque to the server and only ever compared for equality, so the
// limit exists to bound memory, not to validate a format.
const SessionMaxBytes = 128

// Error codes carried by TypeError messages.
const (
	CodeBadHello     = "bad_hello"
	CodeUnauthorized = "unauthorized"
	CodeBadRoom      = "bad_room"
	CodeRoomFull     = "room_full"
	CodeNoPeer       = "no_peer"
	CodeBadType      = "bad_type"
	CodeBadMessage   = "bad_message"
)

// Envelope is the minimal shape shared by every message: just the tag.
type Envelope struct {
	T string `json:"t"`
}

// Hello is the first message a client must send after connecting.
type Hello struct {
	T      string `json:"t"`
	Room   string `json:"room"`
	Token  string `json:"token"`
	Client string `json:"client"`
	// Session identifies one client *instance* across WebSocket
	// reconnections, so a client coming back after a network switch reclaims
	// its own slot instead of colliding with the ghost of its previous
	// connection. Optional: a client that omits it behaves exactly as before,
	// which is what keeps an older cached bundle working after a deploy.
	Session string `json:"session,omitempty"`
}

// ICEServer mirrors RTCIceServer from the WebRTC API.
type ICEServer struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username,omitempty"`
	Credential string   `json:"credential,omitempty"`
}

// Welcome is sent once after a successful hello.
type Welcome struct {
	T          string      `json:"t"`
	Self       string      `json:"self"`
	Peer       *string     `json:"peer"` // null when the room was empty
	Polite     bool        `json:"polite"`
	ICEServers []ICEServer `json:"iceServers"`
}

// PeerEvent is used for both peer-joined and peer-left.
type PeerEvent struct {
	T  string `json:"t"`
	ID string `json:"id"`
}

// Error is sent by the server; some codes are followed by a close.
type Error struct {
	T       string `json:"t"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

// Pong answers a client ping.
type Pong struct {
	T string `json:"t"`
}

// Marshal is a small helper that panics on failure; the types above are
// plain structs and cannot fail to encode.
func Marshal(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic("protocol: marshal: " + err.Error())
	}
	return b
}
