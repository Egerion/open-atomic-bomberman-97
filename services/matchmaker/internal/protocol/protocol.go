// Package protocol is the wire contract: the message types both sides of
// PROTOCOL.md are written against, and the screens that decide whether an
// inbound field may be acted on. It holds no state and talks to nothing — the
// lobby, the relay and the WebSocket adapter all speak through it.
//
// The names and JSON tags here are FROZEN. The C++ client (IXWebSocket,
// ADR-0011) matches them byte for byte; changing one side breaks a deployed
// game.
package protocol

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Wire message-type discriminators — the top-level "type" string of every JSON
// frame. These names are the FROZEN contract the C++ client (IXWebSocket,
// ADR-0011) matches byte-for-byte; see PROTOCOL.md. Do not rename without
// bumping both sides.
const (
	// client -> server
	TypeCreateLobby   = "CreateLobby"
	TypeJoinByCode    = "JoinByCode"
	TypeListPublic    = "ListPublic"
	TypeSetReady      = "SetReady"
	TypeHeartbeat     = "Heartbeat"
	TypeCandidates    = "Candidates"
	TypeStartMatch    = "StartMatch" // request (C->S) and broadcast (S->C) share the name; direction + fields disambiguate
	TypeReanchorLobby = "ReanchorLobby"
	TypeMatchOver     = "MatchOver"     // §5.2 rematch trigger
	TypeAllocateRelay = "AllocateRelay" // relay fallback (§6, relay.go)
	// Chat is BIDIRECTIONAL under one name (like StartMatch): the client frame
	// carries only "text", the relayed frame adds the SERVER's seat/name. See §7.
	TypeChat = "Chat"

	// server -> client
	TypeRelayAllocated   = "RelayAllocated"
	TypeLobbyCreated     = "LobbyCreated"
	TypeJoinAccepted     = "JoinAccepted"
	TypeJoinRejected     = "JoinRejected"
	TypePublicList       = "PublicList"
	TypeRosterUpdate     = "RosterUpdate"
	TypeHeartbeatAck     = "HeartbeatAck"
	TypePeerCandidates   = "PeerCandidates"
	TypeReanchorAccepted = "ReanchorAccepted"
	TypeError            = "Error"
)

// JoinRejected reasons (design §1.2).
const (
	ReasonNotFound      = "not_found"
	ReasonFull          = "full"
	ReasonBuildMismatch = "build_mismatch"
	ReasonInProgress    = "in_progress"
)

// Envelope peeks the discriminator only. The concrete message unmarshals from
// the same flat bytes (fields live alongside "type", never nested).
type Envelope struct {
	Type string `json:"type"`
}

// ---- shared value types -----------------------------------------------------

// Candidate is one ICE-lite endpoint (design §1.5). Priority mirrors ICE:
// host > reflexive > relay.
type Candidate struct {
	Kind  string `json:"kind"` // "host" | "reflexive" | "relay"
	Addr  string `json:"addr"`
	Alloc string `json:"alloc,omitempty"` // relay allocation id, when kind == "relay"
}

// RosterEntry is one seat in a lobby's roster (design §1.4).
type RosterEntry struct {
	Seat        int    `json:"seat"`
	Name        string `json:"name"`
	Ready       bool   `json:"ready"`
	IsHost      bool   `json:"is_host"`
	RTTToHostMs *int   `json:"rtt_to_host_ms,omitempty"`
}

// Topology names the star input-hub for N>2 (design §1.6); for 2P the hub is
// the host seat and the star degenerates to a direct pair.
type Topology struct {
	HubSeat int `json:"hub_seat"`
}

// PublicLobby is one row of the public browser list (design §1.3).
type PublicLobby struct {
	Code        string `json:"code"`
	Name        string `json:"name"`
	Players     int    `json:"players"`
	Max         int    `json:"max"`
	HostRegion  string `json:"host_region"`
	ServerRTTMs int    `json:"server_rtt_ms"`
	BuildOK     bool   `json:"build_ok"`
}

// ---- inbound messages (client -> server) ------------------------------------

type CreateLobbyMsg struct {
	Visibility string `json:"visibility"`
	Name       string `json:"name"`
	MaxSeats   int    `json:"max_seats"`
	BuildHash  string `json:"build_hash"`
	Player     string `json:"player"`
}

type JoinByCodeMsg struct {
	Code      string `json:"code"`
	BuildHash string `json:"build_hash"`
	Player    string `json:"player"`
}

type ListPublicMsg struct {
	BuildHash string `json:"build_hash"`
}

type SetReadyMsg struct {
	Ready bool `json:"ready"`
}

type CandidatesMsg struct {
	LobbyID string      `json:"lobby_id"`
	Seat    int         `json:"seat"`
	List    []Candidate `json:"list"`
}

type StartMatchReqMsg struct {
	LobbyID   string `json:"lobby_id"`
	HostToken string `json:"host_token"`
	// Optional parity payload the host may supply; the server is config-agnostic
	// and simply echoes these into the broadcast (design §1.6, ADR-0011 hard
	// rule: the server never sees MatchConfig).
	InputDelay        *int   `json:"input_delay,omitempty"`
	MatchConfigDigest string `json:"match_config_digest,omitempty"`
}

type ReanchorLobbyMsg struct {
	Code         string `json:"code"`
	RosterDigest string `json:"roster_digest"`
}

type MatchOverMsg struct {
	LobbyID string `json:"lobby_id"`
}

// ChatMsg is the INBOUND half of Chat (§7). It carries the body and nothing
// else on purpose: a seat or a name in a client frame is ignored, so no peer can
// speak as somebody else.
type ChatMsg struct {
	Text string `json:"text"`
}

type AllocateRelayMsg struct {
	LobbyID string `json:"lobby_id"`
	// Seat is a POINTER so "omitted" is distinguishable from seat 0. It is
	// advisory: an allocation always belongs to the SENDER's own seat (a peer
	// must not be able to mint or steal another seat's handle), so a value that
	// disagrees with the sender's seat is rejected rather than honoured.
	Seat *int `json:"seat,omitempty"`
}

// ---- outbound messages (server -> client) -----------------------------------

type LobbyCreatedMsg struct {
	Type      string `json:"type"`
	Code      string `json:"code"`
	LobbyID   string `json:"lobby_id"`
	HostToken string `json:"host_token"`
	YourSeat  int    `json:"your_seat"`
}

type JoinAcceptedMsg struct {
	Type           string        `json:"type"`
	LobbyID        string        `json:"lobby_id"`
	YourSeat       int           `json:"your_seat"`
	Roster         []RosterEntry `json:"roster"`
	HostCandidates []Candidate   `json:"host_candidates"`
}

type JoinRejectedMsg struct {
	Type   string `json:"type"`
	Reason string `json:"reason"`
}

type PublicListMsg struct {
	Type    string        `json:"type"`
	Lobbies []PublicLobby `json:"lobbies"`
}

type RosterUpdateMsg struct {
	Type   string        `json:"type"`
	Roster []RosterEntry `json:"roster"`
}

type HeartbeatAckMsg struct {
	Type string `json:"type"`
}

type PeerCandidatesMsg struct {
	Type string      `json:"type"`
	Seat int         `json:"seat"`
	List []Candidate `json:"list"`
}

type StartMatchMsg struct {
	Type              string   `json:"type"`
	Seed              uint32   `json:"seed"`
	SeatAssign        []int    `json:"seat_assign"`
	MatchConfigDigest string   `json:"match_config_digest"`
	InputDelay        int      `json:"input_delay"`
	Topology          Topology `json:"topology"`
	LocalSeatsMask    int      `json:"local_seats_mask"`
}

// RelayAllocatedMsg answers AllocateRelay (§6). relay_addr is the publicly
// reachable host:port of the UDP forwarder; alloc_id is the 32-hex-char form of
// the 16 binary bytes that prefix every relayed datagram.
type RelayAllocatedMsg struct {
	Type      string `json:"type"`
	RelayAddr string `json:"relay_addr"`
	AllocID   string `json:"alloc_id"`
}

// ChatRelayMsg is the OUTBOUND half of Chat (§7), fanned out to every member of
// the sender's lobby (the sender included, so everyone sees the same order).
// Seat and name come from the server's own roster — never from the frame that
// triggered the relay.
type ChatRelayMsg struct {
	Type string `json:"type"`
	Seat int    `json:"seat"`
	Name string `json:"name"`
	Text string `json:"text"`
}

type ReanchorAcceptedMsg struct {
	Type      string `json:"type"`
	LobbyID   string `json:"lobby_id"`
	Code      string `json:"code"`
	HostToken string `json:"host_token"`
}

// ErrorMsg is the out-of-band failure channel for malformed input and rejected
// control actions that are not covered by a dedicated *Rejected reply.
type ErrorMsg struct {
	Type    string `json:"type"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

// ---- helpers ----------------------------------------------------------------

// ChatMaxBytes caps one chat line (PROTOCOL.md §7). Bytes, not runes: it is a
// wire budget, and the client's 1997 bitmap font is single-byte anyway.
const ChatMaxBytes = 120

// Field ceilings for every string a client can put on the control plane
// (PROTOCOL.md §8). Chat was screened from the start; these close the same gap
// on the fields that reach OTHER people — `player` lands in the roster and in
// every relayed Chat frame's `name`, `name` is served to strangers by
// ListPublic, and a Candidate is fanned out to the whole lobby and kept.
//
// Sized off what the client can actually produce, with slack: the 1997 node
// name is at most 39 bytes (assets::kNodeNameMax) and doubles as the lobby
// name, a build_hash is "0x" + 8 hex, a handle is 32 hex, a roster digest is 64
// hex, and an "[ipv6]:port" is at most 47.
const (
	MaxPlayerNameBytes = 48
	MaxLobbyNameBytes  = 48
	MaxCodeBytes       = 16
	MaxBuildHashBytes  = 32
	MaxHandleBytes     = 64
	MaxDigestBytes     = 96

	// A client publishes two candidates (host + reflexive). The ceiling is what
	// stops one member from making every peer punch at a thousand addresses of
	// its choosing, and from parking a large list in the lobby's memory.
	MaxCandidates          = 16
	MaxCandidateKindBytes  = 16
	MaxCandidateAddrBytes  = 64
	MaxCandidateAllocBytes = 64

	// kMaxErrorEchoBytes bounds how much of a client's own string an Error may
	// quote back, so a large "type" cannot be reflected at its full size.
	kMaxErrorEchoBytes = 40

	// MaxPublicListRows caps one PublicList answer. ListPublic is the request
	// where the smallest frame buys the largest reply to an unauthenticated
	// caller, so the reply is bounded in rows as well as in rate. Rows are
	// sorted by code, so the cut is stable rather than map-order arbitrary.
	MaxPublicListRows = 200
)

// ValidateText is the shared screen for every inbound free-text field: bounded,
// valid UTF-8, no control runes. Empty passes — an unset optional field is not
// an attack, and only chat (§7) additionally insists on a non-blank body.
//
// REJECT, NEVER REPAIR (the rule chat already followed, now applied to names
// too): a truncated display name would put a different name in the roster than
// the one the player chose, and every peer would then see the server's edit.
func ValidateText(s string, maxBytes int) bool {
	if len(s) > maxBytes {
		return false
	}
	if !utf8.ValidString(s) {
		return false
	}
	for _, r := range s {
		if r == utf8.RuneError || unicode.IsControl(r) {
			return false
		}
	}
	return true
}

// ValidateCandidates screens a Candidates list before it is stored and fanned
// out. The addresses themselves are NOT resolved or filtered — a peer's own
// LAN address is legitimate and the server has no way to tell a good one from a
// bad one — but the list is bounded in every dimension.
func ValidateCandidates(list []Candidate) bool {
	if len(list) > MaxCandidates {
		return false
	}
	for _, c := range list {
		if !ValidateText(c.Kind, MaxCandidateKindBytes) ||
			!ValidateText(c.Addr, MaxCandidateAddrBytes) ||
			!ValidateText(c.Alloc, MaxCandidateAllocBytes) {
			return false
		}
	}
	return true
}

// ClipEcho screens a client-supplied string before an Error quotes it back.
// An unknown "type" from a future client is short printable ASCII and echoes
// unchanged, which is the whole diagnostic value; anything longer or
// unprintable is replaced wholesale rather than trimmed, so the reply can be
// neither an amplifier nor a way to push control bytes into somebody's terminal
// through the log. Same rule as everywhere else: reject, never repair.
func ClipEcho(s string) string {
	if !ValidateText(s, kMaxErrorEchoBytes) {
		return "(rejected)"
	}
	return s
}

// ValidateChatText screens one inbound chat body. Chat is the only place a
// player's own typing reaches OTHER players, so it is validated and REJECTED —
// never repaired: a truncated or silently stripped line would put words in
// somebody's mouth. Returns the Error code to answer with, or "" when the text
// may be relayed verbatim.
func ValidateChatText(s string) string {
	if len(s) > ChatMaxBytes {
		return "chat_too_long"
	}
	// encoding/json already substitutes U+FFFD for invalid UTF-8 as it decodes,
	// so this guard is belt-and-braces — but the SUBSTITUTION is the real case:
	// a replacement rune means the sender's bytes did not survive the trip, and
	// forwarding a silently repaired string is exactly what §7 forbids. Both are
	// therefore refusals, not repairs.
	if !utf8.ValidString(s) {
		return "chat_invalid"
	}
	for _, r := range s {
		// Otherwise only control runes are barred. What a given client can DRAW
		// is its own business (the game's FON covers printable ASCII and drops
		// the rest); the relay has no business deciding which alphabets exist.
		if r == utf8.RuneError || unicode.IsControl(r) {
			return "chat_invalid"
		}
	}
	if strings.TrimSpace(s) == "" {
		return "chat_invalid"
	}
	return ""
}

// NormalizeBuildHash canonicalises a build_hash string ("0xA1B2C3D4") for
// equality checks: trim, lowercase, drop an optional "0x". The wire form stays
// the human-readable "0x…" string (PROTOCOL.md); only comparison is normalized.
func NormalizeBuildHash(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	return strings.TrimPrefix(s, "0x")
}

// RosterDigest is a canonical digest over a roster, used by ReanchorLobby
// (design §8.3) to prove lobby continuity when a promoted hub re-anchors. Only
// the (seat, name) pairs feed it — ready/is_host churn is deliberately excluded
// so the digest identifies "the same players in the same seats". The C++ client
// computes it identically:
//
//	for each entry in ASCENDING seat order: append "<seat>:<len(name)>:<name>;"
//	digest = lowercase-hex SHA-256 of that UTF-8 byte string
//
// The length prefix makes it injective regardless of names containing ':' or ';'.
func RosterDigest(roster []RosterEntry) string {
	sorted := append([]RosterEntry(nil), roster...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Seat < sorted[j].Seat })
	var b strings.Builder
	for _, e := range sorted {
		fmt.Fprintf(&b, "%d:%d:%s;", e.Seat, len(e.Name), e.Name)
	}
	sum := sha256.Sum256([]byte(b.String()))
	return hex.EncodeToString(sum[:])
}
