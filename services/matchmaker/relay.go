package main

import (
	"encoding/hex"
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
)

// allocKey pins an allocation to exactly one (lobby, seat) pair.
type allocKey struct {
	code string
	seat int
}

// allocation is one seat's relay reservation.
type allocation struct {
	id   [kAllocIDLen]byte
	key  allocKey
	addr *net.UDPAddr // learned off the first datagram from this seat; nil until then
	// lastSeen is refreshed by allocation and by traffic FROM this seat. A seat
	// that stops sending expires even if a peer keeps aiming at it, so a dead
	// client cannot pin an entry (every live seat sends ~20 datagrams/second).
	lastSeen time.Time
}

// relayCounters is one snapshot of the aggregated data-plane tallies.
type relayCounters struct {
	forwarded    uint64
	bytes        uint64
	short        uint64
	unknownAlloc uint64
	unknownDst   uint64
	noAddr       uint64
	oversize     uint64
	writeErr     uint64
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

	idle time.Duration    // 0 disables idle expiry
	now  func() time.Time // injectable clock (tests)
	log  *slog.Logger

	forwarded    atomic.Uint64
	bytes        atomic.Uint64
	short        atomic.Uint64
	unknownAlloc atomic.Uint64
	unknownDst   atomic.Uint64
	noAddr       atomic.Uint64
	oversize     atomic.Uint64
	writeErr     atomic.Uint64

	lastStats relayCounters // maintain goroutine only
}

func newRelayTable(idle time.Duration, log *slog.Logger) *relayTable {
	return &relayTable{
		byID:  map[[kAllocIDLen]byte]*allocation{},
		byKey: map[allocKey]*allocation{},
		idle:  idle,
		now:   time.Now,
		log:   log,
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
	a := &allocation{key: key, lastSeen: t.now()}
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

// forward is the entire data plane: parse the fixed header, learn the sender's
// public address, and re-address the OPAQUE payload at the destination seat.
// The payload is never examined.
//
// Returns ok=false for every drop path. Callers must stay silent on a drop —
// this parses untrusted input, so it may never panic and may never log per
// datagram (the tallies are aggregated by logStats instead).
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
	// Learn/refresh the sender's public address from the datagram source.
	from.addr = src
	from.lastSeen = t.now()

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
		forwarded:    t.forwarded.Load(),
		bytes:        t.bytes.Load(),
		short:        t.short.Load(),
		unknownAlloc: t.unknownAlloc.Load(),
		unknownDst:   t.unknownDst.Load(),
		noAddr:       t.noAddr.Load(),
		oversize:     t.oversize.Load(),
		writeErr:     t.writeErr.Load(),
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
	t.log.Info("relay stats",
		"allocations", t.size(),
		"forwarded", cur.forwarded, "bytes", cur.bytes,
		"drop_short", cur.short, "drop_unknown_alloc", cur.unknownAlloc,
		"drop_unknown_dst", cur.unknownDst, "drop_dst_addr_unknown", cur.noAddr,
		"drop_oversize", cur.oversize, "drop_write_err", cur.writeErr)
	t.lastStats = cur
}

// relayServer is the UDP listener wrapped around a relayTable.
type relayServer struct {
	conn      *net.UDPConn
	table     *relayTable
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
	s := &relayServer{conn: conn, table: table, log: log, done: make(chan struct{})}
	go s.serve()
	go s.maintain()
	return s, nil
}

func (s *relayServer) serve() {
	// One byte of slack so an oversized datagram is DETECTED (n > max) instead of
	// being silently truncated by the kernel copy into an exact-sized buffer.
	buf := make([]byte, kRelayMaxDatagram+1)
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			return // socket closed on shutdown
		}
		if n > kRelayMaxDatagram {
			s.table.oversize.Add(1)
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
