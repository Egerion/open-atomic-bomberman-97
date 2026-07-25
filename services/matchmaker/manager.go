package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"log/slog"
	"sort"
	"strings"
	"sync"
	"time"
)

// kMaxSeats is the roster ceiling (ADR-0011 decision 4: input-type-4 lets any of
// the 10 slots be remote).
const kMaxSeats = 10

// The chat rate limit (PROTOCOL.md §7), a per-CONNECTION token bucket measured
// in milliseconds of credit: one message costs kChatCreditPerMsgMs, and at most
// kChatBurstMsgs of them may be banked. So a normal exchange (a few lines, then
// a pause) never notices it, while a flood settles at one line every two
// seconds. The C++ client runs the identical bucket, so a well-behaved client
// refuses locally exactly where the server would drop.
const (
	kChatCreditPerMsgMs = 2000
	kChatBurstMsgs      = 4
)

// Control-plane rate limits (PROTOCOL.md §8, SECURITY.md). All three are token
// buckets in milliseconds of credit, and every one of them is sized far above
// what the real client produces — the C++ client sends a heartbeat every 5 s
// and a handful of one-shot frames, i.e. well under one message per second.
const (
	// Every inbound frame, whatever its type. This is the ceiling that bounds
	// everything downstream: parse cost, roster broadcasts, LOCKED→IN_PROGRESS
	// timers, log lines.
	kMsgCreditMs = 50 // → 20 frames/s sustained
	kMsgBurst    = 40

	// ListPublic serves a whole table to an UNAUTHENTICATED caller, so it is the
	// one request where a small frame buys a large answer. Its own, tighter
	// bucket keeps that ratio bounded.
	kListCreditMs = 500 // → 2 browses/s sustained
	kListBurst    = 4

	// A JoinByCode that does not seat the sender is one step of a lobby-code
	// search (32^6 ≈ 1.07e9 codes, PROTOCOL.md §3). Charged per connection AND
	// per source address, because the per-connection budget alone is reset by
	// reconnecting. A player who mistypes a code, or retries a full lobby, never
	// comes near the burst.
	kJoinFailCreditMs = 2000 // → one failed join per 2 s, per connection
	kJoinFailBurst    = 5

	kJoinFailIPCreditMs = 500 // → two failed joins per second, per source IP
	kJoinFailIPBurst    = 30
	kJoinFailIPSlots    = 4096

	// Candidates is the other fan-out request: one frame in, one frame to every
	// OTHER member out. The list is already size-capped (kMaxCandidates), and
	// this caps the rate, so the total a member can push through the fan-out is
	// bounded in both dimensions. The client publishes its list twice per lobby
	// session (once with the LAN address, once when STUN resolves), so 4/s with
	// 8 banked is orders of magnitude more than it uses.
	kCandCreditMs = 250
	kCandBurst    = 8
)

// clientConn is the transport-agnostic seam the manager pushes messages through.
// wsConn (production) adapts a WebSocket; fakeConn (tests) captures frames. This
// keeps the whole lobby state machine unit-testable without a network.
type clientConn interface {
	id() string     // stable per-connection identity
	send(v any)     // enqueue one JSON frame (non-blocking; never holds the manager lock)
	remote() string // peer address, for logging
	disconnect(reason string)
}

// lobbyState is the server-side per-lobby machine (design §5.2).
type lobbyState int

const (
	stateOpen       lobbyState = iota // accepting joins; roster + candidates live
	stateLocked                       // StartMatch broadcast; no new joins; candidate relay still live for the punch
	stateInProgress                   // match running; control plane dormant; joins → in_progress
	stateEvicted                      // freed (empty / all timed out)
)

func (s lobbyState) String() string {
	switch s {
	case stateOpen:
		return "OPEN"
	case stateLocked:
		return "LOCKED"
	case stateInProgress:
		return "IN_PROGRESS"
	case stateEvicted:
		return "EVICTED"
	default:
		return "?"
	}
}

type member struct {
	conn       clientConn
	seat       int
	name       string
	ready      bool
	buildHash  string // normalized; must equal the lobby's
	lastSeen   time.Time
	candidates []Candidate

	// Chat token bucket (§7). Starts full so a member can speak the moment it
	// arrives, and never queues or shortens a line — over-rate means DROPPED.
	chat tokenBucket
}

// newMember seats a connection with a full chat budget.
func newMember(c clientConn, seat int, name, buildHash string, now time.Time) *member {
	return &member{
		conn:      c,
		seat:      seat,
		name:      name,
		buildHash: buildHash,
		lastSeen:  now,
		chat:      newTokenBucket(kChatCreditPerMsgMs, kChatBurstMsgs),
	}
}

// connState is the per-CONNECTION accounting the Manager keeps for every open
// socket, seat or no seat. It exists from accept to close, so a connection that
// never takes a seat is still bounded and still reaped (a seatless socket used
// to be free and immortal — see SECURITY.md "idle connections").
type connState struct {
	conn      clientConn
	srcKey    string // per-IP limiter key, "" when this peer is not capped
	firstSeen time.Time
	lastSeen  time.Time

	msgs     tokenBucket // every inbound frame
	list     tokenBucket // ListPublic
	cand     tokenBucket // Candidates (fans out to every other member)
	joinFail tokenBucket // JoinByCode attempts that did not seat the sender
}

func newConnState(c clientConn, now time.Time) *connState {
	return &connState{
		conn:      c,
		firstSeen: now,
		lastSeen:  now,
		msgs:      newTokenBucket(kMsgCreditMs, kMsgBurst),
		list:      newTokenBucket(kListCreditMs, kListBurst),
		cand:      newTokenBucket(kCandCreditMs, kCandBurst),
		joinFail:  newTokenBucket(kJoinFailCreditMs, kJoinFailBurst),
	}
}

// seatless reports whether this connection holds no lobby seat. Used by the
// reaper: a seated member is governed by the heartbeat window, a seatless one
// by the (much longer) idle window.
func (m *Manager) seatlessLocked(id string) bool {
	_, ok := m.byConn[id]
	return !ok
}

// dropCounters tallies everything the control plane refuses. Aggregated on
// purpose: a log LINE per hostile frame is itself an amplifier, so drops are
// counted here and reported in one periodic line (logDrops).
type dropCounters struct {
	badJSON     uint64
	badMessage  uint64
	unknownType uint64
	overRate    uint64
	joinGuess   uint64
	chatInvalid uint64
	chatDropped uint64
	lobbyCap    uint64
	idleConns   uint64
}

type lobby struct {
	code       string
	id         string
	name       string
	visibility string // "public" | "private"
	maxSeats   int
	buildHash  string // normalized host build; all members must match
	hostToken  string
	hostSeat   int
	state      lobbyState
	members    map[int]*member // seat -> member
	createdAt  time.Time
	// lockTimerArmed stops StartMatch/MatchOver cycling from stacking one
	// LOCKED→IN_PROGRESS timer per round trip. At most one is ever outstanding.
	lockTimerArmed bool
}

func (lb *lobby) freeSeat() (int, bool) {
	for s := 0; s < lb.maxSeats; s++ {
		if _, taken := lb.members[s]; !taken {
			return s, true
		}
	}
	return 0, false
}

// connLoc pins a connection to its single lobby membership.
type connLoc struct {
	code string
	seat int
}

// Manager owns all soft lobby state behind one mutex (ADR-0011: soft in-RAM
// only, no game State ever touches it).
type Manager struct {
	mu      sync.Mutex
	lobbies map[string]*lobby   // code -> lobby
	byConn  map[string]*connLoc // conn.id() -> location
	// conns is the per-connection accounting, one entry per open socket whether
	// or not it holds a seat. Bounded by cfg.MaxConns: wsServer refuses the
	// upgrade past the cap, and every accepted connection is removed on close.
	conns map[string]*connState
	cfg   Config
	log   *slog.Logger
	now   func() time.Time // injectable clock (tests)

	// joinFailIP charges failed JoinByCode attempts per SOURCE ADDRESS as well
	// as per connection, because the per-connection budget is reset simply by
	// reconnecting and a code search does not care which socket it runs over.
	joinFailIP *ipBuckets

	drops     dropCounters // guarded by mu
	lastDrops dropCounters // reaper goroutine only, under mu

	// relay is the UDP forwarder's allocation registry (relay.go). The Manager
	// mints and frees entries; relayServer reads them on the data plane. It has
	// its own lock and never calls back here, so the order is always mu → relay.
	relay *relayTable
}

func NewManager(cfg Config, log *slog.Logger) *Manager {
	cfg = cfg.withDefaults()
	return &Manager{
		lobbies:    map[string]*lobby{},
		byConn:     map[string]*connLoc{},
		conns:      map[string]*connState{},
		cfg:        cfg,
		log:        log,
		now:        time.Now,
		joinFailIP: newIPBuckets(kJoinFailIPSlots, kJoinFailIPCreditMs, kJoinFailIPBurst),
		relay:      newRelayTable(cfg.RelayIdle, log),
	}
}

// addConn registers an accepted socket. srcKey is the per-IP limiter key for
// this connection ("" when the peer is not an address a cap should count — see
// perIPKey), remembered here so the UDP-shaped limits and the control plane
// agree on who a caller is.
func (m *Manager) addConn(c clientConn, srcKey string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	cs := newConnState(c, m.now())
	cs.srcKey = srcKey
	m.conns[c.id()] = cs
}

// connStateLocked returns this connection's accounting, creating it if the
// socket was never registered (the in-process test path; production always goes
// through addConn/removeConn).
func (m *Manager) connStateLocked(c clientConn) *connState {
	if cs, ok := m.conns[c.id()]; ok {
		return cs
	}
	cs := newConnState(c, m.now())
	cs.srcKey = perIPKey(c.remote())
	m.conns[c.id()] = cs
	return cs
}

// dispatch is the single entry point for an inbound frame.
//
// Everything here runs BEFORE any handler and applies to a caller that has
// proved nothing about itself: the global frame bucket bounds parse cost,
// broadcasts, timers and log volume in one place, and it is checked before the
// JSON is even looked at. Over-rate frames are dropped SILENTLY — answering
// each one with an Error would turn the limiter into the amplifier it exists to
// prevent (the same reasoning as the chat bucket, PROTOCOL.md §7.3).
func (m *Manager) dispatch(c clientConn, raw []byte) {
	m.mu.Lock()
	cs := m.connStateLocked(c)
	now := m.now()
	cs.lastSeen = now
	allowed := cs.msgs.allow(now)
	if !allowed {
		m.drops.overRate++
	}
	m.mu.Unlock()
	if !allowed {
		return
	}

	var env envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		m.countDrop(func(d *dropCounters) { d.badJSON++ })
		m.sendErr(c, "bad_json", "message is not valid JSON")
		return
	}
	m.touch(c) // any frame refreshes presence

	switch env.Type {
	case TypeCreateLobby:
		m.handleCreate(c, raw)
	case TypeJoinByCode:
		m.handleJoin(c, raw)
	case TypeListPublic:
		m.handleListPublic(c, raw)
	case TypeSetReady:
		m.handleSetReady(c, raw)
	case TypeHeartbeat:
		c.send(heartbeatAckMsg{Type: TypeHeartbeatAck})
	case TypeCandidates:
		m.handleCandidates(c, raw)
	case TypeStartMatch:
		m.handleStart(c, raw)
	case TypeReanchorLobby:
		m.handleReanchor(c, raw)
	case TypeMatchOver:
		m.handleMatchOver(c, raw)
	case TypeAllocateRelay:
		m.handleAllocateRelay(c, raw)
	case TypeChat:
		m.handleChat(c, raw)
	default:
		m.countDrop(func(d *dropCounters) { d.unknownType++ })
		m.sendErr(c, "unknown_type", "unknown message type: "+clipEcho(env.Type))
	}
}

func (m *Manager) sendErr(c clientConn, code, msg string) {
	c.send(errorMsg{Type: TypeError, Code: code, Message: msg})
}

// countDrop bumps one aggregated tally. Refusals are counted, never logged per
// event: one line per hostile frame is itself an amplifier, so the numbers go
// out in a single periodic line (logDropsLocked).
func (m *Manager) countDrop(bump func(*dropCounters)) {
	m.mu.Lock()
	bump(&m.drops)
	m.mu.Unlock()
}

// badMessage is the shared answer for a frame the server refuses to act on:
// counted, and answered with one bounded Error. It never quotes the offending
// value back.
func (m *Manager) badMessage(c clientConn, msg string) {
	m.countDrop(func(d *dropCounters) { d.badMessage++ })
	m.sendErr(c, "bad_message", msg)
}

// touch refreshes a connection's presence timestamp (heartbeat liveness).
func (m *Manager) touch(c clientConn) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if loc, ok := m.byConn[c.id()]; ok {
		if lb, ok := m.lobbies[loc.code]; ok {
			if mem, ok := lb.members[loc.seat]; ok {
				mem.lastSeen = m.now()
			}
		}
	}
}

// ---- handlers ---------------------------------------------------------------

func (m *Manager) handleCreate(c clientConn, raw []byte) {
	var msg createLobbyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed CreateLobby")
		return
	}
	// `player` and `name` both leave this server again — the first in the roster
	// and in every relayed Chat frame's attribution, the second to any stranger
	// who browses. Screened here, and REJECTED rather than trimmed: a truncated
	// display name would put a name in the roster that the player never chose.
	if !validateText(msg.Player, kMaxPlayerNameBytes) {
		m.badMessage(c, "player name is too long or contains control characters")
		return
	}
	if !validateText(msg.Name, kMaxLobbyNameBytes) {
		m.badMessage(c, "lobby name is too long or contains control characters")
		return
	}
	if !validateText(msg.BuildHash, kMaxBuildHashBytes) {
		m.badMessage(c, "build_hash is malformed")
		return
	}
	vis := msg.Visibility
	if vis != "public" && vis != "private" {
		vis = "private"
	}
	maxSeats := clampInt(msg.MaxSeats, 2, kMaxSeats)

	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.byConn[c.id()]; ok {
		m.sendErr(c, "already_in_lobby", "this connection already holds a lobby seat")
		return
	}
	// The lobby table is the one map an authenticated-by-nothing caller can
	// grow, so it has an explicit ceiling of its own rather than inheriting the
	// connection cap's.
	if m.cfg.MaxLobbies > 0 && len(m.lobbies) >= m.cfg.MaxLobbies {
		m.drops.lobbyCap++
		m.sendErr(c, "server_full", "the server is at its lobby capacity")
		return
	}
	code, err := m.freshCodeLocked()
	if err != nil {
		m.sendErr(c, "internal", "could not allocate a lobby code")
		return
	}
	id, err := newHandle()
	if err != nil {
		m.sendErr(c, "internal", "handle generation failed")
		return
	}
	token, err := newHandle()
	if err != nil {
		m.sendErr(c, "internal", "handle generation failed")
		return
	}
	lb := &lobby{
		code:       code,
		id:         id,
		name:       msg.Name,
		visibility: vis,
		maxSeats:   maxSeats,
		buildHash:  normalizeBuildHash(msg.BuildHash),
		hostToken:  token,
		hostSeat:   0,
		state:      stateOpen,
		members:    map[int]*member{},
		createdAt:  m.now(),
	}
	lb.members[0] = newMember(c, 0, msg.Player, lb.buildHash, m.now())
	m.lobbies[code] = lb
	m.byConn[c.id()] = &connLoc{code: code, seat: 0}

	c.send(lobbyCreatedMsg{Type: TypeLobbyCreated, Code: code, LobbyID: id, HostToken: token, YourSeat: 0})
	m.log.Info("lobby created", "code", code, "visibility", vis, "max_seats", maxSeats, "host", msg.Player)
}

// handleJoin seats a caller that knows a lobby code.
//
// This is the only unauthenticated way INTO somebody else's lobby, and a code
// is 6 Crockford symbols (32^6 ≈ 1.07e9 — PROTOCOL.md §3). Unmetered, that
// space is searchable: a stranger who never gets seated can retry on one socket
// forever, and one hit puts them inside a private lobby. So a JoinByCode that
// does NOT seat the sender is charged against two budgets — the connection's,
// and the source address's, because the first is reset simply by reconnecting.
// A successful join costs nothing, so a player who mistypes a code or retries a
// full lobby never notices either one.
func (m *Manager) handleJoin(c clientConn, raw []byte) {
	var msg joinByCodeMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed JoinByCode")
		return
	}
	if !validateText(msg.Player, kMaxPlayerNameBytes) {
		m.badMessage(c, "player name is too long or contains control characters")
		return
	}
	if !validateText(msg.BuildHash, kMaxBuildHashBytes) {
		m.badMessage(c, "build_hash is malformed")
		return
	}
	if !validateText(msg.Code, kMaxCodeBytes) {
		m.badMessage(c, "code is malformed")
		return
	}
	code := strings.ToUpper(strings.TrimSpace(msg.Code))

	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.byConn[c.id()]; ok {
		m.sendErr(c, "already_in_lobby", "this connection already holds a lobby seat")
		return
	}
	// Budget checked BEFORE the lookup, spent only if the attempt fails, so the
	// limiter cannot be probed by watching which requests are answered.
	cs := m.connStateLocked(c)
	now := m.now()
	if !cs.joinFail.peek(now) || (cs.srcKey != "" && !m.joinFailIP.peek(cs.srcKey)) {
		m.drops.joinGuess++
		return // silent: an answer here would be a free oracle on the rate limit
	}
	reject := func(reason string) {
		cs.joinFail.spend()
		if cs.srcKey != "" {
			m.joinFailIP.allow(cs.srcKey)
		}
		c.send(joinRejectedMsg{Type: TypeJoinRejected, Reason: reason})
	}

	// A code that is not even the right SHAPE never reaches the lobby map, but
	// it still costs the caller a guess — it is a guess.
	if !isLobbyCode(code) {
		reject(ReasonNotFound)
		return
	}
	lb, ok := m.lobbies[code]
	if !ok {
		reject(ReasonNotFound)
		return
	}
	if lb.state != stateOpen {
		reject(ReasonInProgress)
		return
	}
	if normalizeBuildHash(msg.BuildHash) != lb.buildHash {
		// The loud cross-platform door (ADR-0011): turn a mismatched build away
		// before anyone waits.
		reject(ReasonBuildMismatch)
		return
	}
	seat, ok := lb.freeSeat()
	if !ok {
		reject(ReasonFull)
		return
	}
	lb.members[seat] = newMember(c, seat, msg.Player, lb.buildHash, m.now())
	m.byConn[c.id()] = &connLoc{code: code, seat: seat}

	var hostCand []Candidate
	if h, ok := lb.members[lb.hostSeat]; ok {
		hostCand = h.candidates
	}
	c.send(joinAcceptedMsg{
		Type:           TypeJoinAccepted,
		LobbyID:        lb.id,
		YourSeat:       seat,
		Roster:         m.rosterOfLocked(lb),
		HostCandidates: hostCand,
	})
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("join accepted", "code", code, "seat", seat, "player", msg.Player)
}

// handleListPublic answers the public browser. This is the one request where
// the smallest possible frame buys the largest possible answer from an
// unauthenticated caller, so it is bounded twice: its own (tighter) token
// bucket on the rate, and kMaxPublicListRows on the size of one answer.
func (m *Manager) handleListPublic(c clientConn, raw []byte) {
	var msg listPublicMsg
	_ = json.Unmarshal(raw, &msg) // build_hash filter is optional
	want := normalizeBuildHash(msg.BuildHash)

	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.connStateLocked(c).list.allow(m.now()) {
		m.drops.overRate++
		return // silent, like every other over-rate drop
	}
	out := make([]PublicLobby, 0)
	for _, lb := range m.lobbies {
		if lb.visibility != "public" || lb.state != stateOpen {
			continue
		}
		out = append(out, PublicLobby{
			Code:       lb.code,
			Name:       lb.name,
			Players:    len(lb.members),
			Max:        lb.maxSeats,
			HostRegion: "",
			BuildOK:    want == "" || want == lb.buildHash,
		})
	}
	// Sort first, THEN cut, so the truncation is a stable prefix rather than an
	// arbitrary slice of Go's randomised map order.
	sort.Slice(out, func(i, j int) bool { return out[i].Code < out[j].Code })
	if len(out) > kMaxPublicListRows {
		out = out[:kMaxPublicListRows]
	}
	c.send(publicListMsg{Type: TypePublicList, Lobbies: out})
}

func (m *Manager) handleSetReady(c clientConn, raw []byte) {
	var msg setReadyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed SetReady")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if mem.ready == msg.Ready {
		// A no-op SetReady changes no roster, so it broadcasts nothing. Without
		// this, one frame bought a RosterUpdate to every member — the cheapest
		// fan-out amplifier on the control plane — as fast as a member could
		// repeat itself.
		return
	}
	mem.ready = msg.Ready
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
}

func (m *Manager) handleCandidates(c clientConn, raw []byte) {
	var msg candidatesMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed Candidates")
		return
	}
	// A candidate list is STORED per seat and fanned out to every other member,
	// so an unbounded one is both a memory hold and an amplifier (one frame in,
	// up to nine out). msg.Seat stays ignored — the list always belongs to the
	// SENDER's seat, so no member can publish addresses on another's behalf.
	if !validateCandidates(msg.List) {
		m.badMessage(c, "candidate list is too long or malformed")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if lb.state == stateInProgress || lb.state == stateEvicted {
		return // rendezvous window closed
	}
	if !m.connStateLocked(c).cand.allow(m.now()) {
		m.drops.overRate++
		return // silent, like every other over-rate drop
	}
	mem.candidates = msg.List

	// Fan this seat's list out to every other member, and back-fill the sender
	// with any peer lists already known — so the exchange converges regardless
	// of arrival order (design §1.5).
	for seat, other := range lb.members {
		if seat == mem.seat {
			continue
		}
		other.conn.send(peerCandidatesMsg{Type: TypePeerCandidates, Seat: mem.seat, List: msg.List})
		if len(other.candidates) > 0 {
			c.send(peerCandidatesMsg{Type: TypePeerCandidates, Seat: seat, List: other.candidates})
		}
	}
}

func (m *Manager) handleStart(c clientConn, raw []byte) {
	var msg startMatchReqMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed StartMatch")
		return
	}
	if !validateText(msg.HostToken, kMaxHandleBytes) ||
		!validateText(msg.MatchConfigDigest, kMaxDigestBytes) {
		// match_config_digest is ECHOED into the broadcast every seat receives,
		// so it is screened like any other field that reaches other people.
		m.badMessage(c, "host_token or match_config_digest is malformed")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	// Constant-time on the token. A 128-bit crypto/rand handle is not realistically
	// guessable byte-by-byte over a network either way, but a secret compared with
	// == is a secret compared with an early-exit loop, and there is no reason to
	// leave that as the thing standing between a member and the host's authority.
	if mem.seat != lb.hostSeat || !constantTimeEqual(msg.HostToken, lb.hostToken) {
		m.sendErr(c, "not_host", "only the host with a valid host_token may start")
		return
	}
	if lb.state != stateOpen {
		m.sendErr(c, "already_started", "lobby is not open")
		return
	}
	if len(lb.members) < 2 {
		m.sendErr(c, "not_enough_players", "need at least 2 seats to start")
		return
	}
	for _, mm := range lb.members {
		if !mm.ready {
			m.sendErr(c, "not_all_ready", "every seat must be ready")
			return
		}
		if mm.buildHash != lb.buildHash {
			m.sendErr(c, "build_mismatch", "a seat's build_hash diverged")
			return
		}
	}

	seed, err := newSeed()
	if err != nil {
		m.sendErr(c, "internal", "seed generation failed")
		return
	}
	inputDelay := 2
	if msg.InputDelay != nil {
		inputDelay = clampInt(*msg.InputDelay, 1, 8)
	}
	cfgDigest := msg.MatchConfigDigest
	if cfgDigest == "" {
		cfgDigest = "0x00000000"
	}
	seats := sortedSeats(lb)

	lb.state = stateLocked
	for seat, mm := range lb.members {
		mm.conn.send(startMatchMsg{
			Type:              TypeStartMatch,
			Seed:              seed,
			SeatAssign:        seats,
			MatchConfigDigest: cfgDigest,
			InputDelay:        inputDelay,
			Topology:          Topology{HubSeat: lb.hostSeat},
			LocalSeatsMask:    1 << uint(seat), // each connection owns exactly its own seat in v1
		})
	}
	m.log.Info("match started", "code", lb.code, "seats", seats, "input_delay", inputDelay)

	// Design §5.2: LOCKED → IN_PROGRESS on "all peers connected | timeout". v1's
	// control plane has no per-peer "connected" signal, so we implement the
	// timeout arm: after a grace window the lobby goes dormant (candidate relay
	// stops, late joins already reject as in_progress). Phase 2's data plane can
	// add the "connected" arm.
	m.scheduleLockedToInProgressLocked(lb)
}

func (m *Manager) handleReanchor(c clientConn, raw []byte) {
	var msg reanchorLobbyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed ReanchorLobby")
		return
	}
	if !validateText(msg.Code, kMaxCodeBytes) || !validateText(msg.RosterDigest, kMaxDigestBytes) {
		m.badMessage(c, "code or roster_digest is malformed")
		return
	}
	code := strings.ToUpper(strings.TrimSpace(msg.Code))

	m.mu.Lock()
	defer m.mu.Unlock()
	lb, ok := m.lobbies[code]
	if !ok {
		m.sendErr(c, "reanchor_rejected", "lobby code not found")
		return
	}
	loc, ok := m.byConn[c.id()]
	if !ok || loc.code != code {
		m.sendErr(c, "reanchor_rejected", "requester is not a member of this lobby")
		return
	}
	// Re-anchoring is HOST RECOVERY (design §8.3), not a host election: it mints
	// a fresh host_token, which invalidates the sitting host's. Without this
	// check any member of any lobby it had joined — a stranger who was handed a
	// public lobby's code, say — could take the host's authority away from a
	// live, connected host with one frame. So it is only available when the host
	// seat is actually vacant, which is the only situation the design describes.
	if _, hostAlive := lb.members[lb.hostSeat]; hostAlive {
		m.sendErr(c, "reanchor_rejected", "the host seat is still occupied")
		return
	}
	// Prove continuity: the promoted hub's roster digest must match the lobby's
	// surviving roster (design §8.3). Only (seat, name) pairs feed the digest.
	if !constantTimeEqual(rosterDigest(m.rosterOfLocked(lb)), msg.RosterDigest) {
		m.sendErr(c, "reanchor_rejected", "roster_digest does not match the surviving roster")
		return
	}
	token, err := newHandle()
	if err != nil {
		m.sendErr(c, "internal", "handle generation failed")
		return
	}
	lb.hostToken = token
	lb.hostSeat = loc.seat // promote the re-anchoring peer to hub/anchor
	c.send(reanchorAcceptedMsg{Type: TypeReanchorAccepted, LobbyID: lb.id, Code: lb.code, HostToken: token})
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("lobby re-anchored", "code", code, "new_host_seat", loc.seat)
}

func (m *Manager) handleMatchOver(c clientConn, raw []byte) {
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, _ := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if lb.state == stateEvicted {
		return
	}
	// §5.2: match over → OPEN (rematch), roster kept, ready flags cleared.
	lb.state = stateOpen
	for _, mm := range lb.members {
		mm.ready = false
	}
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("lobby reopened for rematch", "code", lb.code)
}

// handleAllocateRelay reserves this seat's slot on the UDP forwarder (§6,
// relay.go) — the fallback a client asks for once its hole-punch has failed.
func (m *Manager) handleAllocateRelay(c clientConn, raw []byte) {
	var msg allocateRelayMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed AllocateRelay")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	// An allocation always belongs to the sender's OWN seat, so a peer cannot
	// mint or steal another seat's handle. The seat field is therefore only ever
	// a cross-check; disagreement is a client bug, not something to honour.
	if msg.Seat != nil && *msg.Seat != mem.seat {
		m.drops.badMessage++
		m.sendErr(c, "bad_message", "seat does not match this connection's seat")
		return
	}
	allocID, err := m.relay.allocate(lb.code, mem.seat)
	if err != nil {
		m.sendErr(c, "internal", "relay allocation failed")
		return
	}
	c.send(relayAllocatedMsg{
		Type:      TypeRelayAllocated,
		RelayAddr: m.cfg.relayAdvertise(),
		AllocID:   allocID,
	})
	m.log.Info("relay allocated", "code", lb.code, "seat", mem.seat)
}

// handleChat relays one typed line to the sender's own lobby (§7).
//
// PORT-ONLY FEATURE: the 1997 game has no chat. It rides this control plane
// because a lobby exists BEFORE the peers punch a direct path, so the WebSocket
// is the only link the players share while they are waiting.
//
// The server stays as dumb here as it is everywhere else — it validates, rate
// limits and forwards; it keeps no history, and it never edits a message.
func (m *Manager) handleChat(c clientConn, raw []byte) {
	var msg chatMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed Chat")
		return
	}
	// Screened BEFORE the lock: rejecting junk needs no lobby state. The
	// rejection is COUNTED rather than logged per line — this path is reachable
	// by a connection holding no seat at all, so a line each would have handed
	// an unauthenticated caller a log amplifier.
	if code := validateChatText(msg.Text); code != "" {
		m.countDrop(func(d *dropCounters) { d.chatInvalid++ })
		m.sendErr(c, code, "chat message rejected")
		return
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		// A connection with no seat has no lobby to speak into. This is THE
		// containment rule for chat: a message only ever reaches the lobby its
		// sender actually sits in.
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if lb.state == stateEvicted {
		return
	}
	if !mem.chat.allow(m.now()) {
		// Dropped whole, never trimmed and never queued, and COUNTED so a flood
		// still shows up in the server's own record — in the periodic aggregate
		// rather than a line each, which would be its own amplifier. No Error
		// goes back either, for the same reason (§7.3).
		m.drops.chatDropped++
		return
	}
	// Seat and name are the SERVER's, read from the roster — the sender's frame
	// cannot claim either. Echoed to the sender too, so every member (including
	// the author) sees one identical, identically-ordered transcript.
	m.broadcastLocked(lb, chatRelayMsg{Type: TypeChat, Seat: mem.seat, Name: mem.name, Text: msg.Text})
}

// ---- lifecycle: disconnect + heartbeat reaper -------------------------------

// removeConn drops a connection's membership and its accounting (called when
// its socket closes). Both maps must be cleared here — leaving connState behind
// would turn the per-connection bookkeeping into the unbounded map it exists to
// avoid.
func (m *Manager) removeConn(c clientConn) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.conns, c.id())
	loc, ok := m.byConn[c.id()]
	if !ok {
		return
	}
	delete(m.byConn, c.id())
	lb, ok := m.lobbies[loc.code]
	if !ok {
		return
	}
	delete(lb.members, loc.seat)
	m.relay.release(loc.code, loc.seat)
	if len(lb.members) == 0 {
		lb.state = stateEvicted
		delete(m.lobbies, lb.code)
		m.relay.releaseLobby(lb.code)
		m.log.Info("lobby evicted (empty)", "code", lb.code)
		return
	}
	// If the host left, hostSeat now dangles; the roster reports no host until a
	// surviving peer re-anchors (design §8). Broadcast the change either way.
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("member left", "code", lb.code, "seat", loc.seat)
}

// runReaper periodically evicts stale members/lobbies (design §5.2).
func (m *Manager) runReaper(ctx context.Context) {
	t := time.NewTicker(m.cfg.HeartbeatInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			m.reap()
		}
	}
}

// reap drops members that missed K heartbeats, closes seatless connections that
// have gone quiet, evicts drained lobbies, and emits the aggregated refusal
// tallies.
func (m *Manager) reap() {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.now()
	m.reapIdleConnsLocked(now)
	m.logDropsLocked()
	deadline := m.cfg.HeartbeatInterval * time.Duration(m.cfg.HeartbeatMiss)
	for code, lb := range m.lobbies {
		dropped := false
		for seat, mem := range lb.members {
			if now.Sub(mem.lastSeen) > deadline {
				delete(m.byConn, mem.conn.id())
				delete(lb.members, seat)
				m.relay.release(code, seat)
				mem.conn.disconnect("heartbeat timeout")
				dropped = true
				m.log.Info("member timed out", "code", code, "seat", seat)
			}
		}
		if len(lb.members) == 0 {
			lb.state = stateEvicted
			delete(m.lobbies, code)
			m.relay.releaseLobby(code)
			m.log.Info("lobby evicted (all timed out)", "code", code)
			continue
		}
		if dropped {
			m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
		}
	}
}

// reapIdleConnsLocked closes sockets that hold no seat and have said nothing
// for cfg.ConnIdleTimeout.
//
// A seated member is already governed by the heartbeat window; a SEATLESS one
// used to be free and immortal, which is what made "open connections and hold
// them" the cheapest way to occupy the server. The C++ client only heartbeats
// while it holds a seat (LobbyFlow::step), but it also reconnects lazily on the
// next action (`if (!client_.is_open()) client_.connect(...)`), so closing an
// idle browser's socket is invisible to it.
func (m *Manager) reapIdleConnsLocked(now time.Time) {
	if m.cfg.ConnIdleTimeout <= 0 {
		return
	}
	for id, cs := range m.conns {
		if !m.seatlessLocked(id) {
			continue
		}
		if now.Sub(cs.lastSeen) > m.cfg.ConnIdleTimeout {
			delete(m.conns, id)
			cs.conn.disconnect("idle")
			m.drops.idleConns++
		}
	}
}

// logDropsLocked emits ONE line covering everything the control plane refused
// since the last tick, and only when a tally actually moved. This is the whole
// point of counting instead of logging: untrusted input must not be able to
// write to the server's log at its own chosen rate.
func (m *Manager) logDropsLocked() {
	cur := m.drops
	if cur == m.lastDrops {
		return
	}
	m.log.Info("control-plane drops",
		"bad_json", cur.badJSON, "bad_message", cur.badMessage,
		"unknown_type", cur.unknownType, "over_rate", cur.overRate,
		"join_guess", cur.joinGuess, "chat_invalid", cur.chatInvalid,
		"chat_dropped", cur.chatDropped, "lobby_cap", cur.lobbyCap,
		"idle_conns", cur.idleConns,
		"lobbies", len(m.lobbies), "conns", len(m.conns))
	m.lastDrops = cur
}

// scheduleLockedToInProgressLocked arms the LOCKED→IN_PROGRESS timeout arm.
// At most ONE timer is outstanding per lobby: a host that cycles
// StartMatch → MatchOver → StartMatch would otherwise leave one runtime timer
// behind per round trip, each holding a closure for the grace window.
func (m *Manager) scheduleLockedToInProgressLocked(lb *lobby) {
	if lb.lockTimerArmed {
		return
	}
	lb.lockTimerArmed = true
	code := lb.code
	time.AfterFunc(m.cfg.LockedGrace, func() {
		m.mu.Lock()
		defer m.mu.Unlock()
		lb, ok := m.lobbies[code]
		if !ok {
			return
		}
		lb.lockTimerArmed = false
		if lb.state == stateLocked {
			lb.state = stateInProgress
			m.log.Info("lobby in progress", "code", code)
		}
	})
}

// ---- locked helpers ---------------------------------------------------------

func (m *Manager) lookupLocked(c clientConn) (*lobby, *member) {
	loc, ok := m.byConn[c.id()]
	if !ok {
		return nil, nil
	}
	lb, ok := m.lobbies[loc.code]
	if !ok {
		return nil, nil
	}
	return lb, lb.members[loc.seat]
}

func (m *Manager) freshCodeLocked() (string, error) {
	for i := 0; i < 32; i++ {
		code, err := newLobbyCode()
		if err != nil {
			return "", err
		}
		if _, exists := m.lobbies[code]; !exists {
			return code, nil
		}
	}
	return "", errors.New("could not find a free lobby code after 32 tries")
}

func (m *Manager) rosterOfLocked(lb *lobby) []RosterEntry {
	seats := sortedSeats(lb)
	out := make([]RosterEntry, 0, len(seats))
	for _, s := range seats {
		mem := lb.members[s]
		out = append(out, RosterEntry{Seat: s, Name: mem.name, Ready: mem.ready, IsHost: s == lb.hostSeat})
	}
	return out
}

func (m *Manager) broadcastLocked(lb *lobby, v any) {
	for _, mem := range lb.members {
		mem.conn.send(v)
	}
}

func sortedSeats(lb *lobby) []int {
	seats := make([]int, 0, len(lb.members))
	for s := range lb.members {
		seats = append(seats, s)
	}
	sort.Ints(seats)
	return seats
}

// constantTimeEqual compares two secrets without an early exit. The length is
// still leaked (subtle.ConstantTimeCompare returns 0 immediately on a length
// mismatch), which is fine — both sides are fixed-width hex handles.
func constantTimeEqual(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}
