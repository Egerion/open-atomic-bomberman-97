package relay

import (
	"bytes"
	"encoding/hex"
	"net"
	"strconv"
	"testing"
	"time"

	"go.uber.org/zap"
)

// ---- helpers ----------------------------------------------------------------

// testLogger discards everything. zap's own no-op logger costs nothing per call,
// which matters in the forwarding suites that push thousands of datagrams.
func testLogger() *zap.Logger { return zap.NewNop() }

// relayPacket builds a client→relay datagram: [16B alloc_id][1B dst_seat][payload].
func relayPacket(t *testing.T, allocID string, dstSeat int, payload []byte) []byte {
	t.Helper()
	raw, err := hex.DecodeString(allocID)
	if err != nil || len(raw) != AllocIDLen {
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

// fixture is a two-seat allocation on a frozen clock — everything the data
// plane needs, with no lobby anywhere near it.
type fixture struct {
	tbl    *Table
	code   string
	allocA string
	allocB string
	now    time.Time
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	f := &fixture{code: "AAAAAA", now: time.Now()}
	f.tbl = NewTable(time.Minute, testLogger())
	f.tbl.now = func() time.Time { return f.now }
	var err error
	if f.allocA, err = f.tbl.Allocate(f.code, 0); err != nil {
		t.Fatalf("allocate seat 0: %v", err)
	}
	if f.allocB, err = f.tbl.Allocate(f.code, 1); err != nil {
		t.Fatalf("allocate seat 1: %v", err)
	}
	return f
}

// waitForCounter polls an atomic tally the listener goroutine owns, so a UDP
// test never races it.
func waitForCounter(t *testing.T, load func() uint64, want uint64) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if load() >= want {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("counter never reached %d (got %d)", want, load())
}

// ---- allocation --------------------------------------------------------------

func TestAllocateIsIdempotentPerSeat(t *testing.T) {
	f := newFixture(t)
	again, err := f.tbl.Allocate(f.code, 0)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if again != f.allocA {
		t.Fatalf("re-allocating a seat must return the same handle: %q vs %q", again, f.allocA)
	}
	if f.tbl.Size() != 2 {
		t.Fatalf("re-allocation must not add a row, %d rows", f.tbl.Size())
	}
}

func TestAllocIDIsThirtyTwoHexCharsAndUnique(t *testing.T) {
	f := newFixture(t)
	for _, id := range []string{f.allocA, f.allocB} {
		if len(id) != 2*AllocIDLen {
			t.Fatalf("alloc_id should be %d hex chars, got %q", 2*AllocIDLen, id)
		}
		if _, err := hex.DecodeString(id); err != nil {
			t.Fatalf("alloc_id must be lowercase hex: %q", id)
		}
	}
	if f.allocA == f.allocB {
		t.Fatal("two seats must get distinct handles")
	}
}

// ---- data plane: end-to-end forwarding ---------------------------------------

func TestRelayForwardsEndToEnd(t *testing.T) {
	tbl := NewTable(time.Minute, testLogger())
	srv, err := Start("127.0.0.1:0", tbl, testLogger())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	allocA, _ := tbl.Allocate("AAAAAA", 0)
	allocB, _ := tbl.Allocate("AAAAAA", 1)

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
	waitForCounter(t, func() uint64 { return tbl.noAddr.Load() }, 1)

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
	if n != HeaderLen+len(payload) {
		t.Fatalf("relayed datagram should be 17+%d bytes, got %d", len(payload), n)
	}
	// Header must be [B's OWN alloc_id][A's seat].
	wantID, _ := hex.DecodeString(allocB)
	if !bytes.Equal(got[:AllocIDLen], wantID) {
		t.Fatalf("header alloc_id should be the RECEIVER's (%s), got %x", allocB, got[:AllocIDLen])
	}
	if got[AllocIDLen] != 0 {
		t.Fatalf("header seat should be the SENDER's seat 0, got %d", got[AllocIDLen])
	}
	if !bytes.Equal(got[HeaderLen:], payload) {
		t.Fatalf("payload must be byte-identical: got % x want % x", got[HeaderLen:], payload)
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
	if !bytes.Equal(got[:AllocIDLen], wantID) || got[AllocIDLen] != 1 {
		t.Fatalf("return header should be [allocA][seat 1], got %x seat=%d", got[:AllocIDLen], got[AllocIDLen])
	}
	if !bytes.Equal(got[HeaderLen:], back) {
		t.Fatalf("return payload mismatch: % x", got[HeaderLen:])
	}
}

// A short datagram must not crash the listener or draw a reply.
func TestRelayIgnoresShortDatagramOverUDP(t *testing.T) {
	tbl := NewTable(time.Minute, testLogger())
	srv, err := Start("127.0.0.1:0", tbl, testLogger())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = srv.Close() }()

	conn, err := net.DialUDP("udp", nil, srv.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	if _, err := conn.Write(make([]byte, HeaderLen-1)); err != nil {
		t.Fatalf("write: %v", err)
	}
	waitForCounter(t, func() uint64 { return tbl.short.Load() }, 1)

	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if _, err := conn.Read(make([]byte, 256)); err == nil {
		t.Fatal("relay must never answer a malformed datagram")
	}
}

// One datagram must not be able to kill the listener. On Linux the kernel
// truncates an oversized datagram silently, but on Windows ReadFromUDP returns
// WSAEMSGSIZE — and the read loop used to `return` on any error, so a single
// 3 KB packet ended the forwarder (as would an ICMP port-unreachable from a
// peer that simply quit, which needs no attacker at all).
func TestRelayStillForwardsAfterAnOversizedDatagram(t *testing.T) {
	tbl := NewTable(time.Minute, testLogger())
	srv, err := Start("127.0.0.1:0", tbl, testLogger())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = srv.Close() }()
	relayAddr := srv.LocalAddr().(*net.UDPAddr)

	allocA, _ := tbl.Allocate("AAAAAA", 0)
	allocB, _ := tbl.Allocate("AAAAAA", 1)

	poison, err := net.DialUDP("udp", nil, relayAddr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	if _, err := poison.Write(make([]byte, MaxDatagram*2)); err != nil {
		t.Fatalf("write oversized: %v", err)
	}
	_ = poison.Close()

	peerA, _ := net.DialUDP("udp", nil, relayAddr)
	peerB, _ := net.DialUDP("udp", nil, relayAddr)
	defer func() { _ = peerA.Close(); _ = peerB.Close() }()

	buf := make([]byte, 2048)
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		_, _ = peerB.Write(relayPacket(t, allocB, 0, []byte("teach"))) // teach B's address
		_, _ = peerA.Write(relayPacket(t, allocA, 1, []byte("BOMB")))
		_ = peerB.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, err := peerB.Read(buf)
		if err != nil {
			continue
		}
		if n > HeaderLen && string(buf[HeaderLen:n]) == "BOMB" {
			return
		}
	}
	t.Fatal("the relay stopped forwarding after one oversized datagram")
}

// ---- data plane: drop paths (table level, deterministic) ---------------------

func TestRelayDropPaths(t *testing.T) {
	f := newFixture(t)
	tbl := f.tbl
	srcA := mustAddr(t, "203.0.113.7:41234")

	t.Run("short datagram", func(t *testing.T) {
		for _, n := range []int{0, 1, AllocIDLen, HeaderLen - 1} {
			if _, _, ok := tbl.Forward(srcA, make([]byte, n)); ok {
				t.Fatalf("%d-byte datagram must be dropped", n)
			}
		}
	})

	t.Run("unknown alloc_id", func(t *testing.T) {
		bogus := make([]byte, HeaderLen+4)
		bogus[0] = 0xDE
		if _, _, ok := tbl.Forward(srcA, bogus); ok {
			t.Fatal("unknown alloc_id must be dropped")
		}
	})

	t.Run("destination address not learned", func(t *testing.T) {
		// A speaks (learning srcA); B is allocated but silent, so A→B drops.
		if _, _, ok := tbl.Forward(srcA, relayPacket(t, f.allocA, 1, []byte("hi"))); ok {
			t.Fatal("must drop while the destination address is unknown")
		}
	})

	t.Run("unknown destination seat", func(t *testing.T) {
		// Seat 3 is in the lobby's seat range but holds no allocation.
		if _, _, ok := tbl.Forward(srcA, relayPacket(t, f.allocA, 3, []byte("hi"))); ok {
			t.Fatal("unallocated destination seat must be dropped")
		}
		// ...and so must a seat number outside the roster entirely.
		if _, _, ok := tbl.Forward(srcA, relayPacket(t, f.allocA, 255, []byte("hi"))); ok {
			t.Fatal("out-of-range destination seat must be dropped")
		}
	})

	t.Run("cross-lobby is impossible", func(t *testing.T) {
		// A second lobby's seat 1 must be unreachable from this lobby's seat 0:
		// the destination is looked up under the SENDER's lobby code.
		allocOther, err := tbl.Allocate("BBBBBB", 1)
		if err != nil {
			t.Fatalf("allocate: %v", err)
		}
		if allocOther == f.allocB {
			t.Fatal("distinct lobbies must get distinct alloc_ids")
		}
		srcOther := mustAddr(t, "198.51.100.9:5000")
		if _, _, ok := tbl.Forward(srcOther, relayPacket(t, allocOther, 0, []byte("x"))); ok {
			t.Fatal("lobby 2 seat 1 → seat 0 should drop: lobby 2 has no seat 0 allocation")
		}
		// And lobby 1's A still cannot reach it either.
		if _, _, ok := tbl.Forward(srcA, relayPacket(t, f.allocA, 1, []byte("x"))); ok {
			t.Fatal("lobby 1 seat 0 must not reach lobby 2's learned peer")
		}
	})

	// Every one of the above must have been counted, not silently discarded.
	c := tbl.Snapshot()
	if c.Forwarded != 0 {
		t.Fatalf("no datagram should have been forwarded, got %d", c.Forwarded)
	}
	if c.Short != 4 || c.UnknownAlloc != 1 || c.UnknownDst != 3 || c.NoAddr != 2 {
		t.Fatalf("drop tallies wrong: %+v", c)
	}
}

// ---- idle expiry -------------------------------------------------------------

func TestRelayIdleExpiry(t *testing.T) {
	now := time.Now()
	tbl := NewTable(60*time.Second, testLogger())
	tbl.now = func() time.Time { return now }

	idA, err := tbl.Allocate("AAAAAA", 0)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	if _, err := tbl.Allocate("AAAAAA", 1); err != nil {
		t.Fatalf("allocate: %v", err)
	}

	// Just short of the window: nothing expires.
	now = now.Add(59 * time.Second)
	if n := tbl.ReapIdle(); n != 0 {
		t.Fatalf("nothing should expire before the idle window, reaped %d", n)
	}

	// Seat 0 sends: its timer resets, seat 1's does not.
	tbl.Forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, idA, 9, []byte("x")))
	now = now.Add(2 * time.Second)
	if n := tbl.ReapIdle(); n != 1 {
		t.Fatalf("only the silent seat should expire, reaped %d", n)
	}
	if tbl.Size() != 1 {
		t.Fatalf("the active seat must survive, %d rows left", tbl.Size())
	}

	// Now let the active one go idle too.
	now = now.Add(61 * time.Second)
	if n := tbl.ReapIdle(); n != 1 {
		t.Fatalf("the idle seat should expire, reaped %d", n)
	}
	if tbl.Size() != 0 {
		t.Fatalf("table should be empty, %d rows left", tbl.Size())
	}
	// An expired handle no longer forwards.
	if _, _, ok := tbl.Forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, idA, 1, []byte("x"))); ok {
		t.Fatal("an expired alloc_id must be unknown")
	}
}

func TestRelayIdleZeroDisablesExpiry(t *testing.T) {
	now := time.Now()
	tbl := NewTable(0, testLogger())
	tbl.now = func() time.Time { return now }
	if _, err := tbl.Allocate("AAAAAA", 0); err != nil {
		t.Fatalf("allocate: %v", err)
	}
	now = now.Add(24 * time.Hour)
	if n := tbl.ReapIdle(); n != 0 {
		t.Fatalf("idle=0 must disable expiry, reaped %d", n)
	}
}

// ---- address pinning: the seat-hijack regression suite ------------------------
//
// The alloc_id is the only thing identifying a sender on the data plane, and it
// travels in CLEARTEXT as the first 16 bytes of every relayed datagram — so on a
// relayed match, which is by definition one where the path is hostile, any
// observer can read it. Before the fix, Forward learned the return path from the
// datagram source UNCONDITIONALLY, so one datagram carrying a stolen alloc_id
// repointed that seat's inbound traffic at an address of the attacker's
// choosing. SECURITY.md F1.

func TestRelayPinsTheLearnedAddressAgainstAStolenAllocID(t *testing.T) {
	f := newFixture(t)
	victim := mustAddr(t, "203.0.113.7:41234")
	peer := mustAddr(t, "198.51.100.4:50000")
	attacker := mustAddr(t, "192.0.2.66:31337")

	// Both seats speak, so both return paths are learned and pinned.
	f.tbl.Forward(victim, relayPacket(t, f.allocA, 1, []byte("a")))
	f.tbl.Forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))

	// The attacker read seat 0's alloc_id off the wire and sends one datagram
	// from its own address. This is the exploit.
	out, dst, ok := f.tbl.Forward(attacker, relayPacket(t, f.allocA, 1, []byte("evil")))
	if ok {
		t.Fatalf("a datagram from an unpinned source must be dropped, got %d bytes to %v", len(out), dst)
	}

	// The pin must not have moved: seat 1's traffic still reaches the VICTIM.
	_, dst, ok = f.tbl.Forward(peer, relayPacket(t, f.allocB, 0, []byte("still yours")))
	if !ok {
		t.Fatal("the legitimate peer must still be able to reach seat 0")
	}
	if !sameUDPAddr(dst, victim) {
		t.Fatalf("seat 0's return path was hijacked: now %v, want %v", dst, victim)
	}
	if got := f.tbl.Snapshot().RebindRefused; got != 1 {
		t.Fatalf("the refusal must be counted: rebind_refused=%d, want 1", got)
	}
}

// A real peer's NAT can remap mid-match, and that must still recover. What
// distinguishes it from a hijack is that the OLD mapping goes silent — the peer
// is not sending from it any more, because it cannot.
func TestRelayAcceptsARebindOnceThePinnedAddressGoesQuiet(t *testing.T) {
	f := newFixture(t)
	oldMapping := mustAddr(t, "203.0.113.7:41234")
	newMapping := mustAddr(t, "203.0.113.7:52999") // same host, new NAT port
	peer := mustAddr(t, "198.51.100.4:50000")

	f.tbl.Forward(oldMapping, relayPacket(t, f.allocA, 1, []byte("a")))
	f.tbl.Forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))

	// Inside the quiet window the move is refused...
	f.now = f.now.Add(RebindQuiet - time.Millisecond)
	if _, _, ok := f.tbl.Forward(newMapping, relayPacket(t, f.allocA, 1, []byte("a"))); ok {
		t.Fatal("a rebind must not be honoured while the pinned address is still live")
	}
	// ...and once the old mapping has been silent long enough, it is taken.
	f.now = f.now.Add(2 * time.Millisecond)
	if _, _, ok := f.tbl.Forward(newMapping, relayPacket(t, f.allocA, 1, []byte("a"))); !ok {
		t.Fatal("a genuine NAT rebind must recover after the quiet window")
	}
	_, dst, ok := f.tbl.Forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))
	if !ok || !sameUDPAddr(dst, newMapping) {
		t.Fatalf("the return path should now be the new mapping, got %v", dst)
	}
	if got := f.tbl.Snapshot().Rebound; got != 1 {
		t.Fatalf("the accepted move must be counted: rebound=%d, want 1", got)
	}
}

// Catching one quiet window must not buy an unlimited number of moves.
func TestRelayThrottlesRepeatedRebinds(t *testing.T) {
	f := newFixture(t)
	f.tbl.Forward(mustAddr(t, "203.0.113.7:1000"), relayPacket(t, f.allocA, 1, []byte("a")))

	// Move as fast as the quiet window allows — the best an attacker who could
	// silence the victim at will could do. The bucket costs more per move than
	// that pace earns back, so it drains.
	const attempts = 20
	for i := 0; i < attempts; i++ {
		f.now = f.now.Add(RebindQuiet + time.Millisecond)
		src := mustAddr(t, "192.0.2.66:"+strconv.Itoa(2000+i))
		f.tbl.Forward(src, relayPacket(t, f.allocA, 1, []byte("x")))
		if f.tbl.Snapshot().RebindThrottled > 0 {
			return
		}
	}
	t.Fatalf("the rebind budget must run out; %d back-to-back moves all succeeded", attempts)
}

// The first datagram wins, because before it there is nothing to protect: an
// on-path observer cannot know the alloc_id until it has seen one.
func TestRelayFirstDatagramLearnsTheAddress(t *testing.T) {
	f := newFixture(t)
	a := mustAddr(t, "203.0.113.7:41234")
	b := mustAddr(t, "198.51.100.4:50000")
	f.tbl.Forward(a, relayPacket(t, f.allocA, 1, []byte("a")))
	_, dst, ok := f.tbl.Forward(b, relayPacket(t, f.allocB, 0, []byte("b")))
	if !ok || !sameUDPAddr(dst, a) {
		t.Fatalf("the first datagram from a seat must establish its return path, got %v ok=%v", dst, ok)
	}
}

// A stranger holding an alloc_id must not be able to keep a dead allocation
// alive past the idle reaper, so lastSeen only moves on an ACCEPTED datagram.
func TestRelayRefusedRebindDoesNotKeepTheAllocationAlive(t *testing.T) {
	f := newFixture(t)
	f.tbl.idle = 30 * time.Second
	f.tbl.Forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, f.allocA, 1, []byte("a")))

	// The real peer goes away; a stranger keeps poking with its alloc_id.
	for i := 0; i < 10; i++ {
		f.now = f.now.Add(4 * time.Second)
		f.tbl.Forward(mustAddr(t, "192.0.2.66:31337"), relayPacket(t, f.allocA, 1, []byte("x")))
	}
	if n := f.tbl.ReapIdle(); n == 0 {
		t.Fatal("a stranger's traffic must not refresh an allocation it does not own")
	}
}

func TestRelayDropsSelfAddressedDatagrams(t *testing.T) {
	f := newFixture(t)
	src := mustAddr(t, "203.0.113.7:41234")
	if _, _, ok := f.tbl.Forward(src, relayPacket(t, f.allocA, 0, []byte("echo"))); ok {
		t.Fatal("a seat addressing itself would make the relay a 1:1 reflector")
	}
	if got := f.tbl.Snapshot().SelfAddressed; got != 1 {
		t.Fatalf("self-addressed drop must be counted, got %d", got)
	}
}

func TestSameUDPAddrHandlesV4MappedForms(t *testing.T) {
	a := mustAddr(t, "203.0.113.7:41234")
	b := mustAddr(t, "203.0.113.7:41234")
	if !sameUDPAddr(a, b) {
		t.Fatal("the same endpoint resolved twice must compare equal")
	}
	if sameUDPAddr(a, mustAddr(t, "203.0.113.7:41235")) {
		t.Fatal("a different port is a different endpoint")
	}
	if sameUDPAddr(a, nil) || sameUDPAddr(nil, a) {
		t.Fatal("nil is not an endpoint")
	}
}

// The pin must survive the read buffer being reused for the next datagram.
func TestCloneUDPAddrCopiesTheIP(t *testing.T) {
	src := mustAddr(t, "203.0.113.7:41234")
	c := cloneUDPAddr(src)
	src.IP[len(src.IP)-1] = 99
	if !sameUDPAddr(c, mustAddr(t, "203.0.113.7:41234")) {
		t.Fatal("the pinned address must not alias the caller's buffer")
	}
}

// LearnedAddr distinguishes "no such seat" from "allocated but silent" — the
// control plane's only read into the pin.
func TestLearnedAddrReportsAllocationAndPinSeparately(t *testing.T) {
	f := newFixture(t)
	if _, ok := f.tbl.LearnedAddr(f.code, 7); ok {
		t.Fatal("an unallocated seat must report ok=false")
	}
	addr, ok := f.tbl.LearnedAddr(f.code, 0)
	if !ok || addr != nil {
		t.Fatalf("an allocated but silent seat must be ok with a nil address, got %v ok=%v", addr, ok)
	}
	src := mustAddr(t, "203.0.113.7:41234")
	f.tbl.Forward(src, relayPacket(t, f.allocA, 9, []byte("x"))) // learns, then drops
	if addr, ok = f.tbl.LearnedAddr(f.code, 0); !ok || !sameUDPAddr(addr, src) {
		t.Fatalf("the pin should be readable after the first datagram, got %v", addr)
	}
}
