package lobby

import (
	"encoding/hex"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
)

// ---- helpers ----------------------------------------------------------------

// allocateRelay drives AllocateRelay for one connection and returns the reply.
func allocateRelay(t *testing.T, m *Manager, c *fakeConn, extra map[string]any) protocol.RelayAllocatedMsg {
	t.Helper()
	msg := map[string]any{"type": protocol.TypeAllocateRelay}
	for k, v := range extra {
		msg[k] = v
	}
	dispatchMap(m, c, msg)
	return lastTyped[protocol.RelayAllocatedMsg](t, c, protocol.TypeRelayAllocated)
}

// relayPacket builds a client→relay datagram: [16B alloc_id][1B dst_seat][payload].
func relayPacket(t *testing.T, allocID string, dstSeat int, payload []byte) []byte {
	t.Helper()
	raw, err := hex.DecodeString(allocID)
	if err != nil || len(raw) != relay.AllocIDLen {
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
	if len(ra.AllocID) != 2*relay.AllocIDLen {
		t.Fatalf("alloc_id should be %d hex chars, got %q", 2*relay.AllocIDLen, ra.AllocID)
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
	if got := m.relay.Size(); got != 2 {
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
	if got := m.relay.Size(); got != 1 {
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
	m.relay.Forward(src, relayPacket(t, ra.AllocID, 9, []byte("x"))) // learns src, drops (no seat 9)

	host.reset()
	again := allocateRelay(t, m, host, nil)
	if again.AllocID != ra.AllocID {
		t.Fatal("expected the same alloc_id")
	}
	addr, ok := m.relay.LearnedAddr(lc.Code, 0)
	if !ok || addr == nil || addr.String() != src.String() {
		t.Fatalf("learned address must survive re-allocation, got %v (ok=%v)", addr, ok)
	}
}

func TestAllocateRelayNotInLobby(t *testing.T) {
	m := newTestManager(t)
	stray := newFakeConn("stray")
	dispatchMap(m, stray, map[string]any{"type": protocol.TypeAllocateRelay, "seat": 0})

	em := lastTyped[protocol.ErrorMsg](t, stray, protocol.TypeError)
	if em.Code != "not_in_lobby" {
		t.Fatalf("expected not_in_lobby, got %q", em.Code)
	}
	if m.relay.Size() != 0 {
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

	dispatchMap(m, guest, map[string]any{"type": protocol.TypeAllocateRelay, "lobby_id": lc.LobbyID, "seat": 0})
	em := lastTyped[protocol.ErrorMsg](t, guest, protocol.TypeError)
	if em.Code != "bad_message" {
		t.Fatalf("expected bad_message for a foreign seat, got %q", em.Code)
	}
	if m.relay.Size() != 0 {
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
	if _, ok := m.relay.LearnedAddr(lc.Code, 1); !ok {
		t.Fatal("omitted seat should allocate for the sender's own seat (1)")
	}
}

func TestAllocateRelayMalformed(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	createLobby(t, m, host, nil)
	host.reset()

	// seat as a string → the typed decode fails.
	dispatchMap(m, host, map[string]any{"type": protocol.TypeAllocateRelay, "seat": "one"})
	em := lastTyped[protocol.ErrorMsg](t, host, protocol.TypeError)
	if em.Code != "bad_message" {
		t.Fatalf("expected bad_message, got %q", em.Code)
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
	if m.relay.Size() != 2 {
		t.Fatalf("expected 2 allocations, got %d", m.relay.Size())
	}

	m.RemoveConn(guest)
	if got := m.relay.Size(); got != 1 {
		t.Fatalf("the departed member's allocation must be freed, got %d rows", got)
	}
	// The survivor's handle keeps working.
	_, stillThere := m.relay.LearnedAddr(lc.Code, 0)
	if !stillThere {
		t.Fatalf("the surviving seat's allocation %s must remain", allocA)
	}

	// Emptying the lobby evicts it and frees the rest.
	m.RemoveConn(host)
	if got := m.relay.Size(); got != 0 {
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
	if got := m.relay.Size(); got != 0 {
		t.Fatalf("reaped members must free their allocations, got %d rows", got)
	}
	if _, ok := m.lobbies[lc.Code]; ok {
		t.Fatal("drained lobby should be evicted")
	}
}
