package main

import (
	"testing"
	"time"
)

// The relay seat-hijack regression suite (SECURITY.md "relay seat hijack").
//
// The alloc_id is the only thing identifying a sender on the data plane, and it
// travels in CLEARTEXT as the first 16 bytes of every relayed datagram — so on
// a relayed match, which is by definition one where the path is hostile, any
// observer can read it. Before the fix, forward() learned the return path from
// the datagram source UNCONDITIONALLY, so one datagram carrying a stolen
// alloc_id repointed that seat's inbound traffic at an address of the
// attacker's choosing.

// relayFixture builds a two-seat lobby with both seats allocated, on a frozen
// clock shared by the manager and the relay table.
type relayFixture struct {
	m      *Manager
	tbl    *relayTable
	allocA string
	allocB string
	now    time.Time
}

func newRelayFixture(t *testing.T) *relayFixture {
	t.Helper()
	f := &relayFixture{m: newTestManager(t), now: time.Now()}
	f.m.now = func() time.Time { return f.now }
	f.tbl = f.m.relay
	f.tbl.now = func() time.Time { return f.now }

	host := newFakeConn("host")
	lc := createLobby(t, f.m, host, nil)
	guest := join(f.m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	f.allocA = allocateRelay(t, f.m, host, nil).AllocID
	f.allocB = allocateRelay(t, f.m, guest, nil).AllocID
	return f
}

func TestRelayPinsTheLearnedAddressAgainstAStolenAllocID(t *testing.T) {
	f := newRelayFixture(t)
	victim := mustAddr(t, "203.0.113.7:41234")
	peer := mustAddr(t, "198.51.100.4:50000")
	attacker := mustAddr(t, "192.0.2.66:31337")

	// Both seats speak, so both return paths are learned and pinned.
	f.tbl.forward(victim, relayPacket(t, f.allocA, 1, []byte("a")))
	f.tbl.forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))

	// The attacker read seat 0's alloc_id off the wire and sends one datagram
	// from its own address. This is the exploit.
	out, dst, ok := f.tbl.forward(attacker, relayPacket(t, f.allocA, 1, []byte("evil")))
	if ok {
		t.Fatalf("a datagram from an unpinned source must be dropped, got %d bytes to %v", len(out), dst)
	}

	// The pin must not have moved: seat 1's traffic still reaches the VICTIM.
	_, dst, ok = f.tbl.forward(peer, relayPacket(t, f.allocB, 0, []byte("still yours")))
	if !ok {
		t.Fatal("the legitimate peer must still be able to reach seat 0")
	}
	if !sameUDPAddr(dst, victim) {
		t.Fatalf("seat 0's return path was hijacked: now %v, want %v", dst, victim)
	}
	if got := f.tbl.snapshot().rebindRefused; got != 1 {
		t.Fatalf("the refusal must be counted: rebind_refused=%d, want 1", got)
	}
}

// A real peer's NAT can remap mid-match, and that must still recover. What
// distinguishes it from a hijack is that the OLD mapping goes silent — the peer
// is not sending from it any more, because it cannot.
func TestRelayAcceptsARebindOnceThePinnedAddressGoesQuiet(t *testing.T) {
	f := newRelayFixture(t)
	oldMapping := mustAddr(t, "203.0.113.7:41234")
	newMapping := mustAddr(t, "203.0.113.7:52999") // same host, new NAT port
	peer := mustAddr(t, "198.51.100.4:50000")

	f.tbl.forward(oldMapping, relayPacket(t, f.allocA, 1, []byte("a")))
	f.tbl.forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))

	// Inside the quiet window the move is refused...
	f.now = f.now.Add(kRelayRebindQuiet - time.Millisecond)
	if _, _, ok := f.tbl.forward(newMapping, relayPacket(t, f.allocA, 1, []byte("a"))); ok {
		t.Fatal("a rebind must not be honoured while the pinned address is still live")
	}
	// ...and once the old mapping has been silent long enough, it is taken.
	f.now = f.now.Add(2 * time.Millisecond)
	if _, _, ok := f.tbl.forward(newMapping, relayPacket(t, f.allocA, 1, []byte("a"))); !ok {
		t.Fatal("a genuine NAT rebind must recover after the quiet window")
	}
	_, dst, ok := f.tbl.forward(peer, relayPacket(t, f.allocB, 0, []byte("b")))
	if !ok || !sameUDPAddr(dst, newMapping) {
		t.Fatalf("the return path should now be the new mapping, got %v", dst)
	}
	if got := f.tbl.snapshot().rebound; got != 1 {
		t.Fatalf("the accepted move must be counted: rebound=%d, want 1", got)
	}
}

// Catching one quiet window must not buy an unlimited number of moves.
func TestRelayThrottlesRepeatedRebinds(t *testing.T) {
	f := newRelayFixture(t)
	f.tbl.forward(mustAddr(t, "203.0.113.7:1000"), relayPacket(t, f.allocA, 1, []byte("a")))

	// Move as fast as the quiet window allows — the best an attacker who could
	// silence the victim at will could do. The bucket costs more per move than
	// that pace earns back, so it drains.
	const attempts = 20
	for i := 0; i < attempts; i++ {
		f.now = f.now.Add(kRelayRebindQuiet + time.Millisecond)
		src := mustAddr(t, "192.0.2.66:"+itoa(2000+i))
		f.tbl.forward(src, relayPacket(t, f.allocA, 1, []byte("x")))
		if f.tbl.snapshot().rebindThrottled > 0 {
			return
		}
	}
	t.Fatalf("the rebind budget must run out; %d back-to-back moves all succeeded", attempts)
}

// The first datagram wins, because before it there is nothing to protect: an
// on-path observer cannot know the alloc_id until it has seen one.
func TestRelayFirstDatagramLearnsTheAddress(t *testing.T) {
	f := newRelayFixture(t)
	a := mustAddr(t, "203.0.113.7:41234")
	b := mustAddr(t, "198.51.100.4:50000")
	f.tbl.forward(a, relayPacket(t, f.allocA, 1, []byte("a")))
	_, dst, ok := f.tbl.forward(b, relayPacket(t, f.allocB, 0, []byte("b")))
	if !ok || !sameUDPAddr(dst, a) {
		t.Fatalf("the first datagram from a seat must establish its return path, got %v ok=%v", dst, ok)
	}
}

// A stranger holding an alloc_id must not be able to keep a dead allocation
// alive past the idle reaper, so lastSeen only moves on an ACCEPTED datagram.
func TestRelayRefusedRebindDoesNotKeepTheAllocationAlive(t *testing.T) {
	f := newRelayFixture(t)
	f.tbl.idle = 30 * time.Second
	f.tbl.forward(mustAddr(t, "203.0.113.7:41234"), relayPacket(t, f.allocA, 1, []byte("a")))

	// The real peer goes away; a stranger keeps poking with its alloc_id.
	for i := 0; i < 10; i++ {
		f.now = f.now.Add(4 * time.Second)
		f.tbl.forward(mustAddr(t, "192.0.2.66:31337"), relayPacket(t, f.allocA, 1, []byte("x")))
	}
	if n := f.tbl.reapIdle(); n == 0 {
		t.Fatal("a stranger's traffic must not refresh an allocation it does not own")
	}
}

func TestRelayDropsSelfAddressedDatagrams(t *testing.T) {
	f := newRelayFixture(t)
	src := mustAddr(t, "203.0.113.7:41234")
	if _, _, ok := f.tbl.forward(src, relayPacket(t, f.allocA, 0, []byte("echo"))); ok {
		t.Fatal("a seat addressing itself would make the relay a 1:1 reflector")
	}
	if got := f.tbl.snapshot().selfAddressed; got != 1 {
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
