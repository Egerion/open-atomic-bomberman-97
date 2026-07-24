package main

import (
	"testing"
	"time"
)

func TestCreateJoinReadyStartHappyPath(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")

	lc := createLobby(t, m, host, map[string]any{"max_seats": 2})
	if len(lc.Code) != 6 {
		t.Fatalf("code should be 6 chars, got %q", lc.Code)
	}
	if lc.HostToken == "" || lc.LobbyID == "" {
		t.Fatal("host_token and lobby_id must be non-empty opaque handles")
	}
	if lc.YourSeat != 0 {
		t.Fatalf("host seat should be 0, got %d", lc.YourSeat)
	}

	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	ja := lastTyped[joinAcceptedMsg](t, guest, TypeJoinAccepted)
	if ja.YourSeat != 1 {
		t.Fatalf("guest seat should be 1, got %d", ja.YourSeat)
	}
	if len(ja.Roster) != 2 {
		t.Fatalf("roster should have 2 seats, got %d", len(ja.Roster))
	}
	if !host.has(TypeRosterUpdate) {
		t.Fatal("host should receive a RosterUpdate when the guest joins")
	}

	setReady(m, host, true)
	setReady(m, guest, true)

	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})

	hs := lastTyped[startMatchMsg](t, host, TypeStartMatch)
	gs := lastTyped[startMatchMsg](t, guest, TypeStartMatch)
	if hs.Seed != gs.Seed {
		t.Fatalf("all peers must receive the same seed: host=%d guest=%d", hs.Seed, gs.Seed)
	}
	if hs.InputDelay != 2 {
		t.Fatalf("default input_delay should be 2, got %d", hs.InputDelay)
	}
	if hs.Topology.HubSeat != 0 {
		t.Fatalf("hub_seat should be the host seat 0, got %d", hs.Topology.HubSeat)
	}
	if len(hs.SeatAssign) != 2 || hs.SeatAssign[0] != 0 || hs.SeatAssign[1] != 1 {
		t.Fatalf("seat_assign should be [0,1], got %v", hs.SeatAssign)
	}
	if hs.LocalSeatsMask != 0b01 {
		t.Fatalf("host local_seats_mask should be 0b01, got %d", hs.LocalSeatsMask)
	}
	if gs.LocalSeatsMask != 0b10 {
		t.Fatalf("guest local_seats_mask should be 0b10, got %d", gs.LocalSeatsMask)
	}
}

func TestStartEchoesHostSuppliedParity(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)

	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
		"input_delay": 1, "match_config_digest": "0xC0FFEE01",
	})
	hs := lastTyped[startMatchMsg](t, host, TypeStartMatch)
	if hs.InputDelay != 1 {
		t.Fatalf("host-supplied input_delay should pass through, got %d", hs.InputDelay)
	}
	if hs.MatchConfigDigest != "0xC0FFEE01" {
		t.Fatalf("host-supplied match_config_digest should pass through, got %q", hs.MatchConfigDigest)
	}
}

func TestBuildHashMismatchRejected(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"build_hash": "0xA1B2C3D4"})

	bad := join(m, lc.Code, "Ada", "0xDEADBEEF", "bad")
	jr := lastTyped[joinRejectedMsg](t, bad, TypeJoinRejected)
	if jr.Reason != ReasonBuildMismatch {
		t.Fatalf("expected build_mismatch, got %q", jr.Reason)
	}
}

func TestBuildHashNormalizationAccepts(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"build_hash": "0xA1B2C3D4"})

	// Same value, different surface form (no 0x, lowercase) must be accepted.
	guest := join(m, lc.Code, "Ada", "a1b2c3d4", "guest")
	if !guest.has(TypeJoinAccepted) {
		t.Fatal("normalized-equal build_hash should be accepted")
	}
}

func TestFullLobbyRejected(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 2})

	g1 := join(m, lc.Code, "Ada", "0xA1B2C3D4", "g1")
	if !g1.has(TypeJoinAccepted) {
		t.Fatal("second seat should join")
	}
	g2 := join(m, lc.Code, "Leo", "0xA1B2C3D4", "g2")
	jr := lastTyped[joinRejectedMsg](t, g2, TypeJoinRejected)
	if jr.Reason != ReasonFull {
		t.Fatalf("expected full, got %q", jr.Reason)
	}
}

func TestJoinNotFound(t *testing.T) {
	m := newTestManager(t)
	c := join(m, "ZZZZZZ", "Ada", "0xA1B2C3D4", "c")
	jr := lastTyped[joinRejectedMsg](t, c, TypeJoinRejected)
	if jr.Reason != ReasonNotFound {
		t.Fatalf("expected not_found, got %q", jr.Reason)
	}
}

func TestCandidateFanOut(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	host.reset()
	guest.reset()

	// Host posts its host candidate; the guest must receive it tagged seat 0.
	dispatchMap(m, host, map[string]any{
		"type": TypeCandidates, "lobby_id": lc.LobbyID, "seat": 0,
		"list": []map[string]any{{"kind": "host", "addr": "192.168.1.9:41234"}},
	})
	pc := lastTyped[peerCandidatesMsg](t, guest, TypePeerCandidates)
	if pc.Seat != 0 || len(pc.List) != 1 || pc.List[0].Addr != "192.168.1.9:41234" {
		t.Fatalf("guest should get host's candidate tagged seat 0, got %+v", pc)
	}

	// Guest posts its reflexive candidate; host gets it tagged seat 1, and the
	// guest is back-filled with the host's already-known list (seat 0).
	dispatchMap(m, guest, map[string]any{
		"type": TypeCandidates, "lobby_id": lc.LobbyID, "seat": 1,
		"list": []map[string]any{{"kind": "reflexive", "addr": "81.2.3.4:52001"}},
	})
	hostPC := lastTyped[peerCandidatesMsg](t, host, TypePeerCandidates)
	if hostPC.Seat != 1 || hostPC.List[0].Addr != "81.2.3.4:52001" {
		t.Fatalf("host should get guest's candidate tagged seat 1, got %+v", hostPC)
	}
	if guest.countOfType(TypePeerCandidates) < 2 {
		t.Fatal("guest should be back-filled with the host's candidates")
	}
}

func TestStartRejectedNotAllReady(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true) // guest NOT ready

	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	em := lastTyped[errorMsg](t, host, TypeError)
	if em.Code != "not_all_ready" {
		t.Fatalf("expected not_all_ready, got %q", em.Code)
	}
	if guest.has(TypeStartMatch) || host.has(TypeStartMatch) {
		t.Fatal("no StartMatch should be broadcast when not all ready")
	}
}

func TestStartRejectedBadHostToken(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)

	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": "wrong-token",
	})
	em := lastTyped[errorMsg](t, host, TypeError)
	if em.Code != "not_host" {
		t.Fatalf("expected not_host, got %q", em.Code)
	}
}

func TestStartRejectedFromNonHost(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)

	// Guest tries to start using the (leaked) host token — must fail on seat check.
	dispatchMap(m, guest, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	em := lastTyped[errorMsg](t, guest, TypeError)
	if em.Code != "not_host" {
		t.Fatalf("expected not_host, got %q", em.Code)
	}
}

func TestJoinRejectedInProgress(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 4})
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)
	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	if !host.has(TypeStartMatch) {
		t.Fatal("match should have started")
	}

	late := join(m, lc.Code, "Leo", "0xA1B2C3D4", "late")
	jr := lastTyped[joinRejectedMsg](t, late, TypeJoinRejected)
	if jr.Reason != ReasonInProgress {
		t.Fatalf("late join should reject in_progress, got %q", jr.Reason)
	}
}

func TestHeartbeatAck(t *testing.T) {
	m := newTestManager(t)
	c := newFakeConn("c")
	dispatchMap(m, c, map[string]any{"type": TypeHeartbeat})
	if !c.has(TypeHeartbeatAck) {
		t.Fatal("Heartbeat should get a HeartbeatAck")
	}
}

func TestReaperDropsStaleMember(t *testing.T) {
	m := newTestManager(t)
	clk := time.Now()
	m.now = func() time.Time { return clk }

	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 2})
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	host.reset()
	guest.reset()

	// Advance past the K-heartbeat deadline (interval*miss = 3s), then keep the
	// host alive with a heartbeat so only the guest is stale.
	clk = clk.Add(4 * time.Second)
	dispatchMap(m, host, map[string]any{"type": TypeHeartbeat})
	m.reap()

	if !guest.isClosed() {
		t.Fatal("stale guest should be disconnected")
	}
	ru := lastTyped[rosterUpdateMsg](t, host, TypeRosterUpdate)
	if len(ru.Roster) != 1 || ru.Roster[0].Seat != 0 {
		t.Fatalf("host should see a 1-seat roster after the drop, got %+v", ru.Roster)
	}
}

func TestReaperEvictsEmptyLobby(t *testing.T) {
	m := newTestManager(t)
	clk := time.Now()
	m.now = func() time.Time { return clk }

	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)

	clk = clk.Add(4 * time.Second)
	m.reap()

	if !host.isClosed() {
		t.Fatal("lone stale host should be disconnected")
	}
	// The code must be free again.
	c := join(m, lc.Code, "Ada", "0xA1B2C3D4", "late")
	jr := lastTyped[joinRejectedMsg](t, c, TypeJoinRejected)
	if jr.Reason != ReasonNotFound {
		t.Fatalf("evicted code should resolve not_found, got %q", jr.Reason)
	}
}

func TestRemoveConnEvictsWhenEmpty(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	m.removeConn(host)

	c := join(m, lc.Code, "Ada", "0xA1B2C3D4", "late")
	jr := lastTyped[joinRejectedMsg](t, c, TypeJoinRejected)
	if jr.Reason != ReasonNotFound {
		t.Fatalf("expected not_found after last member left, got %q", jr.Reason)
	}
}

func TestReanchorLobby(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")

	// Host drops; the surviving guest re-anchors under the same code.
	m.removeConn(host)
	guest.reset()

	survivingRoster := []RosterEntry{{Seat: 1, Name: "Ada"}}
	digest := rosterDigest(survivingRoster)
	dispatchMap(m, guest, map[string]any{
		"type": TypeReanchorLobby, "code": lc.Code, "roster_digest": digest,
	})

	ra := lastTyped[reanchorAcceptedMsg](t, guest, TypeReanchorAccepted)
	if ra.Code != lc.Code {
		t.Fatalf("reanchor should keep the same code, got %q", ra.Code)
	}
	if ra.HostToken == "" || ra.HostToken == lc.HostToken {
		t.Fatal("reanchor should mint a fresh host_token")
	}
	ru := lastTyped[rosterUpdateMsg](t, guest, TypeRosterUpdate)
	if len(ru.Roster) != 1 || !ru.Roster[0].IsHost {
		t.Fatalf("promoted seat should be marked is_host, got %+v", ru.Roster)
	}
}

func TestReanchorRejectsWrongDigest(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	m.removeConn(host)

	dispatchMap(m, guest, map[string]any{
		"type": TypeReanchorLobby, "code": lc.Code, "roster_digest": "deadbeef",
	})
	em := lastTyped[errorMsg](t, guest, TypeError)
	if em.Code != "reanchor_rejected" {
		t.Fatalf("expected reanchor_rejected, got %q", em.Code)
	}
}

func TestMatchOverReopensForRematch(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 4})
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)
	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})

	dispatchMap(m, host, map[string]any{"type": TypeMatchOver, "lobby_id": lc.LobbyID})
	ru := lastTyped[rosterUpdateMsg](t, host, TypeRosterUpdate)
	for _, e := range ru.Roster {
		if e.Ready {
			t.Fatal("ready flags should be cleared on rematch")
		}
	}
	// A new player can join again (lobby is OPEN).
	late := join(m, lc.Code, "Leo", "0xA1B2C3D4", "late")
	if !late.has(TypeJoinAccepted) {
		t.Fatal("lobby should accept joins again after MatchOver")
	}
}

// AllocateRelay is now implemented; its control-plane and data-plane coverage
// lives in relay_test.go.

func TestListPublic(t *testing.T) {
	m := newTestManager(t)
	pub := newFakeConn("pub")
	createLobby(t, m, pub, map[string]any{"visibility": "public", "name": "Ege's game", "build_hash": "0xA1B2C3D4"})
	priv := newFakeConn("priv")
	createLobby(t, m, priv, map[string]any{"visibility": "private", "build_hash": "0xA1B2C3D4"})

	browser := newFakeConn("browser")
	dispatchMap(m, browser, map[string]any{"type": TypeListPublic, "build_hash": "0xA1B2C3D4"})
	pl := lastTyped[publicListMsg](t, browser, TypePublicList)
	if len(pl.Lobbies) != 1 {
		t.Fatalf("only the public lobby should be listed, got %d", len(pl.Lobbies))
	}
	if pl.Lobbies[0].Name != "Ege's game" || !pl.Lobbies[0].BuildOK {
		t.Fatalf("public row wrong: %+v", pl.Lobbies[0])
	}

	// Mismatched build still lists the lobby but flags build_ok=false.
	browser2 := newFakeConn("browser2")
	dispatchMap(m, browser2, map[string]any{"type": TypeListPublic, "build_hash": "0xFFFFFFFF"})
	pl2 := lastTyped[publicListMsg](t, browser2, TypePublicList)
	if len(pl2.Lobbies) != 1 || pl2.Lobbies[0].BuildOK {
		t.Fatalf("mismatched browser should see build_ok=false, got %+v", pl2.Lobbies)
	}
}

func TestUnknownMessageType(t *testing.T) {
	m := newTestManager(t)
	c := newFakeConn("c")
	dispatchMap(m, c, map[string]any{"type": "Nonsense"})
	em := lastTyped[errorMsg](t, c, TypeError)
	if em.Code != "unknown_type" {
		t.Fatalf("expected unknown_type, got %q", em.Code)
	}
}
