package main

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// Control-plane hardening regressions (SECURITY.md). Every limit and every
// refusal introduced there has a case here.

// ---- unbounded growth --------------------------------------------------------

func TestLobbyCapRefusesFurtherCreates(t *testing.T) {
	m := newTestManager(t)
	m.cfg.MaxLobbies = 3
	for i := 0; i < 3; i++ {
		c := newFakeConn("c" + itoa(i))
		createLobby(t, m, c, nil)
	}
	over := newFakeConn("over")
	dispatchMap(m, over, map[string]any{
		"type": TypeCreateLobby, "max_seats": 2, "build_hash": "0xA1B2C3D4", "player": "Mallory",
	})
	if over.has(TypeLobbyCreated) {
		t.Fatal("a lobby past the cap must not be created")
	}
	if got := lastTyped[errorMsg](t, over, TypeError); got.Code != "server_full" {
		t.Fatalf("expected server_full, got %q", got.Code)
	}
	if m.drops.lobbyCap != 1 {
		t.Fatalf("the refusal must be counted, got %d", m.drops.lobbyCap)
	}
}

func TestConnStateIsRemovedWithTheConnection(t *testing.T) {
	// The per-connection accounting is only bounded because it is deleted on
	// close — including for a socket that never took a seat.
	m := newTestManager(t)
	seatless := newFakeConn("seatless")
	dispatchMap(m, seatless, map[string]any{"type": TypeHeartbeat})
	if len(m.conns) != 1 {
		t.Fatalf("expected one tracked connection, got %d", len(m.conns))
	}
	m.removeConn(seatless)
	if len(m.conns) != 0 {
		t.Fatalf("connState leaked after close: %d entries", len(m.conns))
	}

	seated := newFakeConn("seated")
	createLobby(t, m, seated, nil)
	m.removeConn(seated)
	if len(m.conns) != 0 || len(m.byConn) != 0 || len(m.lobbies) != 0 {
		t.Fatalf("state leaked: conns=%d byConn=%d lobbies=%d",
			len(m.conns), len(m.byConn), len(m.lobbies))
	}
}

func TestSeatlessConnectionsAreReapedWhenIdle(t *testing.T) {
	m := newTestManager(t)
	clk := time.Now()
	m.now = func() time.Time { return clk }
	m.cfg.ConnIdleTimeout = 30 * time.Second
	m.cfg.HeartbeatInterval = time.Hour // isolate the idle path from the heartbeat one

	idle := newFakeConn("idle")
	dispatchMap(m, idle, map[string]any{"type": TypeHeartbeat})
	seated := newFakeConn("seated")
	createLobby(t, m, seated, nil)

	clk = clk.Add(31 * time.Second)
	m.reap()

	if !idle.isClosed() {
		t.Fatal("a seatless, silent connection must be closed")
	}
	if seated.isClosed() {
		t.Fatal("a seated member is governed by the heartbeat window, not the idle one")
	}
	if len(m.conns) != 1 {
		t.Fatalf("the reaped connection must be forgotten, %d entries left", len(m.conns))
	}
}

func TestIdleReaperCanBeDisabled(t *testing.T) {
	m := newTestManager(t)
	clk := time.Now()
	m.now = func() time.Time { return clk }
	m.cfg.ConnIdleTimeout = -1

	idle := newFakeConn("idle")
	dispatchMap(m, idle, map[string]any{"type": TypeHeartbeat})
	clk = clk.Add(time.Hour)
	m.reap()
	if idle.isClosed() {
		t.Fatal("a negative timeout must disable the idle reaper")
	}
}

// ---- rate limits -------------------------------------------------------------

func TestGlobalFrameBucketDropsAFloodSilently(t *testing.T) {
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }
	c := newFakeConn("flood")

	for i := 0; i < kMsgBurst; i++ {
		dispatchMap(m, c, map[string]any{"type": TypeHeartbeat})
	}
	if got := c.countOfType(TypeHeartbeatAck); got != kMsgBurst {
		t.Fatalf("the burst allowance should pass: %d acks, want %d", got, kMsgBurst)
	}
	c.reset()
	for i := 0; i < 50; i++ {
		dispatchMap(m, c, map[string]any{"type": TypeHeartbeat})
	}
	if c.countOfType(TypeHeartbeatAck) != 0 {
		t.Fatal("over-rate frames must not be served")
	}
	if c.has(TypeError) {
		t.Fatal("an over-rate frame must be dropped silently — an Error each would amplify the flood")
	}
	if m.drops.overRate == 0 {
		t.Fatal("over-rate drops must be counted")
	}

	now = now.Add(time.Duration(kMsgCreditMs) * time.Millisecond)
	c.reset()
	dispatchMap(m, c, map[string]any{"type": TypeHeartbeat})
	if c.countOfType(TypeHeartbeatAck) != 1 {
		t.Fatal("the bucket must refill over time")
	}
}

func TestListPublicHasItsOwnTighterBucket(t *testing.T) {
	// ListPublic is the request where the smallest frame buys the largest answer
	// from an unauthenticated caller, so it is capped below the global rate.
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }
	c := newFakeConn("browser")

	for i := 0; i < kListBurst; i++ {
		dispatchMap(m, c, map[string]any{"type": TypeListPublic})
	}
	if got := c.countOfType(TypePublicList); got != kListBurst {
		t.Fatalf("the browse burst should pass: %d answers, want %d", got, kListBurst)
	}
	c.reset()
	dispatchMap(m, c, map[string]any{"type": TypeListPublic})
	if c.has(TypePublicList) {
		t.Fatal("an over-rate browse must not be answered")
	}
	if kListCreditMs <= kMsgCreditMs {
		t.Fatal("the ListPublic bucket must be tighter than the global one")
	}
}

func TestPublicListIsRowCapped(t *testing.T) {
	m := newTestManager(t)
	for i := 0; i < kMaxPublicListRows+25; i++ {
		c := newFakeConn("h" + itoa(i))
		createLobby(t, m, c, map[string]any{"visibility": "public"})
	}
	browser := newFakeConn("browser")
	dispatchMap(m, browser, map[string]any{"type": TypeListPublic})
	got := lastTyped[publicListMsg](t, browser, TypePublicList)
	if len(got.Lobbies) != kMaxPublicListRows {
		t.Fatalf("one answer must be row-capped: %d rows, want %d", len(got.Lobbies), kMaxPublicListRows)
	}
	// Sorted-then-cut, so the prefix is stable rather than map-order arbitrary.
	for i := 1; i < len(got.Lobbies); i++ {
		if got.Lobbies[i-1].Code >= got.Lobbies[i].Code {
			t.Fatal("rows must be sorted by code so the truncation is deterministic")
		}
	}
}

// ---- lobby-code guessing -----------------------------------------------------

func TestFailedJoinsAreBudgetedThenSilent(t *testing.T) {
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }
	guesser := newFakeConn("guesser")
	guesser.addr = "203.0.113.66:40000" // a real client address, so the IP cap applies too

	for i := 0; i < kJoinFailBurst; i++ {
		dispatchMap(m, guesser, map[string]any{
			"type": TypeJoinByCode, "code": "ZZZZZ" + string(rune('0'+i)),
			"build_hash": "0xA1B2C3D4", "player": "Mallory",
		})
	}
	if got := guesser.countOfType(TypeJoinRejected); got != kJoinFailBurst {
		t.Fatalf("the guess burst should be answered: %d rejections, want %d", got, kJoinFailBurst)
	}
	guesser.reset()
	for i := 0; i < 20; i++ {
		dispatchMap(m, guesser, map[string]any{
			"type": TypeJoinByCode, "code": "YYYYY0",
			"build_hash": "0xA1B2C3D4", "player": "Mallory",
		})
	}
	if guesser.has(TypeJoinRejected) || guesser.has(TypeError) {
		t.Fatal("a guess past the budget must get no answer at all — any reply is an oracle")
	}
	if m.drops.joinGuess == 0 {
		t.Fatal("throttled guesses must be counted")
	}
}

func TestSuccessfulJoinCostsNoGuessBudget(t *testing.T) {
	// A player who joins normally must never meet the anti-guessing limit, even
	// after several joins in a row from one address.
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 10})

	for i := 0; i < kJoinFailBurst+5; i++ {
		g := join(m, lc.Code, "P"+itoa(i), "0xA1B2C3D4", "g"+itoa(i))
		if !g.has(TypeJoinAccepted) {
			t.Fatalf("join %d was refused; a successful join must not spend guess credit", i)
		}
		m.removeConn(g)
	}
}

func TestMalformedCodesAreCountedAsGuesses(t *testing.T) {
	m := newTestManager(t)
	c := newFakeConn("guesser")
	// Right length budget, wrong alphabet: 'I','L','O','U' are excluded from
	// Crockford base-32, so this can never be a real code.
	dispatchMap(m, c, map[string]any{
		"type": TypeJoinByCode, "code": "IIIIII", "build_hash": "0xA1B2C3D4", "player": "M",
	})
	if got := lastTyped[joinRejectedMsg](t, c, TypeJoinRejected); got.Reason != ReasonNotFound {
		t.Fatalf("a malformed code is not found, got %q", got.Reason)
	}
}

// ---- field validation: reject, never repair ----------------------------------

func TestOversizedAndControlCharacterNamesAreRejected(t *testing.T) {
	cases := []struct {
		name  string
		field string
		value string
	}{
		{"long player", "player", strings.Repeat("A", kMaxPlayerNameBytes+1)},
		{"control rune in player", "player", "Ada\x00\x1b[31m"},
		{"newline in player", "player", "Ada\nEge"},
		{"long lobby name", "name", strings.Repeat("B", kMaxLobbyNameBytes+1)},
		{"control rune in lobby name", "name", "game\x07"},
		{"long build_hash", "build_hash", strings.Repeat("f", kMaxBuildHashBytes+1)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			m := newTestManager(t)
			c := newFakeConn("c")
			msg := map[string]any{
				"type": TypeCreateLobby, "max_seats": 2,
				"build_hash": "0xA1B2C3D4", "player": "Ege", "name": "game",
			}
			msg[tc.field] = tc.value
			dispatchMap(m, c, msg)
			if c.has(TypeLobbyCreated) {
				t.Fatal("the lobby must not be created")
			}
			if got := lastTyped[errorMsg](t, c, TypeError); got.Code != "bad_message" {
				t.Fatalf("want bad_message, got %q", got.Code)
			}
			if len(m.lobbies) != 0 {
				t.Fatal("nothing may be stored from a rejected frame")
			}
		})
	}
}

func TestJoinRejectsAnOversizedPlayerNameBeforeItReachesTheRoster(t *testing.T) {
	// The known gap: `player` went verbatim into the roster and out to every
	// member in the RosterUpdate broadcast.
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	host.reset()

	bad := newFakeConn("bad")
	dispatchMap(m, bad, map[string]any{
		"type": TypeJoinByCode, "code": lc.Code, "build_hash": "0xA1B2C3D4",
		"player": strings.Repeat("X", 1000),
	})
	if bad.has(TypeJoinAccepted) {
		t.Fatal("the join must be refused")
	}
	if host.has(TypeRosterUpdate) {
		t.Fatal("a rejected name must never reach the other members")
	}
}

func TestNamesAtTheLimitStillPass(t *testing.T) {
	// The 1997 node name (39 bytes) and any normal display name must be
	// untouched — a limit that rejects real players is a bug, not a fix.
	m := newTestManager(t)
	c := newFakeConn("c")
	name := strings.Repeat("N", kMaxPlayerNameBytes)
	createLobby(t, m, c, map[string]any{"player": name, "name": "Ege's oyunu — ünlü"})
	lb := m.lobbies[lastTyped[lobbyCreatedMsg](t, c, TypeLobbyCreated).Code]
	if lb.members[0].name != name {
		t.Fatal("a name at exactly the limit must pass through unchanged")
	}
	if lb.name != "Ege's oyunu — ünlü" {
		t.Fatalf("non-ASCII lobby names must pass verbatim, got %q", lb.name)
	}
}

func TestCandidateListIsBoundedInEveryDimension(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	host.reset()

	huge := make([]map[string]any, kMaxCandidates+1)
	for i := range huge {
		huge[i] = map[string]any{"kind": "host", "addr": "192.0.2.1:1"}
	}
	dispatchMap(m, guest, map[string]any{"type": TypeCandidates, "list": huge})
	if host.has(TypePeerCandidates) {
		t.Fatal("an over-long candidate list must not be fanned out")
	}
	if got := lastTyped[errorMsg](t, guest, TypeError); got.Code != "bad_message" {
		t.Fatalf("want bad_message, got %q", got.Code)
	}

	guest.reset()
	dispatchMap(m, guest, map[string]any{"type": TypeCandidates, "list": []map[string]any{
		{"kind": "host", "addr": strings.Repeat("a", kMaxCandidateAddrBytes+1)},
	}})
	if host.has(TypePeerCandidates) {
		t.Fatal("an over-long candidate address must not be fanned out")
	}

	// The real thing still works.
	guest.reset()
	host.reset()
	dispatchMap(m, guest, map[string]any{"type": TypeCandidates, "list": []map[string]any{
		{"kind": "host", "addr": "192.168.1.9:41234"},
		{"kind": "reflexive", "addr": "81.2.3.4:52001"},
	}})
	if !host.has(TypePeerCandidates) {
		t.Fatal("a normal candidate list must still be relayed")
	}
}

// ---- parse limits ------------------------------------------------------------

func TestDeeplyNestedJSONIsRejectedNotFatal(t *testing.T) {
	m := newTestManager(t)
	c := newFakeConn("c")
	// Just under the WebSocket read limit, so this is exactly what a hostile
	// client can actually deliver.
	depth := wsReadLimit / 2
	raw := []byte(strings.Repeat("[", depth) + strings.Repeat("]", depth))
	m.dispatch(c, raw)
	if got := lastTyped[errorMsg](t, c, TypeError); got.Code != "bad_json" {
		t.Fatalf("want bad_json, got %q", got.Code)
	}
	if m.drops.badJSON != 1 {
		t.Fatalf("the refusal must be counted, got %d", m.drops.badJSON)
	}
}

func TestUnknownTypeEchoIsBounded(t *testing.T) {
	m := newTestManager(t)
	c := newFakeConn("c")
	dispatchMap(m, c, map[string]any{"type": strings.Repeat("Z", 4000)})
	got := lastTyped[errorMsg](t, c, TypeError)
	if got.Code != "unknown_type" {
		t.Fatalf("want unknown_type, got %q", got.Code)
	}
	if len(got.Message) > 128 {
		t.Fatalf("an Error must not reflect the caller's string back at size: %d bytes", len(got.Message))
	}

	c.reset()
	dispatchMap(m, c, map[string]any{"type": "Weird\x1b[2J"})
	if strings.ContainsRune(lastTyped[errorMsg](t, c, TypeError).Message, '\x1b') {
		t.Fatal("an Error must not echo control characters back")
	}

	// A short, printable unknown type still echoes — that is its diagnostic value.
	c.reset()
	dispatchMap(m, c, map[string]any{"type": "FutureMessage"})
	if !strings.Contains(lastTyped[errorMsg](t, c, TypeError).Message, "FutureMessage") {
		t.Fatal("a well-formed unknown type should still be named in the Error")
	}
}

// ---- authorisation -----------------------------------------------------------

func TestReanchorIsRefusedWhileTheHostIsAlive(t *testing.T) {
	// Re-anchoring mints a FRESH host_token, which invalidates the sitting
	// host's. Anyone who was handed a lobby code could otherwise join and take
	// the host's authority away with one frame.
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 4})
	guest := join(m, lc.Code, "Mallory", "0xA1B2C3D4", "guest")

	digest := rosterDigest(m.rosterOfLocked(m.lobbies[lc.Code]))
	dispatchMap(m, guest, map[string]any{
		"type": TypeReanchorLobby, "code": lc.Code, "roster_digest": digest,
	})
	if guest.has(TypeReanchorAccepted) {
		t.Fatal("a member must not be able to depose a live host")
	}
	if got := lastTyped[errorMsg](t, guest, TypeError); got.Code != "reanchor_rejected" {
		t.Fatalf("want reanchor_rejected, got %q", got.Code)
	}
	if m.lobbies[lc.Code].hostToken != lc.HostToken {
		t.Fatal("the sitting host's token must survive a refused re-anchor")
	}
	if m.lobbies[lc.Code].hostSeat != 0 {
		t.Fatal("the host seat must not move")
	}
}

func TestReanchorWorksOnceTheHostIsGone(t *testing.T) {
	// The case the feature exists for (design §8.3) must keep working.
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 4})
	survivor := join(m, lc.Code, "Ada", "0xA1B2C3D4", "survivor")

	m.removeConn(host) // the host drops
	survivor.reset()

	digest := rosterDigest(m.rosterOfLocked(m.lobbies[lc.Code]))
	dispatchMap(m, survivor, map[string]any{
		"type": TypeReanchorLobby, "code": lc.Code, "roster_digest": digest,
	})
	ra := lastTyped[reanchorAcceptedMsg](t, survivor, TypeReanchorAccepted)
	if ra.HostToken == lc.HostToken || ra.HostToken == "" {
		t.Fatal("a promoted hub must get a fresh host_token")
	}
	if m.lobbies[lc.Code].hostSeat != 1 {
		t.Fatalf("the survivor should be the new host seat, got %d", m.lobbies[lc.Code].hostSeat)
	}
}

func TestStartMatchNeedsTheHostSeatAndTheToken(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)

	// A member with the RIGHT token but the wrong seat.
	guest.reset()
	dispatchMap(m, guest, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	if guest.has(TypeStartMatch) {
		t.Fatal("a non-host seat must not start the match even holding the token")
	}
	if got := lastTyped[errorMsg](t, guest, TypeError); got.Code != "not_host" {
		t.Fatalf("want not_host, got %q", got.Code)
	}

	// The host seat with the WRONG token.
	host.reset()
	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID,
		"host_token": strings.Repeat("0", len(lc.HostToken)),
	})
	if host.has(TypeStartMatch) {
		t.Fatal("the host seat must still present a valid token")
	}
}

func TestMatchConfigDigestIsScreenedBeforeItIsBroadcast(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)
	guest.reset()

	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
		"match_config_digest": strings.Repeat("d", kMaxDigestBytes+1),
	})
	if guest.has(TypeStartMatch) {
		t.Fatal("an unbounded echoed field must not reach the other seats")
	}
}

// ---- WebSocket admission -----------------------------------------------------

func TestWebSocketConnectionCapRefusesBeforeTheUpgrade(t *testing.T) {
	cfg := Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour, MaxConns: 2}
	mgr := NewManager(cfg, newLogger("error"))
	ws := newWSServer(mgr, cfg, newLogger("error"))
	srv := httptest.NewServer(ws.handler())
	defer srv.Close()

	url := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws"
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	a := wsDial(t, ctx, url)
	defer a.Close(websocket.StatusNormalClosure, "")
	b := wsDial(t, ctx, url)
	defer b.Close(websocket.StatusNormalClosure, "")

	_, resp, err := websocket.Dial(ctx, url, nil)
	if err == nil {
		t.Fatal("a third connection past the cap must be refused")
	}
	if resp == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("expected a 503 before the upgrade, got %v", resp)
	}
	if ws.refused.Load() != 1 {
		t.Fatalf("the refusal must be counted, got %d", ws.refused.Load())
	}

	// A slot freed by a close is reusable — the cap is concurrency, not a quota.
	a.Close(websocket.StatusNormalClosure, "")
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if c, _, err := websocket.Dial(ctx, url, nil); err == nil {
			c.Close(websocket.StatusNormalClosure, "")
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("a closed connection must return its slot")
}

func TestWebSocketReadLimitClosesAnOversizedFrame(t *testing.T) {
	cfg := Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := NewManager(cfg, newLogger("error"))
	ws := newWSServer(mgr, cfg, newLogger("error"))
	srv := httptest.NewServer(ws.handler())
	defer srv.Close()

	url := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws"
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	c := wsDial(t, ctx, url)
	defer c.Close(websocket.StatusNormalClosure, "")

	giant, _ := json.Marshal(map[string]any{
		"type": TypeCreateLobby, "player": strings.Repeat("A", wsReadLimit*2),
	})
	_ = c.Write(ctx, websocket.MessageText, giant)
	if _, _, err := c.Read(ctx); err == nil {
		t.Fatal("a frame past the read limit must close the connection, not be parsed")
	}
	if len(mgr.lobbies) != 0 {
		t.Fatal("nothing may be created from a frame that exceeded the read limit")
	}
}

// ---- UDP listener survival ----------------------------------------------------

// One datagram must not be able to kill a UDP listener. On Linux the kernel
// truncates an oversized datagram silently, but on Windows ReadFromUDP returns
// WSAEMSGSIZE — and the read loops used to `return` on any error, so a single
// 3 KB packet ended the relay (or an ICMP port-unreachable from a peer that had
// simply quit, which needs no attacker at all).
func TestStunStillAnswersAfterAnOversizedDatagram(t *testing.T) {
	s, err := startStun("127.0.0.1:0", newLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
	}
	defer func() { _ = s.Close() }()

	conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	if _, err := conn.Write(make([]byte, kStunMaxDatagram*4)); err != nil {
		t.Fatalf("write oversized: %v", err)
	}
	probe, _ := json.Marshal(stunProbe{Type: "StunProbe", Nonce: "alive"})
	if _, err := conn.Write(probe); err != nil {
		t.Fatalf("write probe: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("the listener died on one oversized datagram: %v", err)
	}
	var reply stunReply
	if err := json.Unmarshal(buf[:n], &reply); err != nil || reply.Nonce != "alive" {
		t.Fatalf("bad reply after the poison datagram: %q (%v)", reply.Nonce, err)
	}
}

// The relay's survival, proven by forwarding real traffic after the bad packet.
func TestRelayStillForwardsAfterAnOversizedDatagram(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	allocA := allocateRelay(t, m, host, nil).AllocID
	allocB := allocateRelay(t, m, guest, nil).AllocID

	srv, err := startRelay("127.0.0.1:0", m.relay, newLogger("error"))
	if err != nil {
		t.Fatalf("startRelay: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	poison, err := net.DialUDP("udp", nil, relayAddr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	if _, err := poison.Write(make([]byte, kRelayMaxDatagram*2)); err != nil {
		t.Fatalf("write oversized: %v", err)
	}
	_ = poison.Close()

	peerA, _ := net.DialUDP("udp", nil, relayAddr)
	peerB, _ := net.DialUDP("udp", nil, relayAddr)
	defer func() { _ = peerA.Close(); _ = peerB.Close() }()

	_ = peerB.SetReadDeadline(time.Now().Add(3 * time.Second))
	deadline := time.Now().Add(3 * time.Second)
	buf := make([]byte, 2048)
	for time.Now().Before(deadline) {
		_, _ = peerB.Write(relayPacket(t, allocB, 0, []byte("teach"))) // teach B's address
		_, _ = peerA.Write(relayPacket(t, allocA, 1, []byte("BOMB")))
		_ = peerB.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, err := peerB.Read(buf)
		if err != nil {
			continue
		}
		if n > kRelayHeaderLen && string(buf[kRelayHeaderLen:n]) == "BOMB" {
			return
		}
	}
	t.Fatal("the relay stopped forwarding after one oversized datagram")
}

// ---- STUN --------------------------------------------------------------------

func TestStunDropsOversizedProbesAndNonces(t *testing.T) {
	s, err := startStun("127.0.0.1:0", newLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
	}
	defer func() { _ = s.Close() }()

	dial := func() *net.UDPConn {
		conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
		if err != nil {
			t.Fatalf("dial: %v", err)
		}
		return conn
	}
	silent := func(conn *net.UDPConn, what string) {
		t.Helper()
		_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		buf := make([]byte, 4096)
		if _, err := conn.Read(buf); err == nil {
			t.Fatalf("%s must get no reply", what)
		}
	}

	// A datagram past the size cap: dropped before it is even parsed, so the
	// reply can never be grown by a large echoed nonce.
	big := dial()
	defer func() { _ = big.Close() }()
	probe, _ := json.Marshal(stunProbe{Type: "StunProbe", Nonce: strings.Repeat("n", kStunMaxDatagram)})
	if _, err := big.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	silent(big, "an oversized datagram")

	// A nonce past its own cap, inside an otherwise legal datagram.
	longNonce := dial()
	defer func() { _ = longNonce.Close() }()
	probe, _ = json.Marshal(stunProbe{Type: "StunProbe", Nonce: strings.Repeat("n", kStunMaxNonceBytes+1)})
	if len(probe) > kStunMaxDatagram {
		t.Fatalf("test setup: the probe must be under the datagram cap, got %d", len(probe))
	}
	if _, err := longNonce.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	silent(longNonce, "an over-long nonce")

	// And the real client's probe still works. The C++ StunClient's nonce is
	// "seat<N>-<32 hex lobby id>".
	ok := dial()
	defer func() { _ = ok.Close() }()
	probe, _ = json.Marshal(stunProbe{Type: "StunProbe", Nonce: "seat0-" + strings.Repeat("a", 32)})
	if _, err := ok.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	_ = ok.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := ok.Read(buf)
	if err != nil {
		t.Fatalf("a normal probe must still be answered: %v", err)
	}
	var reply stunReply
	if err := json.Unmarshal(buf[:n], &reply); err != nil || reply.Nonce != "seat0-"+strings.Repeat("a", 32) {
		t.Fatalf("nonce not echoed verbatim: %q (%v)", reply.Nonce, err)
	}
}
