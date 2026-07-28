// Package lobby is the control plane's domain core: the lobby state machine,
// every handler behind it, the per-connection accounting, and the heartbeat and
// idle reapers. It is transport-agnostic — it pushes frames through the
// ClientConn seam and never touches a socket — which is what lets the whole
// state machine be tested without a network.
//
// It depends on protocol for the wire types, ratelimit for the budgets, config
// for its caps, and relay for the allocations it mints. Nothing depends on it
// but the WebSocket adapter and main.
package lobby

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"sort"
	"strings"
	"sync"
	"time"

	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/ratelimit"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
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
	// OTHER member out. The list is already size-capped (protocol.MaxCandidates), and
	// this caps the rate, so the total a member can push through the fan-out is
	// bounded in both dimensions. The client publishes its list twice per lobby
	// session (once with the LAN address, once when STUN resolves), so 4/s with
	// 8 banked is orders of magnitude more than it uses.
	kCandCreditMs = 250
	kCandBurst    = 8
)

// ClientConn is the transport-agnostic seam the manager pushes messages through.
// wsapi.conn (production) adapts a WebSocket; fakeConn (tests) captures frames. This
// keeps the whole lobby state machine unit-testable without a network.
type ClientConn interface {
	ID() string     // stable per-connection identity
	Send(v any)     // enqueue one JSON frame (non-blocking; never holds the manager lock)
	Remote() string // peer address, for logging
	Disconnect(reason string)
}

type member struct {
	conn       ClientConn
	seat       int
	name       string
	ready      bool
	buildHash  string // normalized; must equal the lobby's
	lastSeen   time.Time
	candidates []protocol.Candidate

	// Chat token bucket (§7). Starts full so a member can speak the moment it
	// arrives, and never queues or shortens a line — over-rate means DROPPED.
	chat ratelimit.Bucket
}

// newMember seats a connection with a full chat budget.
func newMember(c ClientConn, seat int, name, buildHash string, now time.Time) *member {
	return &member{
		conn:      c,
		seat:      seat,
		name:      name,
		buildHash: buildHash,
		lastSeen:  now,
		chat:      ratelimit.NewBucket(kChatCreditPerMsgMs, kChatBurstMsgs),
	}
}

// connState is the per-CONNECTION accounting the Manager keeps for every open
// socket, seat or no seat. It exists from accept to close, so a connection that
// never takes a seat is still bounded and still reaped (a seatless socket used
// to be free and immortal — see SECURITY.md "idle connections").
type connState struct {
	conn      ClientConn
	srcKey    string // per-IP limiter key, "" when this peer is not capped
	firstSeen time.Time
	lastSeen  time.Time

	msgs     ratelimit.Bucket // every inbound frame
	list     ratelimit.Bucket // ListPublic
	cand     ratelimit.Bucket // Candidates (fans out to every other member)
	joinFail ratelimit.Bucket // JoinByCode attempts that did not seat the sender
}

func newConnState(c ClientConn, now time.Time) *connState {
	return &connState{
		conn:      c,
		firstSeen: now,
		lastSeen:  now,
		msgs:      ratelimit.NewBucket(kMsgCreditMs, kMsgBurst),
		list:      ratelimit.NewBucket(kListCreditMs, kListBurst),
		cand:      ratelimit.NewBucket(kCandCreditMs, kCandBurst),
		joinFail:  ratelimit.NewBucket(kJoinFailCreditMs, kJoinFailBurst),
	}
}

// LobbyCount reports how many lobbies are live. The only window this package
// opens onto its own state — for start-up assertions and tests, never for a
// caller to act on.
func (m *Manager) LobbyCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.lobbies)
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
	byConn  map[string]*connLoc // conn.ID() -> location
	// conns is the per-connection accounting, one entry per open socket whether
	// or not it holds a seat. Bounded by cfg.MaxConns: the WebSocket adapter
	// refuses the upgrade past the cap, and every accepted connection is removed
	// on close.
	conns map[string]*connState
	cfg   config.Config
	log   *zap.Logger
	now   func() time.Time // injectable clock (tests)

	// lastReap is when the reaper goroutine last completed a pass. Read by the
	// `lobby_reaper` liveness probe (app.livenessProbes): if the reaper dies,
	// members and lobbies are never evicted again and the process is degraded in
	// a way only a restart fixes.
	lastReap time.Time

	// joinFailIP charges failed JoinByCode attempts per SOURCE ADDRESS as well
	// as per connection, because the per-connection budget is reset simply by
	// reconnecting and a code search does not care which socket it runs over.
	joinFailIP *ratelimit.Table

	drops     dropCounters // guarded by mu
	lastDrops dropCounters // reaper goroutine only, under mu

	// relay is the UDP forwarder's allocation registry. The Manager mints and
	// frees entries; the relay's own listener reads them on the data plane. It
	// has its own lock and never calls back here, so the only lock order that
	// can occur is Manager.mu → relay.Table.mu. Injected rather than
	// constructed so main can hand the same table to the listener.
	relay *relay.Table
}

func NewManager(cfg config.Config, relayTable *relay.Table, log *zap.Logger) *Manager {
	m := &Manager{
		lobbies:    map[string]*lobby{},
		byConn:     map[string]*connLoc{},
		conns:      map[string]*connState{},
		cfg:        cfg.WithDefaults(),
		log:        log,
		now:        time.Now,
		joinFailIP: ratelimit.NewTable(kJoinFailIPSlots, kJoinFailIPCreditMs, kJoinFailIPBurst),
		relay:      relayTable,
	}
	m.lastReap = m.now()
	return m
}

// ReaperAge reports how long it has been since the reaper last completed a pass.
// The liveness check reads it; a value several heartbeat intervals old means the
// reaper goroutine is gone and eviction has stopped.
func (m *Manager) ReaperAge() time.Duration {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.now().Sub(m.lastReap)
}

// AddConn registers an accepted socket. srcKey is the per-IP limiter key for
// this connection ("" when the peer is not an address a cap should count — see
// ratelimit.SourceKey), remembered here so the UDP-shaped limits and the
// control plane agree on who a caller is.
func (m *Manager) AddConn(c ClientConn, srcKey string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	cs := newConnState(c, m.now())
	cs.srcKey = srcKey
	m.conns[c.ID()] = cs
}

// connStateLocked returns this connection's accounting, creating it if the
// socket was never registered (the in-process test path; production always goes
// through AddConn/RemoveConn).
func (m *Manager) connStateLocked(c ClientConn) *connState {
	if cs, ok := m.conns[c.ID()]; ok {
		return cs
	}
	cs := newConnState(c, m.now())
	cs.srcKey = ratelimit.SourceKey(c.Remote())
	m.conns[c.ID()] = cs
	return cs
}

// Dispatch is the single entry point for an inbound frame.
//
// Everything here runs BEFORE any handler and applies to a caller that has
// proved nothing about itself: the global frame bucket bounds parse cost,
// broadcasts, timers and log volume in one place, and it is checked before the
// JSON is even looked at. Over-rate frames are dropped SILENTLY — answering
// each one with an Error would turn the limiter into the amplifier it exists to
// prevent (the same reasoning as the chat bucket, PROTOCOL.md §7.3).
func (m *Manager) Dispatch(c ClientConn, raw []byte) {
	m.mu.Lock()
	cs := m.connStateLocked(c)
	now := m.now()
	cs.lastSeen = now
	allowed := cs.msgs.Allow(now)
	if !allowed {
		m.drops.overRate++
	}
	m.mu.Unlock()
	if !allowed {
		return
	}

	var env protocol.Envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		m.countDrop(func(d *dropCounters) { d.badJSON++ })
		m.sendErr(c, "bad_json", "message is not valid JSON")
		return
	}
	m.touch(c) // any frame refreshes presence

	switch env.Type {
	case protocol.TypeCreateLobby:
		m.handleCreate(c, raw)
	case protocol.TypeJoinByCode:
		m.handleJoin(c, raw)
	case protocol.TypeListPublic:
		m.handleListPublic(c, raw)
	case protocol.TypeSetReady:
		m.handleSetReady(c, raw)
	case protocol.TypeHeartbeat:
		c.Send(protocol.HeartbeatAckMsg{Type: protocol.TypeHeartbeatAck})
	case protocol.TypeCandidates:
		m.handleCandidates(c, raw)
	case protocol.TypeStartMatch:
		m.handleStart(c, raw)
	case protocol.TypeReanchorLobby:
		m.handleReanchor(c, raw)
	case protocol.TypeMatchOver:
		m.handleMatchOver(c, raw)
	case protocol.TypeAllocateRelay:
		m.handleAllocateRelay(c, raw)
	case protocol.TypeChat:
		m.handleChat(c, raw)
	default:
		m.countDrop(func(d *dropCounters) { d.unknownType++ })
		m.sendErr(c, "unknown_type", "unknown message type: "+protocol.ClipEcho(env.Type))
	}
}

func (m *Manager) sendErr(c ClientConn, code, msg string) {
	c.Send(protocol.ErrorMsg{Type: protocol.TypeError, Code: code, Message: msg})
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
func (m *Manager) badMessage(c ClientConn, msg string) {
	m.countDrop(func(d *dropCounters) { d.badMessage++ })
	m.sendErr(c, "bad_message", msg)
}

// touch refreshes a connection's presence timestamp (heartbeat liveness).
func (m *Manager) touch(c ClientConn) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if loc, ok := m.byConn[c.ID()]; ok {
		if lb, ok := m.lobbies[loc.code]; ok {
			if mem, ok := lb.members[loc.seat]; ok {
				mem.lastSeen = m.now()
			}
		}
	}
}

// ---- handlers ---------------------------------------------------------------

func (m *Manager) handleCreate(c ClientConn, raw []byte) {
	var msg protocol.CreateLobbyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed CreateLobby")
		return
	}
	// `player` and `name` both leave this server again — the first in the roster
	// and in every relayed Chat frame's attribution, the second to any stranger
	// who browses. Screened here, and REJECTED rather than trimmed: a truncated
	// display name would put a name in the roster that the player never chose.
	if !protocol.ValidateText(msg.Player, protocol.MaxPlayerNameBytes) {
		m.badMessage(c, "player name is too long or contains control characters")
		return
	}
	if !protocol.ValidateText(msg.Name, protocol.MaxLobbyNameBytes) {
		m.badMessage(c, "lobby name is too long or contains control characters")
		return
	}
	if !protocol.ValidateText(msg.BuildHash, protocol.MaxBuildHashBytes) {
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
	if _, ok := m.byConn[c.ID()]; ok {
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
	id, err := NewHandle()
	if err != nil {
		m.sendErr(c, "internal", "handle generation failed")
		return
	}
	token, err := NewHandle()
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
		buildHash:  protocol.NormalizeBuildHash(msg.BuildHash),
		hostToken:  token,
		hostSeat:   0,
		state:      stateOpen,
		members:    map[int]*member{},
		createdAt:  m.now(),
	}
	lb.members[0] = newMember(c, 0, msg.Player, lb.buildHash, m.now())
	m.lobbies[code] = lb
	m.byConn[c.ID()] = &connLoc{code: code, seat: 0}

	c.Send(protocol.LobbyCreatedMsg{Type: protocol.TypeLobbyCreated, Code: code, LobbyID: id, HostToken: token, YourSeat: 0})
	m.log.Info("lobby created",
		zap.String("code", code), zap.String("visibility", vis),
		zap.Int("max_seats", maxSeats), zap.String("host", msg.Player))
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
func (m *Manager) handleJoin(c ClientConn, raw []byte) {
	var msg protocol.JoinByCodeMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed JoinByCode")
		return
	}
	if !protocol.ValidateText(msg.Player, protocol.MaxPlayerNameBytes) {
		m.badMessage(c, "player name is too long or contains control characters")
		return
	}
	if !protocol.ValidateText(msg.BuildHash, protocol.MaxBuildHashBytes) {
		m.badMessage(c, "build_hash is malformed")
		return
	}
	if !protocol.ValidateText(msg.Code, protocol.MaxCodeBytes) {
		m.badMessage(c, "code is malformed")
		return
	}
	code := strings.ToUpper(strings.TrimSpace(msg.Code))

	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.byConn[c.ID()]; ok {
		m.sendErr(c, "already_in_lobby", "this connection already holds a lobby seat")
		return
	}
	// Budget checked BEFORE the lookup, spent only if the attempt fails, so the
	// limiter cannot be probed by watching which requests are answered.
	cs := m.connStateLocked(c)
	now := m.now()
	if !cs.joinFail.Peek(now) || (cs.srcKey != "" && !m.joinFailIP.Peek(cs.srcKey)) {
		m.drops.joinGuess++
		return // silent: an answer here would be a free oracle on the rate limit
	}
	reject := func(reason string) {
		cs.joinFail.Spend()
		if cs.srcKey != "" {
			m.joinFailIP.Allow(cs.srcKey)
		}
		c.Send(protocol.JoinRejectedMsg{Type: protocol.TypeJoinRejected, Reason: reason})
	}

	// A code that is not even the right SHAPE never reaches the lobby map, but
	// it still costs the caller a guess — it is a guess.
	if !isLobbyCode(code) {
		reject(protocol.ReasonNotFound)
		return
	}
	lb, ok := m.lobbies[code]
	if !ok {
		reject(protocol.ReasonNotFound)
		return
	}
	if !lb.state.acceptsJoins() {
		reject(protocol.ReasonInProgress)
		return
	}
	if protocol.NormalizeBuildHash(msg.BuildHash) != lb.buildHash {
		// The loud cross-platform door (ADR-0011): turn a mismatched build away
		// before anyone waits.
		reject(protocol.ReasonBuildMismatch)
		return
	}
	seat, ok := lb.freeSeat()
	if !ok {
		reject(protocol.ReasonFull)
		return
	}
	lb.members[seat] = newMember(c, seat, msg.Player, lb.buildHash, m.now())
	m.byConn[c.ID()] = &connLoc{code: code, seat: seat}

	var hostCand []protocol.Candidate
	if h, ok := lb.members[lb.hostSeat]; ok {
		hostCand = h.candidates
	}
	c.Send(protocol.JoinAcceptedMsg{
		Type:           protocol.TypeJoinAccepted,
		LobbyID:        lb.id,
		YourSeat:       seat,
		Roster:         m.rosterOfLocked(lb),
		HostCandidates: hostCand,
	})
	m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("join accepted",
		zap.String("code", code), zap.Int("seat", seat), zap.String("player", msg.Player))
}

// handleListPublic answers the public browser. This is the one request where
// the smallest possible frame buys the largest possible answer from an
// unauthenticated caller, so it is bounded twice: its own (tighter) token
// bucket on the rate, and protocol.MaxPublicListRows on the size of one answer.
func (m *Manager) handleListPublic(c ClientConn, raw []byte) {
	var msg protocol.ListPublicMsg
	_ = json.Unmarshal(raw, &msg) // build_hash filter is optional
	want := protocol.NormalizeBuildHash(msg.BuildHash)

	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.connStateLocked(c).list.Allow(m.now()) {
		m.drops.overRate++
		return // silent, like every other over-rate drop
	}
	out := make([]protocol.PublicLobby, 0)
	for _, lb := range m.lobbies {
		// A row a browser cannot act on is noise, so listing follows the same
		// predicate that decides whether a join would be accepted.
		if lb.visibility != "public" || !lb.state.acceptsJoins() {
			continue
		}
		out = append(out, protocol.PublicLobby{
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
	if len(out) > protocol.MaxPublicListRows {
		out = out[:protocol.MaxPublicListRows]
	}
	c.Send(protocol.PublicListMsg{Type: protocol.TypePublicList, Lobbies: out})
}

func (m *Manager) handleSetReady(c ClientConn, raw []byte) {
	var msg protocol.SetReadyMsg
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
	m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
}

func (m *Manager) handleCandidates(c ClientConn, raw []byte) {
	var msg protocol.CandidatesMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed Candidates")
		return
	}
	// A candidate list is STORED per seat and fanned out to every other member,
	// so an unbounded one is both a memory hold and an amplifier (one frame in,
	// up to nine out). msg.Seat stays ignored — the list always belongs to the
	// SENDER's seat, so no member can publish addresses on another's behalf.
	if !protocol.ValidateCandidates(msg.List) {
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
	if !lb.state.relaysCandidates() {
		return // rendezvous window closed
	}
	if !m.connStateLocked(c).cand.Allow(m.now()) {
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
		other.conn.Send(protocol.PeerCandidatesMsg{Type: protocol.TypePeerCandidates, Seat: mem.seat, List: msg.List})
		if len(other.candidates) > 0 {
			c.Send(protocol.PeerCandidatesMsg{Type: protocol.TypePeerCandidates, Seat: seat, List: other.candidates})
		}
	}
}

func (m *Manager) handleStart(c ClientConn, raw []byte) {
	var msg protocol.StartMatchReqMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed StartMatch")
		return
	}
	if !protocol.ValidateText(msg.HostToken, protocol.MaxHandleBytes) ||
		!protocol.ValidateText(msg.MatchConfigDigest, protocol.MaxDigestBytes) {
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
	if !lb.state.canStart() {
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
		mm.conn.Send(protocol.StartMatchMsg{
			Type:              protocol.TypeStartMatch,
			Seed:              seed,
			SeatAssign:        seats,
			MatchConfigDigest: cfgDigest,
			InputDelay:        inputDelay,
			Topology:          protocol.Topology{HubSeat: lb.hostSeat},
			LocalSeatsMask:    1 << uint(seat), // each connection owns exactly its own seat in v1
		})
	}
	m.log.Info("match started",
		zap.String("code", lb.code), zap.Ints("seats", seats), zap.Int("input_delay", inputDelay))

	// Design §5.2: LOCKED → IN_PROGRESS on "all peers connected | timeout". v1's
	// control plane has no per-peer "connected" signal, so we implement the
	// timeout arm: after a grace window the lobby goes dormant (candidate relay
	// stops, late joins already reject as in_progress). Phase 2's data plane can
	// add the "connected" arm.
	m.scheduleLockedToInProgressLocked(lb)
}

func (m *Manager) handleReanchor(c ClientConn, raw []byte) {
	var msg protocol.ReanchorLobbyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed ReanchorLobby")
		return
	}
	if !protocol.ValidateText(msg.Code, protocol.MaxCodeBytes) || !protocol.ValidateText(msg.RosterDigest, protocol.MaxDigestBytes) {
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
	loc, ok := m.byConn[c.ID()]
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
	if !constantTimeEqual(protocol.RosterDigest(m.rosterOfLocked(lb)), msg.RosterDigest) {
		m.sendErr(c, "reanchor_rejected", "roster_digest does not match the surviving roster")
		return
	}
	token, err := NewHandle()
	if err != nil {
		m.sendErr(c, "internal", "handle generation failed")
		return
	}
	lb.hostToken = token
	lb.hostSeat = loc.seat // promote the re-anchoring peer to hub/anchor
	c.Send(protocol.ReanchorAcceptedMsg{Type: protocol.TypeReanchorAccepted, LobbyID: lb.id, Code: lb.code, HostToken: token})
	m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("lobby re-anchored", zap.String("code", code), zap.Int("new_host_seat", loc.seat))
}

func (m *Manager) handleMatchOver(c ClientConn, raw []byte) {
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, _ := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if !lb.state.isLive() {
		return
	}
	// §5.2: match over → OPEN (rematch), roster kept, ready flags cleared.
	lb.state = stateOpen
	for _, mm := range lb.members {
		mm.ready = false
	}
	m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("lobby reopened for rematch", zap.String("code", lb.code))
}

// handleAllocateRelay reserves this seat's slot on the UDP forwarder (§6,
// the relay package) — the fallback a client asks for once its hole-punch has failed.
func (m *Manager) handleAllocateRelay(c ClientConn, raw []byte) {
	var msg protocol.AllocateRelayMsg
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
	allocID, err := m.relay.Allocate(lb.code, mem.seat)
	if err != nil {
		// A cost cap (relay.Limits) refuses NEW allocations here; a match
		// already forwarding is untouched, and a seat re-allocating its own
		// existing handle still succeeds.
		//
		// IT TRAVELS AS THE EXISTING `internal` REFUSAL, DELIBERATELY. There is
		// no "relay is full" code to reach for: PROTOCOL.md §6.1 is FROZEN and
		// permits exactly not_in_lobby / bad_message / internal in answer to
		// AllocateRelay, and a deployed client is written against that. Nothing
		// is lost by reusing it, because the client does not branch on the code
		// at all here — LobbyFlow::handle_server_message turns ANY Error
		// arriving in Phase::Relaying into fail("RELAY UNAVAILABLE - CANNOT
		// CONNECT"), which is the correct outcome and the same one an older
		// server without a relay produces. The reason goes in `message`, which
		// §4 defines as diagnostic text. Minting a new code would have been a
		// one-sided change to a frozen contract for zero client-visible gain.
		m.sendErr(c, "internal", relayRefusal(err))
		return
	}
	c.Send(protocol.RelayAllocatedMsg{
		Type:      protocol.TypeRelayAllocated,
		RelayAddr: m.cfg.AdvertisedRelay(),
		AllocID:   allocID,
	})
	m.log.Info("relay allocated", zap.String("code", lb.code), zap.Int("seat", mem.seat))
}

// relayRefusal is the diagnostic text for a refused allocation. It names the
// cap that fired so an operator reading a player's screenshot can tell a cost
// ceiling from a genuine fault, and it says nothing about how much budget is
// left or how full the table is — a refused caller learns THAT it was refused,
// never how close it got.
//
// The relay logs the refusals itself, aggregated; nothing is logged here,
// because AllocateRelay is a frame a client can repeat at its own rate and rule
// 4 of SECURITY.md says untrusted input never sets the log's pace.
func relayRefusal(err error) string {
	switch {
	case errors.Is(err, relay.ErrEgressBudget):
		return "relay egress budget exhausted"
	case errors.Is(err, relay.ErrAllocationLimit):
		return "relay is at its allocation capacity"
	default:
		return "relay allocation failed"
	}
}

// handleChat relays one typed line to the sender's own lobby (§7).
//
// PORT-ONLY FEATURE: the 1997 game has no chat. It rides this control plane
// because a lobby exists BEFORE the peers punch a direct path, so the WebSocket
// is the only link the players share while they are waiting.
//
// The server stays as dumb here as it is everywhere else — it validates, rate
// limits and forwards; it keeps no history, and it never edits a message.
func (m *Manager) handleChat(c ClientConn, raw []byte) {
	var msg protocol.ChatMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.badMessage(c, "malformed Chat")
		return
	}
	// Screened BEFORE the lock: rejecting junk needs no lobby state. The
	// rejection is COUNTED rather than logged per line — this path is reachable
	// by a connection holding no seat at all, so a line each would have handed
	// an unauthenticated caller a log amplifier.
	if code := protocol.ValidateChatText(msg.Text); code != "" {
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
	if !lb.state.isLive() {
		return
	}
	if !mem.chat.Allow(m.now()) {
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
	m.broadcastLocked(lb, protocol.ChatRelayMsg{Type: protocol.TypeChat, Seat: mem.seat, Name: mem.name, Text: msg.Text})
}

// ---- lifecycle: disconnect + heartbeat reaper -------------------------------

// RemoveConn drops a connection's membership and its accounting (called when
// its socket closes). Both maps must be cleared here — leaving connState behind
// would turn the per-connection bookkeeping into the unbounded map it exists to
// avoid.
func (m *Manager) RemoveConn(c ClientConn) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.conns, c.ID())
	loc, ok := m.byConn[c.ID()]
	if !ok {
		return
	}
	delete(m.byConn, c.ID())
	lb, ok := m.lobbies[loc.code]
	if !ok {
		return
	}
	delete(lb.members, loc.seat)
	m.relay.Release(loc.code, loc.seat)
	if len(lb.members) == 0 {
		lb.state = stateEvicted
		delete(m.lobbies, lb.code)
		m.relay.ReleaseLobby(lb.code)
		m.log.Info("lobby evicted (empty)", zap.String("code", lb.code))
		return
	}
	// If the host left, hostSeat now dangles; the roster reports no host until a
	// surviving peer re-anchors (design §8). Broadcast the change either way.
	m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
	m.log.Info("member left", zap.String("code", lb.code), zap.Int("seat", loc.seat))
}

// RunReaper periodically evicts stale members/lobbies (design §5.2).
func (m *Manager) RunReaper(ctx context.Context) {
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
	m.lastReap = now
	m.reapIdleConnsLocked(now)
	m.logDropsLocked()
	deadline := m.cfg.HeartbeatInterval * time.Duration(m.cfg.HeartbeatMiss)
	for code, lb := range m.lobbies {
		dropped := false
		for seat, mem := range lb.members {
			if now.Sub(mem.lastSeen) > deadline {
				delete(m.byConn, mem.conn.ID())
				delete(lb.members, seat)
				m.relay.Release(code, seat)
				mem.conn.Disconnect("heartbeat timeout")
				dropped = true
				m.log.Info("member timed out", zap.String("code", code), zap.Int("seat", seat))
			}
		}
		if len(lb.members) == 0 {
			lb.state = stateEvicted
			delete(m.lobbies, code)
			m.relay.ReleaseLobby(code)
			m.log.Info("lobby evicted (all timed out)", zap.String("code", code))
			continue
		}
		if dropped {
			m.broadcastLocked(lb, protocol.RosterUpdateMsg{Type: protocol.TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
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
			cs.conn.Disconnect("idle")
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
		zap.Uint64("bad_json", cur.badJSON), zap.Uint64("bad_message", cur.badMessage),
		zap.Uint64("unknown_type", cur.unknownType), zap.Uint64("over_rate", cur.overRate),
		zap.Uint64("join_guess", cur.joinGuess), zap.Uint64("chat_invalid", cur.chatInvalid),
		zap.Uint64("chat_dropped", cur.chatDropped), zap.Uint64("lobby_cap", cur.lobbyCap),
		zap.Uint64("idle_conns", cur.idleConns),
		zap.Int("lobbies", len(m.lobbies)), zap.Int("conns", len(m.conns)))
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
			m.log.Info("lobby in progress", zap.String("code", code))
		}
	})
}

// ---- locked helpers ---------------------------------------------------------

func (m *Manager) lookupLocked(c ClientConn) (*lobby, *member) {
	loc, ok := m.byConn[c.ID()]
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

func (m *Manager) rosterOfLocked(lb *lobby) []protocol.RosterEntry {
	seats := sortedSeats(lb)
	out := make([]protocol.RosterEntry, 0, len(seats))
	for _, s := range seats {
		mem := lb.members[s]
		out = append(out, protocol.RosterEntry{Seat: s, Name: mem.name, Ready: mem.ready, IsHost: s == lb.hostSeat})
	}
	return out
}

func (m *Manager) broadcastLocked(lb *lobby, v any) {
	for _, mem := range lb.members {
		mem.conn.Send(v)
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
