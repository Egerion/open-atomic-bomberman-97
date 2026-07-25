package main

import (
	"encoding/hex"
	"errors"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// UDP relay forwarder — the TURN-like fallback for peers whose hole-punch fails
// (symmetric NAT / CGNAT). ADR-0011 decision 3, design §4, PROTOCOL.md §6.
//
// HARD INVARIANT (ADR-0011): the server never simulates and never decodes a
// game datagram. Everything past the fixed 17-byte header is OPAQUE to this
// file — it forwards, it does not inspect. Nor does it add reliability,
// ordering, or rate shaping: the game's own netcode is loss-tolerant and this
// must stay a dumb forwarder.
//
// Control plane (WebSocket):
//
//	C→S  {"type":"AllocateRelay","lobby_id":"…","seat":1}
//	S→C  {"type":"RelayAllocated","relay_addr":"host:port","alloc_id":"<32 hex>"}
//
// Data plane (UDP, this file). Every datagram, BOTH directions, is
//
//	[16 bytes alloc_id (binary)][1 byte seat][opaque payload …]
//
//	C→S: alloc_id is the SENDER's allocation; the seat byte is the DESTINATION.
//	S→C: alloc_id is the RECEIVER's own allocation; the seat byte is the SENDER.
//
// So the header length is fixed in both directions — a client strips 17 bytes
// and learns who sent it from the seat byte. The relay learns each peer's public
// address off the datagram source (the same "read it from the wire" trick as
// stun.go); that learned address is what makes the return path work through a
// symmetric NAT, since no peer can predict it.
//
// The client side is a plain Transport swap decided by the Rendezvous outcome
// (design §4); the RollbackSession above it is unchanged.

const (
	// kAllocIDLen is the binary alloc_id width; the control plane renders the
	// same 128 bits as 32 lowercase hex chars (like lobby_id / host_token).
	kAllocIDLen = 16

	// kRelayHeaderLen is the fixed prefix both directions carry.
	kRelayHeaderLen = kAllocIDLen + 1

	// kRelayMaxDatagram caps a relayed datagram. Game frames are tiny (a per-tick
	// input frame is tens of bytes); this is generous headroom that still bounds
	// the read buffer. Oversized datagrams are DROPPED and counted, never
	// silently truncated (see relayServer.serve).
	kRelayMaxDatagram = 2048

	// kRelayMaintainInterval is how often idle allocations are reaped and the
	// aggregated counters are logged.
	kRelayMaintainInterval = 10 * time.Second

	// kRelayRebindQuiet is the address-pinning window (SECURITY.md "relay seat
	// hijack"). Once a seat's address is learned it is PINNED: a datagram
	// carrying that alloc_id from any other source is dropped, and the pin only
	// moves after the pinned address itself has been silent this long. A live
	// seat sends ~20 datagrams/second, so the pin is continuously re-asserted
	// while the real peer is on the air.
	kRelayRebindQuiet = 5 * time.Second

	// A genuine NAT rebind is a rare event; a hijack attempt retries. One
	// accepted move per 10 s with 3 banked keeps the rare case working and
	// stops the pin from being flapped.
	kRelayRebindCostMs = 10000
	kRelayRebindBurst  = 3

	// kRelayIngressSlots / cost / burst gate the UDP listener per source address
	// BEFORE the allocation table is touched, so a flood cannot contend the
	// table's mutex with live matches. 250 datagrams/s is an order of magnitude
	// above a 20 Hz seat (and loopback/private sources are exempt entirely, so
	// LAN and same-host play are never shaped).
	kRelayIngressSlots  = 4096
	kRelayIngressCostMs = 4
	kRelayIngressBurst  = 500
)

// allocKey pins an allocation to exactly one (lobby, seat) pair.
type allocKey struct {
	code string
	seat int
}

// allocation is one seat's relay reservation.
type allocation struct {
	id  [kAllocIDLen]byte
	key allocKey
	// addr is the seat's PINNED return path: learned off its first datagram and
	// thereafter only moved under the rules in forward(). nil until learned.
	addr *net.UDPAddr
	// addrSeen is when a datagram was last accepted FROM the pinned address —
	// the "is the real peer still on the air?" clock the rebind rule reads. It
	// is deliberately separate from lastSeen, which also moves on a legitimate
	// rebind, so a rebind cannot reset its own quiet window.
	addrSeen time.Time
	// rebind bounds how often the pin may move, so a stranger who catches one
	// quiet window cannot then flap the seat.
	rebind tokenBucket
	// lastSeen is refreshed by allocation and by ACCEPTED traffic from this
	// seat. A seat that stops sending expires even if a peer keeps aiming at it,
	// so a dead client cannot pin an entry (every live seat sends ~20
	// datagrams/second) — and neither can a stranger who knows the alloc_id.
	lastSeen time.Time
}

// relayCounters is one snapshot of the aggregated data-plane tallies.
type relayCounters struct {
	forwarded       uint64
	bytes           uint64
	short           uint64
	unknownAlloc    uint64
	unknownDst      uint64
	noAddr          uint64
	selfAddressed   uint64
	rebindRefused   uint64
	rebindThrottled uint64
	rebound         uint64
	oversize        uint64
	rateLimited     uint64
	readErr         uint64
	writeErr        uint64
}

// relayTable is the allocation registry: minted by the control plane
// (Manager.handleAllocateRelay), read and address-learned by the data plane
// (relayServer.serve). It owns its own mutex and never reaches back into the
// Manager, so the only lock order that ever occurs is Manager.mu → relayTable.mu.
//
// Deliberately socket-free: the whole forwarding decision is unit-testable.
type relayTable struct {
	mu    sync.Mutex
	byID  map[[kAllocIDLen]byte]*allocation
	byKey map[allocKey]*allocation

	idle        time.Duration    // 0 disables idle expiry
	rebindQuiet time.Duration    // pin window; 0 disables pinning (tests only)
	now         func() time.Time // injectable clock (tests)
	log         *slog.Logger

	forwarded       atomic.Uint64
	bytes           atomic.Uint64
	short           atomic.Uint64
	unknownAlloc    atomic.Uint64
	unknownDst      atomic.Uint64
	noAddr          atomic.Uint64
	selfAddressed   atomic.Uint64
	rebindRefused   atomic.Uint64
	rebindThrottled atomic.Uint64
	rebound         atomic.Uint64
	oversize        atomic.Uint64
	rateLimited     atomic.Uint64
	readErr         atomic.Uint64
	writeErr        atomic.Uint64

	lastStats relayCounters // maintain goroutine only
}

func newRelayTable(idle time.Duration, log *slog.Logger) *relayTable {
	return &relayTable{
		byID:        map[[kAllocIDLen]byte]*allocation{},
		byKey:       map[allocKey]*allocation{},
		idle:        idle,
		rebindQuiet: kRelayRebindQuiet,
		now:         time.Now,
		log:         log,
	}
}

// allocate reserves (or re-returns) the relay handle for one seat and gives back
// its 32-hex-char control-plane form. It is IDEMPOTENT per (lobby, seat): a
// client that retries after a lost reply gets the SAME alloc_id back and keeps
// its already-learned address instead of orphaning the old entry.
func (t *relayTable) allocate(code string, seat int) (string, error) {
	key := allocKey{code: code, seat: seat}

	t.mu.Lock()
	defer t.mu.Unlock()
	if a, ok := t.byKey[key]; ok {
		a.lastSeen = t.now()
		return hex.EncodeToString(a.id[:]), nil
	}

	// Same 128-bit crypto/rand generator as lobby_id / host_token; decoding its
	// hex form here keeps the two representations provably identical.
	handle, err := newHandle()
	if err != nil {
		return "", err
	}
	raw, err := hex.DecodeString(handle)
	if err != nil || len(raw) != kAllocIDLen {
		return "", err
	}
	a := &allocation{
		key:      key,
		lastSeen: t.now(),
		rebind:   newTokenBucket(kRelayRebindCostMs, kRelayRebindBurst),
	}
	copy(a.id[:], raw)
	t.byID[a.id] = a
	t.byKey[key] = a
	return handle, nil
}

// release frees one seat's allocation (member disconnect / heartbeat timeout).
func (t *relayTable) release(code string, seat int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.releaseLocked(allocKey{code: code, seat: seat})
}

// releaseLobby frees every allocation of a lobby (eviction).
func (t *relayTable) releaseLobby(code string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	for key := range t.byKey {
		if key.code == code {
			t.releaseLocked(key)
		}
	}
}

func (t *relayTable) releaseLocked(key allocKey) {
	if a, ok := t.byKey[key]; ok {
		delete(t.byKey, key)
		delete(t.byID, a.id)
	}
}

// forward is the entire data plane: parse the fixed header, authenticate the
// sender's address against the seat's pin, and re-address the OPAQUE payload at
// the destination seat. The payload is never examined.
//
// Returns ok=false for every drop path. Callers must stay silent on a drop —
// this parses untrusted input, so it may never panic and may never log per
// datagram (the tallies are aggregated by logStats instead).
//
// ADDRESS PINNING (SECURITY.md). The alloc_id is the only thing identifying a
// sender here, and it travels in CLEARTEXT as the first 16 bytes of every
// relayed datagram — a relayed match is by definition one where the path is
// hostile, so "on-path observer knows the alloc_id" is inside the threat model,
// not outside it. Learning the return path unconditionally therefore meant one
// datagram from anybody who had read a header could repoint a seat's inbound
// traffic at an address of their choosing. Instead:
//
//	first datagram      → learn the address and PIN it
//	pinned source       → forward, and re-assert the pin
//	different source, pinned address still live (spoke within rebindQuiet)
//	                    → DROP; the pin does not move
//	different source, pinned address quiet ≥ rebindQuiet, budget left
//	                    → move the pin (a real NAT rebind lands here)
//
// A live seat sends ~20 datagrams/second, so the pin is re-asserted
// continuously while the real peer is on the air and the window in which it can
// be taken never opens. What this does NOT close is written up in SECURITY.md:
// an attacker who can also silence the victim (a full on-path MITM, which owns
// the traffic anyway) can manufacture the quiet window.
func (t *relayTable) forward(src *net.UDPAddr, pkt []byte) ([]byte, *net.UDPAddr, bool) {
	if len(pkt) < kRelayHeaderLen {
		t.short.Add(1)
		return nil, nil, false
	}
	var id [kAllocIDLen]byte
	copy(id[:], pkt[:kAllocIDLen])
	dstSeat := int(pkt[kAllocIDLen])
	payload := pkt[kRelayHeaderLen:]

	t.mu.Lock()
	defer t.mu.Unlock()

	from, ok := t.byID[id]
	if !ok {
		t.unknownAlloc.Add(1)
		return nil, nil, false
	}
	// A seat addressing itself has no legitimate meaning and would turn the
	// forwarder into a 1:1 reflector for its own sender.
	if dstSeat == from.key.seat {
		t.selfAddressed.Add(1)
		return nil, nil, false
	}
	now := t.now()
	switch {
	case from.addr == nil:
		from.addr = cloneUDPAddr(src)
		from.addrSeen = now
	case sameUDPAddr(from.addr, src):
		from.addrSeen = now
	case t.rebindQuiet > 0 && now.Sub(from.addrSeen) < t.rebindQuiet:
		// The pinned peer is still on the air; this is somebody else holding a
		// header they read off the wire. Drop it whole — forwarding it would
		// also inject a stranger's bytes into the victim's match.
		t.rebindRefused.Add(1)
		return nil, nil, false
	case !from.rebind.allow(now):
		t.rebindThrottled.Add(1)
		return nil, nil, false
	default:
		from.addr = cloneUDPAddr(src)
		from.addrSeen = now
		t.rebound.Add(1)
	}
	from.lastSeen = now

	to, ok := t.byKey[allocKey{code: from.key.code, seat: dstSeat}]
	if !ok {
		t.unknownDst.Add(1)
		return nil, nil, false
	}
	if to.addr == nil {
		t.noAddr.Add(1) // destination has not spoken yet — nothing to aim at
		return nil, nil, false
	}

	out := make([]byte, kRelayHeaderLen+len(payload))
	copy(out[:kAllocIDLen], to.id[:])      // the RECEIVER's own alloc_id
	out[kAllocIDLen] = byte(from.key.seat) // the SENDER's seat
	copy(out[kRelayHeaderLen:], payload)
	return out, to.addr, true
}

// sameUDPAddr compares two UDP endpoints by value. net.UDPAddr is a struct with
// a slice in it, so == is not usable and IP.Equal is what handles the
// 4-byte/16-byte representations of the same v4 address.
func sameUDPAddr(a, b *net.UDPAddr) bool {
	if a == nil || b == nil {
		return a == b
	}
	return a.Port == b.Port && a.Zone == b.Zone && a.IP.Equal(b.IP)
}

// cloneUDPAddr copies an address out of the read path. ReadFromUDP hands back a
// fresh struct per call today, but the pin outlives the datagram and must not
// depend on that.
func cloneUDPAddr(a *net.UDPAddr) *net.UDPAddr {
	if a == nil {
		return nil
	}
	out := &net.UDPAddr{Port: a.Port, Zone: a.Zone}
	out.IP = append(net.IP(nil), a.IP...)
	return out
}

// reapIdle drops allocations with no traffic for the idle window, so the table
// cannot grow unbounded when a client vanishes without a clean disconnect.
func (t *relayTable) reapIdle() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.idle <= 0 {
		return 0
	}
	now := t.now()
	n := 0
	for key, a := range t.byKey {
		if now.Sub(a.lastSeen) > t.idle {
			t.releaseLocked(key)
			n++
		}
	}
	return n
}

func (t *relayTable) size() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.byKey)
}

func (t *relayTable) snapshot() relayCounters {
	return relayCounters{
		forwarded:       t.forwarded.Load(),
		bytes:           t.bytes.Load(),
		short:           t.short.Load(),
		unknownAlloc:    t.unknownAlloc.Load(),
		unknownDst:      t.unknownDst.Load(),
		noAddr:          t.noAddr.Load(),
		selfAddressed:   t.selfAddressed.Load(),
		rebindRefused:   t.rebindRefused.Load(),
		rebindThrottled: t.rebindThrottled.Load(),
		rebound:         t.rebound.Load(),
		oversize:        t.oversize.Load(),
		rateLimited:     t.rateLimited.Load(),
		readErr:         t.readErr.Load(),
		writeErr:        t.writeErr.Load(),
	}
}

// logStats emits ONE aggregated line per maintenance tick rather than a line per
// dropped datagram — untrusted input must not be able to flood the log. Nothing
// is dropped silently in the sense that matters: every drop reason is counted
// and reported here, with oversize (the only truncation-shaped failure) raised
// to WARN because it means a client exceeded kRelayMaxDatagram.
func (t *relayTable) logStats() {
	cur := t.snapshot()
	if cur == t.lastStats {
		return
	}
	if cur.oversize > t.lastStats.oversize {
		t.log.Warn("relay dropped oversized datagrams (not truncated)",
			"limit_bytes", kRelayMaxDatagram, "dropped", cur.oversize-t.lastStats.oversize)
	}
	// A refused rebind is the signature of the seat-hijack attempt (SECURITY.md)
	// — somebody sending a valid alloc_id from the wrong address while the real
	// peer is still on the air. Raised to WARN because, unlike the other drops,
	// it is never something a well-behaved client produces.
	if cur.rebindRefused > t.lastStats.rebindRefused {
		t.log.Warn("relay refused address rebinds (pinned peer still live)",
			"count", cur.rebindRefused-t.lastStats.rebindRefused, "quiet", t.rebindQuiet)
	}
	t.log.Info("relay stats",
		"allocations", t.size(),
		"forwarded", cur.forwarded, "bytes", cur.bytes,
		"drop_short", cur.short, "drop_unknown_alloc", cur.unknownAlloc,
		"drop_unknown_dst", cur.unknownDst, "drop_dst_addr_unknown", cur.noAddr,
		"drop_self_addressed", cur.selfAddressed,
		"drop_rebind_refused", cur.rebindRefused,
		"drop_rebind_throttled", cur.rebindThrottled, "rebound", cur.rebound,
		"drop_oversize", cur.oversize, "drop_rate_limited", cur.rateLimited,
		"read_err", cur.readErr, "drop_write_err", cur.writeErr)
	t.lastStats = cur
}

// relayServer is the UDP listener wrapped around a relayTable.
type relayServer struct {
	conn      *net.UDPConn
	table     *relayTable
	ingress   *ipBuckets // per-source gate in front of the table
	log       *slog.Logger
	done      chan struct{}
	closeOnce sync.Once
}

func startRelay(addr string, table *relayTable, log *slog.Logger) (*relayServer, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	s := &relayServer{
		conn:    conn,
		table:   table,
		ingress: newIPBuckets(kRelayIngressSlots, kRelayIngressCostMs, kRelayIngressBurst),
		log:     log,
		done:    make(chan struct{}),
	}
	go s.serve()
	go s.maintain()
	return s, nil
}

func (s *relayServer) serve() {
	// One byte of slack so an oversized datagram is DETECTED (n > max) instead of
	// being silently truncated by the kernel copy into an exact-sized buffer.
	buf := make([]byte, kRelayMaxDatagram+1)
	errs := 0
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			// ONLY a closed socket ends the loop. Every other error here is
			// per-datagram and attacker-triggerable: Windows reports an
			// oversized datagram as WSAEMSGSIZE and an ICMP port-unreachable
			// from an earlier forward as WSAECONNRESET, rather than truncating
			// or ignoring the way Linux does. Returning on those made one
			// datagram enough to take the forwarder down for good — and the
			// second of those needs no attacker at all, just a peer that quit.
			if errors.Is(err, net.ErrClosed) || errs >= kUDPMaxConsecutiveErrs {
				return
			}
			errs++
			s.table.readErr.Add(1)
			continue
		}
		errs = 0
		if n > kRelayMaxDatagram {
			s.table.oversize.Add(1)
			continue
		}
		// Gate per source BEFORE the table lock, so a flood cannot contend the
		// allocation mutex with live matches. Loopback/private sources are
		// exempt (perIPKey returns ""), so LAN and same-host play are never
		// shaped — and a public seat's 20 Hz is two orders under the ceiling.
		if key := perIPKey(src.String()); key != "" && !s.ingress.allow(key) {
			s.table.rateLimited.Add(1)
			continue
		}
		out, dst, ok := s.table.forward(src, buf[:n])
		if !ok {
			continue
		}
		if _, err := s.conn.WriteToUDP(out, dst); err != nil {
			s.table.writeErr.Add(1)
			continue
		}
		s.table.forwarded.Add(1)
		s.table.bytes.Add(uint64(len(out)))
	}
}

func (s *relayServer) maintain() {
	tk := time.NewTicker(kRelayMaintainInterval)
	defer tk.Stop()
	for {
		select {
		case <-s.done:
			return
		case <-tk.C:
			if n := s.table.reapIdle(); n > 0 {
				s.log.Info("relay allocations expired (idle)", "count", n, "idle", s.table.idle)
			}
			s.table.logStats()
		}
	}
}

func (s *relayServer) LocalAddr() net.Addr { return s.conn.LocalAddr() }

func (s *relayServer) Close() error {
	s.closeOnce.Do(func() { close(s.done) })
	return s.conn.Close()
}
