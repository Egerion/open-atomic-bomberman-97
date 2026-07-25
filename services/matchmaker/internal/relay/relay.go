// Package relay is the UDP relay forwarder — the TURN-like fallback for peers
// whose hole-punch fails (symmetric NAT / CGNAT). ADR-0011 decision 3, design
// §4, PROTOCOL.md §6.
//
// It owns the allocation table and the listener, and nothing else: it does not
// know what a lobby is beyond an opaque code, and it never reaches back into the
// package that mints its allocations. The dependency runs one way, lobby →
// relay, so the only lock order that can occur is Manager.mu → Table.mu.
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
// the stun package); that learned address is what makes the return path work
// through a symmetric NAT, since no peer can predict it.
//
// The client side is a plain Transport swap decided by the Rendezvous outcome
// (design §4); the RollbackSession above it is unchanged.
package relay

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/ratelimit"
)

const (
	// AllocIDLen is the binary alloc_id width; the control plane renders the
	// same 128 bits as 32 lowercase hex chars (like lobby_id / host_token).
	AllocIDLen = 16

	// HeaderLen is the fixed prefix both directions carry.
	HeaderLen = AllocIDLen + 1

	// MaxDatagram caps a relayed datagram. Game frames are tiny (a per-tick
	// input frame is tens of bytes); this is generous headroom that still bounds
	// the read buffer. Oversized datagrams are DROPPED and counted, never
	// silently truncated (see Server.serve).
	MaxDatagram = 2048

	// kMaintainInterval is how often idle allocations are reaped and the
	// aggregated counters are logged.
	kMaintainInterval = 10 * time.Second

	// RebindQuiet is the address-pinning window (SECURITY.md "relay seat
	// hijack"). Once a seat's address is learned it is PINNED: a datagram
	// carrying that alloc_id from any other source is dropped, and the pin only
	// moves after the pinned address itself has been silent this long. A live
	// seat sends ~20 datagrams/second, so the pin is continuously re-asserted
	// while the real peer is on the air.
	RebindQuiet = 5 * time.Second

	// A genuine NAT rebind is a rare event; a hijack attempt retries. One
	// accepted move per 10 s with 3 banked keeps the rare case working and
	// stops the pin from being flapped.
	kRebindCostMs = 10000
	RebindBurst   = 3

	// kIngressSlots / cost / burst gate the UDP listener per source address
	// BEFORE the allocation table is touched, so a flood cannot contend the
	// table's mutex with live matches. 250 datagrams/s is an order of magnitude
	// above a 20 Hz seat (and loopback/private sources are exempt entirely, so
	// LAN and same-host play are never shaped).
	kIngressSlots  = 4096
	kIngressCostMs = 4
	kIngressBurst  = 500

	// kMaxConsecutiveReadErrs stops a genuinely broken socket from spinning the
	// read loop hot, without letting one bad datagram end it (see Server.serve).
	kMaxConsecutiveReadErrs = 64
)

// allocKey pins an allocation to exactly one (lobby, seat) pair.
type allocKey struct {
	code string
	seat int
}

// allocation is one seat's relay reservation.
type allocation struct {
	id  [AllocIDLen]byte
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
	rebind ratelimit.Bucket
	// lastSeen is refreshed by allocation and by ACCEPTED traffic from this
	// seat. A seat that stops sending expires even if a peer keeps aiming at it,
	// so a dead client cannot pin an entry (every live seat sends ~20
	// datagrams/second) — and neither can a stranger who knows the alloc_id.
	lastSeen time.Time
}

// Counters is one snapshot of the aggregated data-plane tallies.
type Counters struct {
	Forwarded       uint64
	Bytes           uint64
	Short           uint64
	UnknownAlloc    uint64
	UnknownDst      uint64
	NoAddr          uint64
	SelfAddressed   uint64
	RebindRefused   uint64
	RebindThrottled uint64
	Rebound         uint64
	Oversize        uint64
	RateLimited     uint64
	ReadErr         uint64
	WriteErr        uint64
}

// Table is the allocation registry: minted by the control plane
// (Manager.handleAllocateRelay), read and address-learned by the data plane
// (Server.serve). It owns its own mutex and never reaches back into the
// Manager, so the only lock order that ever occurs is Manager.mu → Table.mu.
//
// Deliberately socket-free: the whole forwarding decision is unit-testable.
type Table struct {
	mu    sync.Mutex
	byID  map[[AllocIDLen]byte]*allocation
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

	lastStats Counters // maintain goroutine only
}

func NewTable(idle time.Duration, log *slog.Logger) *Table {
	return &Table{
		byID:        map[[AllocIDLen]byte]*allocation{},
		byKey:       map[allocKey]*allocation{},
		idle:        idle,
		rebindQuiet: RebindQuiet,
		now:         time.Now,
		log:         log,
	}
}

// Allocate reserves (or re-returns) the relay handle for one seat and gives back
// its 32-hex-char control-plane form. It is IDEMPOTENT per (lobby, seat): a
// client that retries after a lost reply gets the SAME alloc_id back and keeps
// its already-learned address instead of orphaning the old entry.
func (t *Table) Allocate(code string, seat int) (string, error) {
	key := allocKey{code: code, seat: seat}

	t.mu.Lock()
	defer t.mu.Unlock()
	if a, ok := t.byKey[key]; ok {
		a.lastSeen = t.now()
		return hex.EncodeToString(a.id[:]), nil
	}

	a := &allocation{
		key:      key,
		lastSeen: t.now(),
		rebind:   ratelimit.NewBucket(kRebindCostMs, RebindBurst),
	}
	// The same 128 bits of crypto/rand behind lobby_id and host_token, drawn
	// straight into the binary form the data plane compares and hex-encoded for
	// the control plane — the two representations are the same bytes by
	// construction rather than by a round trip through the encoder.
	if _, err := rand.Read(a.id[:]); err != nil {
		return "", err
	}
	t.byID[a.id] = a
	t.byKey[key] = a
	return hex.EncodeToString(a.id[:]), nil
}

// Release frees one seat's allocation (member disconnect / heartbeat timeout).
func (t *Table) Release(code string, seat int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.releaseLocked(allocKey{code: code, seat: seat})
}

// releaseLobby frees every allocation of a lobby (eviction).
func (t *Table) ReleaseLobby(code string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	for key := range t.byKey {
		if key.code == code {
			t.releaseLocked(key)
		}
	}
}

func (t *Table) releaseLocked(key allocKey) {
	if a, ok := t.byKey[key]; ok {
		delete(t.byKey, key)
		delete(t.byID, a.id)
	}
}

// Forward is the entire data plane: parse the fixed header, authenticate the
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
func (t *Table) Forward(src *net.UDPAddr, pkt []byte) ([]byte, *net.UDPAddr, bool) {
	if len(pkt) < HeaderLen {
		t.short.Add(1)
		return nil, nil, false
	}
	var id [AllocIDLen]byte
	copy(id[:], pkt[:AllocIDLen])
	dstSeat := int(pkt[AllocIDLen])
	payload := pkt[HeaderLen:]

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
	case !from.rebind.Allow(now):
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

	out := make([]byte, HeaderLen+len(payload))
	copy(out[:AllocIDLen], to.id[:])      // the RECEIVER's own alloc_id
	out[AllocIDLen] = byte(from.key.seat) // the SENDER's seat
	copy(out[HeaderLen:], payload)
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

// ReapIdle drops allocations with no traffic for the idle window, so the table
// cannot grow unbounded when a client vanishes without a clean disconnect.
func (t *Table) ReapIdle() int {
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

// LearnedAddr reports one seat's pinned return path. ok is false when the seat
// holds no allocation at all; a nil address with ok=true means the seat is
// allocated but has never spoken. Read-only — the pin is only ever moved by
// Forward, under the rules documented there.
func (t *Table) LearnedAddr(code string, seat int) (addr *net.UDPAddr, ok bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	a, ok := t.byKey[allocKey{code: code, seat: seat}]
	if !ok {
		return nil, false
	}
	return a.addr, true
}

func (t *Table) Size() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.byKey)
}

func (t *Table) Snapshot() Counters {
	return Counters{
		Forwarded:       t.forwarded.Load(),
		Bytes:           t.bytes.Load(),
		Short:           t.short.Load(),
		UnknownAlloc:    t.unknownAlloc.Load(),
		UnknownDst:      t.unknownDst.Load(),
		NoAddr:          t.noAddr.Load(),
		SelfAddressed:   t.selfAddressed.Load(),
		RebindRefused:   t.rebindRefused.Load(),
		RebindThrottled: t.rebindThrottled.Load(),
		Rebound:         t.rebound.Load(),
		Oversize:        t.oversize.Load(),
		RateLimited:     t.rateLimited.Load(),
		ReadErr:         t.readErr.Load(),
		WriteErr:        t.writeErr.Load(),
	}
}

// logStats emits ONE aggregated line per maintenance tick rather than a line per
// dropped datagram — untrusted input must not be able to flood the log. Nothing
// is dropped silently in the sense that matters: every drop reason is counted
// and reported here, with oversize (the only truncation-shaped failure) raised
// to WARN because it means a client exceeded MaxDatagram.
func (t *Table) logStats() {
	cur := t.Snapshot()
	if cur == t.lastStats {
		return
	}
	if cur.Oversize > t.lastStats.Oversize {
		t.log.Warn("relay dropped oversized datagrams (not truncated)",
			"limit_bytes", MaxDatagram, "dropped", cur.Oversize-t.lastStats.Oversize)
	}
	// A refused rebind is the signature of the seat-hijack attempt (SECURITY.md)
	// — somebody sending a valid alloc_id from the wrong address while the real
	// peer is still on the air. Raised to WARN because, unlike the other drops,
	// it is never something a well-behaved client produces.
	if cur.RebindRefused > t.lastStats.RebindRefused {
		t.log.Warn("relay refused address rebinds (pinned peer still live)",
			"count", cur.RebindRefused-t.lastStats.RebindRefused, "quiet", t.rebindQuiet)
	}
	t.log.Info("relay stats",
		"allocations", t.Size(),
		"forwarded", cur.Forwarded, "bytes", cur.Bytes,
		"drop_short", cur.Short, "drop_unknown_alloc", cur.UnknownAlloc,
		"drop_unknown_dst", cur.UnknownDst, "drop_dst_addr_unknown", cur.NoAddr,
		"drop_self_addressed", cur.SelfAddressed,
		"drop_rebind_refused", cur.RebindRefused,
		"drop_rebind_throttled", cur.RebindThrottled, "rebound", cur.Rebound,
		"drop_oversize", cur.Oversize, "drop_rate_limited", cur.RateLimited,
		"read_err", cur.ReadErr, "drop_write_err", cur.WriteErr)
	t.lastStats = cur
}

// Server is the UDP listener wrapped around a Table.
type Server struct {
	conn      *net.UDPConn
	table     *Table
	ingress   *ratelimit.Table // per-source gate in front of the table
	log       *slog.Logger
	done      chan struct{}
	closeOnce sync.Once
}

func Start(addr string, table *Table, log *slog.Logger) (*Server, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	s := &Server{
		conn:    conn,
		table:   table,
		ingress: ratelimit.NewTable(kIngressSlots, kIngressCostMs, kIngressBurst),
		log:     log,
		done:    make(chan struct{}),
	}
	go s.serve()
	go s.maintain()
	return s, nil
}

func (s *Server) serve() {
	// One byte of slack so an oversized datagram is DETECTED (n > max) instead of
	// being silently truncated by the kernel copy into an exact-sized buffer.
	buf := make([]byte, MaxDatagram+1)
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
			if errors.Is(err, net.ErrClosed) || errs >= kMaxConsecutiveReadErrs {
				return
			}
			errs++
			s.table.readErr.Add(1)
			continue
		}
		errs = 0
		if n > MaxDatagram {
			s.table.oversize.Add(1)
			continue
		}
		// Gate per source BEFORE the table lock, so a flood cannot contend the
		// allocation mutex with live matches. Loopback/private sources are
		// exempt (SourceKey returns ""), so LAN and same-host play are never
		// shaped — and a public seat's 20 Hz is two orders under the ceiling.
		if key := ratelimit.SourceKey(src.String()); key != "" && !s.ingress.Allow(key) {
			s.table.rateLimited.Add(1)
			continue
		}
		out, dst, ok := s.table.Forward(src, buf[:n])
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

func (s *Server) maintain() {
	tk := time.NewTicker(kMaintainInterval)
	defer tk.Stop()
	for {
		select {
		case <-s.done:
			return
		case <-tk.C:
			if n := s.table.ReapIdle(); n > 0 {
				s.log.Info("relay allocations expired (idle)", "count", n, "idle", s.table.idle)
			}
			s.table.logStats()
		}
	}
}

func (s *Server) LocalAddr() net.Addr { return s.conn.LocalAddr() }

func (s *Server) Close() error {
	s.closeOnce.Do(func() { close(s.done) })
	return s.conn.Close()
}
