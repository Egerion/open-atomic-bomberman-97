package main

import (
	"bytes"
	"encoding/hex"
	"net"
	"strings"
	"testing"
	"time"
)

// ---- helpers ----------------------------------------------------------------

// allocateRelay drives AllocateRelay for one connection and returns the reply.
func allocateRelay(t *testing.T, m *Manager, c *fakeConn, extra map[string]any) relayAllocatedMsg {
	t.Helper()
	msg := map[string]any{"type": TypeAllocateRelay}
	for k, v := range extra {
		msg[k] = v
	}
	dispatchMap(m, c, msg)
	return lastTyped[relayAllocatedMsg](t, c, TypeRelayAllocated)
}

// relayPacket builds a client→relay datagram: [16B alloc_id][1B dst_seat][payload].
func relayPacket(t *testing.T, allocID string, dstSeat int, payload []byte) []byte {
	t.Helper()
	raw, err := hex.DecodeString(allocID)
	if err != nil || len(raw) != kAllocIDLen {
		t.Fatalf("alloc_id %q is not 16 binary bytes of hex", allocID)
	}
	return append(append(raw, byte(dstSeat)), payload...)
}

func mustAddr(t *testing.T, s string) *net.UDPAddr {
	t.Helper()
	a, err := net.ResolveUDPAddr("udp", s)
	if err != nil {
		t.Fatalf("resolve %q: %v", s, err)
	}
	return a
}

// ---- control plane: AllocateRelay -------------------------------------------

func TestAllocateRelayHappyPath(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")

	ra := allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})
	if ra.RelayAddr != "relay.test:8082" {
		t.Fatalf("relay_addr should be the advertised address, got %q", ra.RelayAddr)
	}
	if len(ra.AllocID) != 2*kAllocIDLen {
		t.Fatalf("alloc_id should be %d hex chars, got %q", 2*kAllocIDLen, ra.AllocID)
	}
	if _, err := hex.DecodeString(ra.AllocID); err != nil {
		t.Fatalf("alloc_id must be lowercase hex: %v", err)
	}
	if ra.AllocID != strings.ToLower(ra.AllocID) {
		t.Fatalf("alloc_id must be LOWERCASE hex, got %q", ra.AllocID)
	}

	rb := allocateRelay(t, m, guest, map[string]any{"lobby_id": lc.LobbyID, "seat": 1})
	if rb.AllocID == ra.AllocID {
		t.Fatal("distinct seats must get distinct alloc_ids")
	}
	if got := m.relay.size(); got != 2 {
		t.Fatalf("table should hold 2 allocations, got %d", got)
	}
}

func TestAllocateRelayIsIdempotentPerSeat(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)

	first := allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})
	host.reset()
	second := allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})

	if second.AllocID != first.AllocID {
		t.Fatalf("re-allocating the same seat must return the SAME alloc_id: %q vs %q", first.AllocID, second.AllocID)
	}
	if got := m.relay.size(); got != 1 {
		t.Fatalf("idempotent re-allocation must not add a row, got %d", got)
	}
}

// A seat's learned address must survive a re-allocation, otherwise a retry after
// a lost reply would silently break the return path mid-match.
func TestAllocateRelayIdempotentKeepsLearnedAddr(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	ra := allocateRelay(t, m, host, nil)

	src := mustAddr(t, "203.0.113.7:41234")
	m.relay.forward(src, relayPacket(t, ra.AllocID, 9, []byte("x"))) // learns src, drops (no seat 9)

	host.reset()
	again := allocateRelay(t, m, host, nil)
	if again.AllocID != ra.AllocID {
		t.Fatal("expected the same alloc_id")
	}
	m.relay.mu.Lock()
	defer m.relay.mu.Unlock()
	a := m.relay.byKey[allocKey{code: lc.Code, seat: 0}]
	if a == nil || a.addr == nil || a.addr.String() != src.String() {
		t.Fatalf("learned address must survive re-allocation, got %v", a)
	}
}

func TestAllocateRelayNotInLobby(t *testing.T) {
	m := newTestManager(t)
	stray := newFakeConn("stray")
	dispatchMap(m, stray, map[string]any{"type": TypeAllocateRelay, "seat": 0})

	em := lastTyped[errorMsg](t, stray, TypeError)
	if em.Code != "not_in_lobby" {
		t.Fatalf("expected not_in_lobby, got %q", em.Code)
	}
	if m.relay.size() != 0 {
		t.Fatal("a rejected AllocateRelay must not create an allocation")
	}
}

// The seat field is a cross-check only — an allocation always belongs to the
// sender's own seat, so a peer cannot mint or steal another seat's handle.
func TestAllocateRelayRejectsForeignSeat(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	guest.reset()

	dispatchMap(m, guest, map[string]any{"type": TypeAllocateRelay, "lobby_id": lc.LobbyID, "seat": 0})
	em := lastTyped[errorMsg](t, guest, TypeError)
	if em.Code != "bad_message" {
		t.Fatalf("expected bad_message for a foreign seat, got %q", em.Code)
	}
	if m.relay.size() != 0 {
		t.Fatal("a rejected AllocateRelay must not create an allocation")
	}
}

// An omitted seat is legal (it is advisory) and allocates for the sender's seat.
func TestAllocateRelayOmittedSeatUsesOwnSeat(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	guest.reset()

	allocateRelay(t, m, guest, nil)
	m.relay.mu.Lock()
	defer m.relay.mu.Unlock()
	if _, ok := m.relay.byKey[allocKey{code: lc.Code, seat: 1}]; !ok {
		t.Fatal("omitted seat should allocate for the sender's own seat (1)")
	}
}

func TestAllocateRelayMalformed(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	createLobby(t, m, host, nil)
	host.reset()

	// seat as a string → the typed decode fails.
	dispatchMap(m, host, map[string]any{"type": TypeAllocateRelay, "seat": "one"})
	em := lastTyped[errorMsg](t, host, TypeError)
	if em.Code != "bad_message" {
		t.Fatalf("expected bad_message, got %q", em.Code)
	}
}

// ---- data plane: end-to-end forwarding ---------------------------------------

func TestRelayForwardsEndToEnd(t *testing.T) {
	m := newTestManager(t)
	srv, err := startRelay("127.0.0.1:0", m.relay, newLogger("error"))
	if err != nil {
		t.Fatalf("startRelay: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	allocA := allocateRelay(t, m, host, nil).AllocID  // seat 0
	allocB := allocateRelay(t, m, guest, nil).AllocID // seat 1

	sockA, err := net.DialUDP("udp", nil, relayAddr)
	if err != nil {
		t.Fatalf("dial A: %v", err)
	}
	defer func() { _ = sockA.Close() }()
	sockB, err := net.DialUDP("udp", nil, relayAddr)
	if err != nil {
		t.Fatalf("dial B: %v", err)
	}
	defer func() { _ = sockB.Close() }()

	// B speaks first so the relay learns B's public address (this datagram is
	// itself dropped — A has not spoken yet, so there is nothing to forward to).
	if _, err := sockB.Write(relayPacket(t, allocB, 0, []byte("hello-from-b"))); err != nil {
		t.Fatalf("B write: %v", err)
	}
	waitForCounter(t, func() uint64 { return m.relay.noAddr.Load() }, 1)

	payload := []byte{0x00, 0x01, 0xFF, 0x7F, 'o', 'p', 'a', 'q', 'u', 'e', 0x00}
	if _, err := sockA.Write(relayPacket(t, allocA, 1, payload)); err != nil {
		t.Fatalf("A write: %v", err)
	}

	_ = sockB.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := sockB.Read(buf)
	if err != nil {
		t.Fatalf("B read: %v", err)
	}
	got := buf[:n]
	if n != kRelayHeaderLen+len(payload) {
		t.Fatalf("relayed datagram should be 17+%d bytes, got %d", len(payload), n)
	}
	// Header must be [B's OWN alloc_id][A's seat].
	wantID, _ := hex.DecodeString(allocB)
	if !bytes.Equal(got[:kAllocIDLen], wantID) {
		t.Fatalf("header alloc_id should be the RECEIVER's (%s), got %x", allocB, got[:kAllocIDLen])
	}
	if got[kAllocIDLen] != 0 {
		t.Fatalf("header seat should be the SENDER's seat 0, got %d", got[kAllocIDLen])
	}
	if !bytes.Equal(got[kRelayHeaderLen:], payload) {
		t.Fatalf("payload must be byte-identical: got % x want % x", got[kRelayHeaderLen:], payload)
	}

	// Return path: A's address is learned now, so B→A forwards too.
	back := []byte("pong")
	if _, err := sockB.Write(relayPacket(t, allocB, 0, back)); err != nil {
		t.Fatalf("B write 2: %v", err)
	}
	_ = sockA.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err = sockA.Read(buf)
	if err != nil {
		t.Fatalf("A read: %v", err)
	}
	got = buf[:n]
	wantID, _ = hex.DecodeString(allocA)
	if !bytes.Equal(got[:kAllocIDLen], wantID) || got[kAllocIDLen] != 1 {
		t.Fatalf("return header should be [allocA][seat 1], got %x seat=%d", got[:kAllocIDLen], got[kAllocIDLen])
	}
	if !bytes.Equal(got[kRelayHeaderLen:], back) {
		t.Fatalf("return payload mismatch: % x", got[kRelayHeaderLen:])
	}
}

// A short datagram must not crash the listener or draw a reply.
func TestRelayIgnoresShortDatagramOverUDP(t *testing.T) {
	m := newTestManager(t)
	srv, err := startRelay("127.0.0.1:0", m.relay, newLogger("error"))
	if err != nil {
		t.Fatalf("startRelay: %v", err)
	}
	defer func() { _ = srv.Close() }()

	conn, err := net.DialUDP("udp", nil, srv.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	if _, err := conn.Write(make([]byte, kRelayHeaderLen-1)); err != nil {
		t.Fatalf("write: %v", err)
	}
	waitForCounter(t, func() uint64 { return m.relay.short.Load() }, 1)

	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if _, err := conn.Read(make([]byte, 256)); err == nil {
		t.Fatal("relay must never answer a malformed datagram")
	}
}

// ---- data plane: drop paths (table level, deterministic) ---------------------

func TestRelayDropPaths(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, map[string]any{"max_seats": 4})
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	allocA := allocateRelay(t, m, host, nil).AllocID  // seat 0
	allocB := allocateRelay(t, m, guest, nil).AllocID // seat 1

	srcA := mustAddr(t, "203.0.113.7:41234")
	tbl := m.relay

	t.Run("short datagram", func(t *testing.T) {
		for _, n := range []int{0, 1, kAllocIDLen, kRelayHeaderLen - 1} {
			if _, _, ok := tbl.forward(srcA, make([]byte, n)); ok {
				t.Fatalf("%d-byte datagram must be dropped", n)
			}
		}
	})

	t.Run("unknown alloc_id", func(t *testing.T) {
		bogus := make([]byte, kRelayHeaderLen+4)
		bogus[0] = 0xDE
		if _, _, ok := tbl.forward(srcA, bogus); ok {
			t.Fatal("unknown alloc_id must be dropped")
		}
	})

	t.Run("destination address not learned", func(t *testing.T) {
		// A speaks (learning srcA); B is allocated but silent, so A→B drops.
		if _, _, ok := tbl.forward(srcA, relayPacket(t, allocA, 1, []byte("hi"))); ok {
			t.Fatal("must drop while the destination address is unknown")
		}
	})

	t.Run("unknown destination seat", func(t *testing.T) {
		// Seat 3 exists in the lobby's seat range but holds no allocation.
		if _, _, ok := tbl.forward(srcA, relayPacket(t, allocA, 3, []byte("hi"))); ok {
			t.Fatal("unallocated destination seat must be dropped")
		}
		// ...and so must a seat number outside the roster entirely.
		if _, _, ok := tbl.forward(srcA, relayPacket(t, allocA, 255, []byte("hi"))); ok {
			t.Fatal("out-of-range destination seat must be dropped")
		}
	})

	t.Run("cross-lobby is impossible", func(t *testing.T) {
		// A second lobby's seat 1 must be unreachable from this lobby's seat 0:
		// the destination is looked up under the SENDER's lobby code.
		other := newFakeConn("other-host")
		lc2 := createLobby(t, m, other, nil)
		other2 := join(m, lc2.Code, "Leo", "0xA1B2C3D4", "other-guest")
		allocOther := allocateRelay(t, m, other2, nil).AllocID
		if allocOther == allocB {
			t.Fatal("distinct lobbies must get distinct alloc_ids")
		}
		srcOther := mustAddr(t, "198.51.100.9:5000")
		if _, _, ok := tbl.forward(srcOther, relayPacket(t, allocOther, 0, []byte("x"))); ok {
			t.Fatal("lobby 2 seat 1 → seat 0 should drop: seat 0 of lobby 2 never spoke")
		}
		// And lobby 1's A still cannot reach it either.
		if _, _, ok := tbl.forward(srcA, relayPacket(t, allocA, 1, []byte("x"))); ok {
			t.Fatal("lobby 1 seat 0 must not reach lobby 2's learned peer")
		}
	})

	// Every one of the above must have been counted, not silently discarded.
	c := tbl.snapshot()
	if c.forwarded != 0 {
		t.Fatalf("no datagram should have been forwarded, got %d", c.forwarded)
	}
	if c.short != 4 || c.unknownAlloc != 1 || c.unknownDst != 3 || c.noAddr != 2 {
		t.Fatalf("drop tallies wrong: %+v", c)
	}
}

// ---- allocation lifetime -----------------------------------------------------

func TestRelayAllocationFreedOnMemberDrop(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	allocA := allocateRelay(t, m, host, nil).AllocID
	allocateRelay(t, m, guest, nil)
	if m.relay.size() != 2 {
		t.Fatalf("expected 2 allocations, got %d", m.relay.size())
	}

	m.removeConn(guest)
	if got := m.relay.size(); got != 1 {
		t.Fatalf("the departed member's allocation must be freed, got %d rows", got)
	}
	// The survivor's handle keeps working.
	m.relay.mu.Lock()
	_, stillThere := m.relay.byKey[allocKey{code: lc.Code, seat: 0}]
	m.relay.mu.Unlock()
	if !stillThere {
		t.Fatalf("the surviving seat's allocation %s must remain", allocA)
	}

	// Emptying the lobby evicts it and frees the rest.
	m.removeConn(host)
	if got := m.relay.size(); got != 0 {
		t.Fatalf("lobby eviction must free every allocation, got %d rows", got)
	}
}

func TestRelayAllocationsFreedOnHeartbeatReap(t *testing.T) {
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }

	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	allocateRelay(t, m, host, nil)
	allocateRelay(t, m, guest, nil)

	// Push past interval × miss so both members time out and the lobby evicts.
	now = now.Add(m.cfg.HeartbeatInterval*time.Duration(m.cfg.HeartbeatMiss) + time.Second)
	m.reap()

	if !host.isClosed() || !guest.isClosed() {
		t.Fatal("both members should have been dropped by the reaper")
	}
	if got := m.relay.size(); got != 0 {
		t.Fatalf("reaped members must free their allocations, got %d rows", got)
	}
	if _, ok := m.lobbies[lc.Code]; ok {
		t.Fatal("drained lobby should be evicted")
	}
}

func TestRelayIdleExpiry(t *testing.T) {
	now := time.Now()
	tbl := newRelayTable(60*time.Second, newLogger("error"))
	tbl.now = func() time.Time { return now }

	idA, err := tbl.allocate("AAAAAA", 0)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.allocate("AAAAAA", 1); err != nil {
		t.Fatalf("allocate: %v", err)
	}

	// Just short of the window: nothing expires.
	now = now.Add(59 * time.Second)
	if n := tbl.reapIdle(); n != 0 {
		t.Fatalf("nothing should expire before the idle window, reaped %d", n)
	}

	// Seat 0 sends: its timer resets, seat 1's does not.
	tbl.forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, idA, 9, []byte("x")))
	now = now.Add(2 * time.Second)
	if n := tbl.reapIdle(); n != 1 {
		t.Fatalf("only the silent seat should expire, reaped %d", n)
	}
	if tbl.size() != 1 {
		t.Fatalf("the active seat must survive, %d rows left", tbl.size())
	}

	// Now let the active one go idle too.
	now = now.Add(61 * time.Second)
	if n := tbl.reapIdle(); n != 1 {
		t.Fatalf("the idle seat should expire, reaped %d", n)
	}
	if tbl.size() != 0 {
		t.Fatalf("table should be empty, %d rows left", tbl.size())
	}
	// An expired handle no longer forwards.
	if _, _, ok := tbl.forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, idA, 1, []byte("x"))); ok {
		t.Fatal("an expired alloc_id must be unknown")
	}
}

func TestRelayIdleZeroDisablesExpiry(t *testing.T) {
	now := time.Now()
	tbl := newRelayTable(0, newLogger("error"))
	tbl.now = func() time.Time { return now }
	if _, err := tbl.allocate("AAAAAA", 0); err != nil {
		t.Fatalf("allocate: %v", err)
	}
	now = now.Add(24 * time.Hour)
	if n := tbl.reapIdle(); n != 0 {
		t.Fatalf("idle=0 must disable expiry, reaped %d", n)
	}
}

// ---- config ------------------------------------------------------------------

func TestRelayAdvertiseFallsBackToListenAddr(t *testing.T) {
	cfg := parseConfig([]string{"-relay-addr", "127.0.0.1:9999"})
	if got := cfg.relayAdvertise(); got != "127.0.0.1:9999" {
		t.Fatalf("unset -relay-advertise should fall back to -relay-addr, got %q", got)
	}
	cfg = parseConfig([]string{"-relay-addr", ":8082", "-relay-advertise", "relay.example:8082"})
	if got := cfg.relayAdvertise(); got != "relay.example:8082" {
		t.Fatalf("-relay-advertise should win, got %q", got)
	}
	if cfg.RelayIdle != 60*time.Second {
		t.Fatalf("default relay idle should be 60s, got %v", cfg.RelayIdle)
	}
}

func TestHasRoutableHost(t *testing.T) {
	for _, c := range []struct {
		addr string
		want bool
	}{
		{"relay.example:8082", true},
		{"203.0.113.7:8082", true},
		{"[2001:db8::1]:8082", true},
		{":8082", false},
		{"0.0.0.0:8082", false},
		{"[::]:8082", false},
		{"nonsense", false},
	} {
		if got := hasRoutableHost(c.addr); got != c.want {
			t.Fatalf("hasRoutableHost(%q) = %v, want %v", c.addr, got, c.want)
		}
	}
}

// ---- small helpers -----------------------------------------------------------

// waitForCounter polls an atomic tally the relay goroutine owns, so a UDP test
// never races the listener.
func waitForCounter(t *testing.T, load func() uint64, want uint64) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if load() >= want {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("counter did not reach %d within the deadline (got %d)", want, load())
}
