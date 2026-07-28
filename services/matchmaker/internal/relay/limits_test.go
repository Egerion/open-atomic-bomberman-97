package relay

import (
	"bytes"
	"encoding/hex"
	"errors"
	"net"
	"testing"
	"time"
)

// The two relay cost caps (Limits). The property under test throughout is not
// "the cap fires" — that is the easy half — but WHERE it fires: admission only.
// A refused allocation must never be reachable from the forwarding path, so a
// match that is already crossing the relay when a cap is hit keeps crossing it.

// ---- allocation ceiling -------------------------------------------------------

func TestAllocationCapRefusesANewSeat(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{MaxAllocations: 2}, testLogger())

	if _, err := tbl.Allocate("AAAAAA", 0); err != nil {
		t.Fatalf("seat 0 under the cap: %v", err)
	}
	if _, err := tbl.Allocate("AAAAAA", 1); err != nil {
		t.Fatalf("seat 1 under the cap: %v", err)
	}

	// A third seat — and equally a second lobby's first seat — is past the
	// ceiling and must be refused rather than admitted and reaped later.
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrAllocationLimit) {
		t.Fatalf("past the cap Allocate must return ErrAllocationLimit, got %v", err)
	}
	if tbl.Size() != 2 {
		t.Fatalf("a refused allocation must not add a row: %d rows", tbl.Size())
	}
	if got := tbl.Snapshot().AllocRefusedCap; got != 1 {
		t.Fatalf("the refusal must be counted for the operator, got %d", got)
	}
}

// The idempotent re-allocation PROTOCOL.md §6.1 promises is safe mid-match must
// keep working at the ceiling, or a client that merely retries after a lost
// RelayAllocated loses its handle the moment the server fills up.
func TestAllocationCapNeverRefusesASeatThatAlreadyHasOne(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{MaxAllocations: 2}, testLogger())
	first, err := tbl.Allocate("AAAAAA", 0)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.Allocate("AAAAAA", 1); err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrAllocationLimit) {
		t.Fatalf("precondition: the table should be at its cap, got %v", err)
	}

	again, err := tbl.Allocate("AAAAAA", 0)
	if err != nil {
		t.Fatalf("re-allocating an EXISTING seat at the cap must succeed: %v", err)
	}
	if again != first {
		t.Fatalf("re-allocation must return the same handle: %q vs %q", again, first)
	}
}

func TestZeroLimitsMeansUnlimited(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{}, testLogger())
	for seat := 0; seat < 64; seat++ {
		if _, err := tbl.Allocate("AAAAAA", seat); err != nil {
			t.Fatalf("a zero-valued Limits must not cap anything, seat %d: %v", seat, err)
		}
	}
	tbl.countForwarded(1 << 20)
	if _, err := tbl.Allocate("BBBBBB", 0); err != nil {
		t.Fatalf("a zero budget must not cap anything: %v", err)
	}
}

// ---- egress budget ------------------------------------------------------------

// The budget is denominated in what the HOST bills, not in what the forwarder
// moved: a datagram costs its payload plus one IPv4+UDP header. Getting this
// wrong understates a real invoice by about half at game payload sizes.
func TestEgressCountsThePerDatagramIPAndUDPHeaders(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{}, testLogger())
	tbl.countForwarded(100)
	tbl.countForwarded(100)

	c := tbl.Snapshot()
	if c.Forwarded != 2 {
		t.Fatalf("forwarded = %d, want 2", c.Forwarded)
	}
	if c.Bytes != 200 {
		t.Fatalf("bytes must stay payload-only (what the forwarder moved), got %d", c.Bytes)
	}
	if want := uint64(2 * (100 + kIPv4UDPOverhead)); c.Egress != want {
		t.Fatalf("egress must include %d B of IP+UDP per datagram: got %d want %d",
			kIPv4UDPOverhead, c.Egress, want)
	}
}

func TestEgressBudgetRefusesANewAllocationOnceSpent(t *testing.T) {
	const budget = 4096
	tbl := NewTable(time.Minute, Limits{EgressBudgetBytes: budget}, testLogger())

	if _, err := tbl.Allocate("AAAAAA", 0); err != nil {
		t.Fatalf("under the budget: %v", err)
	}
	// Still under: the budget bounds the total, so it is spent, not reserved.
	tbl.countForwarded(budget / 4)
	if _, err := tbl.Allocate("AAAAAA", 1); err != nil {
		t.Fatalf("still under the budget: %v", err)
	}

	for tbl.Snapshot().Egress < budget {
		tbl.countForwarded(512)
	}
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrEgressBudget) {
		t.Fatalf("past the budget Allocate must return ErrEgressBudget, got %v", err)
	}
	if got := tbl.Snapshot().AllocRefusedBudget; got != 1 {
		t.Fatalf("the refusal must be counted for the operator, got %d", got)
	}

	// The seats that were admitted before the budget ran out keep their handles
	// and can still re-request them.
	if _, err := tbl.Allocate("AAAAAA", 0); err != nil {
		t.Fatalf("an EXISTING seat must still re-allocate past the budget: %v", err)
	}
}

// A negative budget is the documented "disabled".
func TestNegativeLimitsDisableTheCaps(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{MaxAllocations: -1, EgressBudgetBytes: -1}, testLogger())
	tbl.countForwarded(1 << 20)
	for seat := 0; seat < 8; seat++ {
		if _, err := tbl.Allocate("AAAAAA", seat); err != nil {
			t.Fatalf("negative limits must disable the cap, seat %d: %v", seat, err)
		}
	}
}

// ---- THE ONE THAT MATTERS -----------------------------------------------------

// A match already forwarding when the budget runs out must keep forwarding,
// byte-identically, for as long as it lasts. This is the whole point of putting
// the cap in Allocate and nowhere else: an exhausted budget is a closed door,
// never a cut cable. Cutting a live match in half is a worse outcome than the
// bill, and a refactor that "helpfully" moves the check into Forward would pass
// every other test in this file and fail only this one.
//
// End-to-end over the real UDP listener rather than against Table.Forward, so
// the proof covers the actual serve() path an operator's traffic takes.
func TestExhaustedBudgetDoesNotBreakAMatchAlreadyForwarding(t *testing.T) {
	// Small enough that a handful of datagrams spends it.
	tbl := NewTable(time.Minute, Limits{EgressBudgetBytes: 200, MaxAllocations: 2}, testLogger())
	srv, err := Start("127.0.0.1:0", tbl, testLogger())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	allocA, err := tbl.Allocate("AAAAAA", 0)
	if err != nil {
		t.Fatalf("allocate seat 0: %v", err)
	}
	allocB, err := tbl.Allocate("AAAAAA", 1)
	if err != nil {
		t.Fatalf("allocate seat 1: %v", err)
	}

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

	// B speaks first so the relay learns its return path (this one is dropped —
	// A has not spoken yet), then the match is live in both directions.
	if _, err := sockB.Write(relayPacket(t, allocB, 0, []byte("hello"))); err != nil {
		t.Fatalf("B write: %v", err)
	}
	waitForCounter(t, func() uint64 { return tbl.noAddr.Load() }, 1)

	// Burn the budget with real forwarded traffic.
	payload := bytes.Repeat([]byte{0xAB}, 64)
	buf := make([]byte, 2048)
	for i := 0; i < 4; i++ {
		if _, err := sockA.Write(relayPacket(t, allocA, 1, payload)); err != nil {
			t.Fatalf("A write %d: %v", i, err)
		}
		_ = sockB.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := sockB.Read(buf); err != nil {
			t.Fatalf("B read %d: %v", i, err)
		}
	}
	// The listener owns the tallies, so wait for it to book all four rather than
	// reading them the instant the last datagram lands. `forwarded` is the commit
	// marker (countForwarded bumps it last), so this also settles `egress`.
	waitForCounter(t, func() uint64 { return tbl.forwarded.Load() }, 4)
	if egress := tbl.Snapshot().Egress; egress < 200 {
		t.Fatalf("precondition: the budget should be spent, egress=%d", egress)
	}

	// The door is now shut for anyone NEW...
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrEgressBudget) {
		t.Fatalf("a new lobby must be refused past the budget, got %v", err)
	}

	// ...and the running match is untouched. Both directions, payload verified
	// byte-for-byte, header still re-addressed correctly.
	forwardedBefore := tbl.Snapshot().Forwarded
	live := []byte{0x00, 0x01, 0xFF, 0x7F, 'l', 'i', 'v', 'e', 0x00}

	if _, err := sockA.Write(relayPacket(t, allocA, 1, live)); err != nil {
		t.Fatalf("A write after exhaustion: %v", err)
	}
	_ = sockB.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := sockB.Read(buf)
	if err != nil {
		t.Fatalf("the live match stopped forwarding A→B once the budget ran out: %v", err)
	}
	wantID, _ := hex.DecodeString(allocB)
	if !bytes.Equal(buf[:AllocIDLen], wantID) || buf[AllocIDLen] != 0 {
		t.Fatalf("A→B header should stay [allocB][seat 0], got %x seat=%d", buf[:AllocIDLen], buf[AllocIDLen])
	}
	if !bytes.Equal(buf[HeaderLen:n], live) {
		t.Fatalf("A→B payload must stay byte-identical: got % x want % x", buf[HeaderLen:n], live)
	}

	back := []byte("pong-after-the-budget")
	if _, err := sockB.Write(relayPacket(t, allocB, 0, back)); err != nil {
		t.Fatalf("B write after exhaustion: %v", err)
	}
	_ = sockA.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err = sockA.Read(buf)
	if err != nil {
		t.Fatalf("the live match stopped forwarding B→A once the budget ran out: %v", err)
	}
	wantID, _ = hex.DecodeString(allocA)
	if !bytes.Equal(buf[:AllocIDLen], wantID) || buf[AllocIDLen] != 1 {
		t.Fatalf("B→A header should stay [allocA][seat 1], got %x seat=%d", buf[:AllocIDLen], buf[AllocIDLen])
	}
	if !bytes.Equal(buf[HeaderLen:n], back) {
		t.Fatalf("B→A payload must stay byte-identical: got % x want % x", buf[HeaderLen:n], back)
	}

	waitForCounter(t, func() uint64 { return tbl.forwarded.Load() }, forwardedBefore+2)
	// And the overspend is visible rather than hidden: the budget bounds what
	// STARTS, so a live match is allowed to carry it past its ceiling and the
	// counter must keep saying so.
	if egress := tbl.Snapshot().Egress; egress <= 200 {
		t.Fatalf("post-budget forwarding must still be counted, egress=%d", egress)
	}
}

// The same property for the allocation ceiling: a full table does not stop the
// matches that filled it.
func TestFullAllocationTableDoesNotBreakAMatchAlreadyForwarding(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{MaxAllocations: 2}, testLogger())
	srv, err := Start("127.0.0.1:0", tbl, testLogger())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	allocA, _ := tbl.Allocate("AAAAAA", 0)
	allocB, _ := tbl.Allocate("AAAAAA", 1)
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrAllocationLimit) {
		t.Fatalf("precondition: the table should be full, got %v", err)
	}

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

	if _, err := sockB.Write(relayPacket(t, allocB, 0, []byte("hello"))); err != nil {
		t.Fatalf("B write: %v", err)
	}
	waitForCounter(t, func() uint64 { return tbl.noAddr.Load() }, 1)

	payload := []byte("still-playing")
	if _, err := sockA.Write(relayPacket(t, allocA, 1, payload)); err != nil {
		t.Fatalf("A write: %v", err)
	}
	buf := make([]byte, 2048)
	_ = sockB.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := sockB.Read(buf)
	if err != nil {
		t.Fatalf("a full allocation table stopped a live match forwarding: %v", err)
	}
	if !bytes.Equal(buf[HeaderLen:n], payload) {
		t.Fatalf("payload must stay byte-identical: % x", buf[HeaderLen:n])
	}
}

// Releasing a seat frees the row, so a server that filled up recovers on its own
// as matches end. Without this the ceiling would be a one-way latch.
func TestReleasingASeatFreesRoomUnderTheCap(t *testing.T) {
	tbl := NewTable(time.Minute, Limits{MaxAllocations: 2}, testLogger())
	if _, err := tbl.Allocate("AAAAAA", 0); err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.Allocate("AAAAAA", 1); err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.Allocate("BBBBBB", 0); !errors.Is(err, ErrAllocationLimit) {
		t.Fatalf("precondition: the table should be full, got %v", err)
	}

	tbl.ReleaseLobby("AAAAAA")
	if _, err := tbl.Allocate("BBBBBB", 0); err != nil {
		t.Fatalf("a freed row must be reusable: %v", err)
	}
}
