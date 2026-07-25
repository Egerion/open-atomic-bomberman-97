package main

import (
	"context"
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
	// arrives; chatSeen is the last refill instant, not the last message.
	chatCreditMs int
	chatSeen     time.Time
}

// newMember seats a connection with a full chat budget.
func newMember(c clientConn, seat int, name, buildHash string, now time.Time) *member {
	return &member{
		conn:         c,
		seat:         seat,
		name:         name,
		buildHash:    buildHash,
		lastSeen:     now,
		chatCreditMs: kChatCreditPerMsgMs * kChatBurstMsgs,
	}
}

// spendChatCredit refills this member's bucket for the elapsed time and takes
// one message out of it. False means the line must be DROPPED (§7): the bucket
// never queues and never shortens a message.
func (mem *member) spendChatCredit(now time.Time) bool {
	if !mem.chatSeen.IsZero() {
		if elapsed := now.Sub(mem.chatSeen); elapsed > 0 {
			mem.chatCreditMs += int(elapsed / time.Millisecond)
		}
	}
	mem.chatSeen = now
	if max := kChatCreditPerMsgMs * kChatBurstMsgs; mem.chatCreditMs > max {
		mem.chatCreditMs = max
	}
	if mem.chatCreditMs < kChatCreditPerMsgMs {
		return false
	}
	mem.chatCreditMs -= kChatCreditPerMsgMs
	return true
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
	cfg     Config
	log     *slog.Logger
	now     func() time.Time // injectable clock (tests)

	// relay is the UDP forwarder's allocation registry (relay.go). The Manager
	// mints and frees entries; relayServer reads them on the data plane. It has
	// its own lock and never calls back here, so the order is always mu → relay.
	relay *relayTable
}

func NewManager(cfg Config, log *slog.Logger) *Manager {
	return &Manager{
		lobbies: map[string]*lobby{},
		byConn:  map[string]*connLoc{},
		cfg:     cfg,
		log:     log,
		now:     time.Now,
		relay:   newRelayTable(cfg.RelayIdle, log),
	}
}

// dispatch is the single entry point for an inbound frame.
func (m *Manager) dispatch(c clientConn, raw []byte) {
	var env envelope
	if err := json.Unmarshal(raw, &env); err != nil {
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
		m.sendErr(c, "unknown_type", "unknown message type: "+env.Type)
	}
}

func (m *Manager) sendErr(c clientConn, code, msg string) {
	c.send(errorMsg{Type: TypeError, Code: code, Message: msg})
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
		m.sendErr(c, "bad_message", "malformed CreateLobby")
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

func (m *Manager) handleJoin(c clientConn, raw []byte) {
	var msg joinByCodeMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.sendErr(c, "bad_message", "malformed JoinByCode")
		return
	}
	code := strings.ToUpper(strings.TrimSpace(msg.Code))

	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.byConn[c.id()]; ok {
		m.sendErr(c, "already_in_lobby", "this connection already holds a lobby seat")
		return
	}
	lb, ok := m.lobbies[code]
	if !ok {
		c.send(joinRejectedMsg{Type: TypeJoinRejected, Reason: ReasonNotFound})
		return
	}
	if lb.state != stateOpen {
		c.send(joinRejectedMsg{Type: TypeJoinRejected, Reason: ReasonInProgress})
		return
	}
	if normalizeBuildHash(msg.BuildHash) != lb.buildHash {
		// The loud cross-platform door (ADR-0011): turn a mismatched build away
		// before anyone waits.
		c.send(joinRejectedMsg{Type: TypeJoinRejected, Reason: ReasonBuildMismatch})
		return
	}
	seat, ok := lb.freeSeat()
	if !ok {
		c.send(joinRejectedMsg{Type: TypeJoinRejected, Reason: ReasonFull})
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

func (m *Manager) handleListPublic(c clientConn, raw []byte) {
	var msg listPublicMsg
	_ = json.Unmarshal(raw, &msg) // build_hash filter is optional
	want := normalizeBuildHash(msg.BuildHash)

	m.mu.Lock()
	defer m.mu.Unlock()
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
	sort.Slice(out, func(i, j int) bool { return out[i].Code < out[j].Code })
	c.send(publicListMsg{Type: TypePublicList, Lobbies: out})
}

func (m *Manager) handleSetReady(c clientConn, raw []byte) {
	var msg setReadyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.sendErr(c, "bad_message", "malformed SetReady")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	mem.ready = msg.Ready
	m.broadcastLocked(lb, rosterUpdateMsg{Type: TypeRosterUpdate, Roster: m.rosterOfLocked(lb)})
}

func (m *Manager) handleCandidates(c clientConn, raw []byte) {
	var msg candidatesMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.sendErr(c, "bad_message", "malformed Candidates")
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
		m.sendErr(c, "bad_message", "malformed StartMatch")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	lb, mem := m.lookupLocked(c)
	if lb == nil {
		m.sendErr(c, "not_in_lobby", "no lobby seat for this connection")
		return
	}
	if mem.seat != lb.hostSeat || msg.HostToken != lb.hostToken {
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
	m.scheduleLockedToInProgress(lb.code)
}

func (m *Manager) handleReanchor(c clientConn, raw []byte) {
	var msg reanchorLobbyMsg
	if err := json.Unmarshal(raw, &msg); err != nil {
		m.sendErr(c, "bad_message", "malformed ReanchorLobby")
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
	// Prove continuity: the promoted hub's roster digest must match the lobby's
	// surviving roster (design §8.3). Only (seat, name) pairs feed the digest.
	if rosterDigest(m.rosterOfLocked(lb)) != msg.RosterDigest {
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
		m.sendErr(c, "bad_message", "malformed AllocateRelay")
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
		m.sendErr(c, "bad_message", "malformed Chat")
		return
	}
	// Screened BEFORE the lock: rejecting junk needs no lobby state.
	if code := validateChatText(msg.Text); code != "" {
		m.sendErr(c, code, "chat message rejected")
		m.log.Warn("chat rejected", "reason", code, "bytes", len(msg.Text), "remote", c.remote())
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
	if !mem.spendChatCredit(m.now()) {
		// Dropped whole, never trimmed and never queued — and LOGGED, so a
		// flood shows up in the server's own record instead of vanishing. No
		// Error goes back: answering every dropped line would just amplify the
		// flood it is meant to damp.
		m.log.Warn("chat dropped (rate limit)", "code", lb.code, "seat", mem.seat, "remote", c.remote())
		return
	}
	// Seat and name are the SERVER's, read from the roster — the sender's frame
	// cannot claim either. Echoed to the sender too, so every member (including
	// the author) sees one identical, identically-ordered transcript.
	m.broadcastLocked(lb, chatRelayMsg{Type: TypeChat, Seat: mem.seat, Name: mem.name, Text: msg.Text})
}

// ---- lifecycle: disconnect + heartbeat reaper -------------------------------

// removeConn drops a connection's membership (called when its socket closes).
func (m *Manager) removeConn(c clientConn) {
	m.mu.Lock()
	defer m.mu.Unlock()
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

// reap drops members that missed K heartbeats and evicts drained lobbies.
func (m *Manager) reap() {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.now()
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

// scheduleLockedToInProgress arms the LOCKED→IN_PROGRESS timeout arm.
func (m *Manager) scheduleLockedToInProgress(code string) {
	grace := m.cfg.LockedGrace
	time.AfterFunc(grace, func() {
		m.mu.Lock()
		defer m.mu.Unlock()
		if lb, ok := m.lobbies[code]; ok && lb.state == stateLocked {
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

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}
