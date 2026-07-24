package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"sort"
	"strings"
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
	TypeAllocateRelay = "AllocateRelay" // Phase 2 (stub — see relay.go)

	// server -> client
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

// envelope peeks the discriminator only. The concrete message unmarshals from
// the same flat bytes (fields live alongside "type", never nested).
type envelope struct {
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

type createLobbyMsg struct {
	Visibility string `json:"visibility"`
	Name       string `json:"name"`
	MaxSeats   int    `json:"max_seats"`
	BuildHash  string `json:"build_hash"`
	Player     string `json:"player"`
}

type joinByCodeMsg struct {
	Code      string `json:"code"`
	BuildHash string `json:"build_hash"`
	Player    string `json:"player"`
}

type listPublicMsg struct {
	BuildHash string `json:"build_hash"`
}

type setReadyMsg struct {
	Ready bool `json:"ready"`
}

type candidatesMsg struct {
	LobbyID string      `json:"lobby_id"`
	Seat    int         `json:"seat"`
	List    []Candidate `json:"list"`
}

type startMatchReqMsg struct {
	LobbyID   string `json:"lobby_id"`
	HostToken string `json:"host_token"`
	// Optional parity payload the host may supply; the server is config-agnostic
	// and simply echoes these into the broadcast (design §1.6, ADR-0011 hard
	// rule: the server never sees MatchConfig).
	InputDelay        *int   `json:"input_delay,omitempty"`
	MatchConfigDigest string `json:"match_config_digest,omitempty"`
}

type reanchorLobbyMsg struct {
	Code         string `json:"code"`
	RosterDigest string `json:"roster_digest"`
}

type matchOverMsg struct {
	LobbyID string `json:"lobby_id"`
}

type allocateRelayMsg struct {
	LobbyID string `json:"lobby_id"`
	Seat    int    `json:"seat"`
}

// ---- outbound messages (server -> client) -----------------------------------

type lobbyCreatedMsg struct {
	Type      string `json:"type"`
	Code      string `json:"code"`
	LobbyID   string `json:"lobby_id"`
	HostToken string `json:"host_token"`
	YourSeat  int    `json:"your_seat"`
}

type joinAcceptedMsg struct {
	Type           string        `json:"type"`
	LobbyID        string        `json:"lobby_id"`
	YourSeat       int           `json:"your_seat"`
	Roster         []RosterEntry `json:"roster"`
	HostCandidates []Candidate   `json:"host_candidates"`
}

type joinRejectedMsg struct {
	Type   string `json:"type"`
	Reason string `json:"reason"`
}

type publicListMsg struct {
	Type    string        `json:"type"`
	Lobbies []PublicLobby `json:"lobbies"`
}

type rosterUpdateMsg struct {
	Type   string        `json:"type"`
	Roster []RosterEntry `json:"roster"`
}

type heartbeatAckMsg struct {
	Type string `json:"type"`
}

type peerCandidatesMsg struct {
	Type string      `json:"type"`
	Seat int         `json:"seat"`
	List []Candidate `json:"list"`
}

type startMatchMsg struct {
	Type              string   `json:"type"`
	Seed              uint32   `json:"seed"`
	SeatAssign        []int    `json:"seat_assign"`
	MatchConfigDigest string   `json:"match_config_digest"`
	InputDelay        int      `json:"input_delay"`
	Topology          Topology `json:"topology"`
	LocalSeatsMask    int      `json:"local_seats_mask"`
}

type reanchorAcceptedMsg struct {
	Type      string `json:"type"`
	LobbyID   string `json:"lobby_id"`
	Code      string `json:"code"`
	HostToken string `json:"host_token"`
}

// errorMsg is the out-of-band failure channel for malformed input and rejected
// control actions that are not covered by a dedicated *Rejected reply.
type errorMsg struct {
	Type    string `json:"type"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

// ---- helpers ----------------------------------------------------------------

// normalizeBuildHash canonicalises a build_hash string ("0xA1B2C3D4") for
// equality checks: trim, lowercase, drop an optional "0x". The wire form stays
// the human-readable "0x…" string (PROTOCOL.md); only comparison is normalized.
func normalizeBuildHash(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	return strings.TrimPrefix(s, "0x")
}

// rosterDigest is a canonical digest over a roster, used by ReanchorLobby
// (design §8.3) to prove lobby continuity when a promoted hub re-anchors. Only
// the (seat, name) pairs feed it — ready/is_host churn is deliberately excluded
// so the digest identifies "the same players in the same seats". The C++ client
// computes it identically:
//
//	for each entry in ASCENDING seat order: append "<seat>:<len(name)>:<name>;"
//	digest = lowercase-hex SHA-256 of that UTF-8 byte string
//
// The length prefix makes it injective regardless of names containing ':' or ';'.
func rosterDigest(roster []RosterEntry) string {
	sorted := append([]RosterEntry(nil), roster...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Seat < sorted[j].Seat })
	var b strings.Builder
	for _, e := range sorted {
		fmt.Fprintf(&b, "%d:%d:%s;", e.Seat, len(e.Name), e.Name)
	}
	sum := sha256.Sum256([]byte(b.String()))
	return hex.EncodeToString(sum[:])
}
