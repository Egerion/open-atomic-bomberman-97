package main

import (
	"crypto/rand"
	"encoding/binary"
	"hash/fnv"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

// Rate-limiting and source-identification primitives shared by the three
// listeners. See SECURITY.md.
//
// Two shapes, and the difference between them is the whole point:
//
//   - tokenBucket is per-CONNECTION (or per-allocation) state. A WebSocket
//     connection is a thing the server already accounts for and caps, so a
//     bucket per connection is bounded by the connection cap.
//   - ipBuckets is per-SOURCE-ADDRESS state for the UDP listeners and for
//     limits that must survive a reconnect. A source address is attacker-chosen
//     (and, over UDP, forgeable), so it must NEVER key a growing map: the table
//     is a FIXED-SIZE array indexed by a hash of the address. Colliding sources
//     share a budget, which is the price of never letting a stranger allocate.

// tokenBucket is a token bucket measured in MILLISECONDS of credit: one event
// costs costMs, credit accrues with elapsed wall time, and at most costMs*burst
// may be banked. It starts full, so the first burst never waits.
//
// This is the generalisation of the chat bucket (PROTOCOL.md §7.3). The
// arithmetic is unchanged — including the deliberate loss of sub-millisecond
// fractions, which can only make a limit stricter than nominal — chat simply no
// longer keeps its own copy of it.
type tokenBucket struct {
	costMs   int
	capMs    int
	creditMs int
	seen     time.Time
}

func newTokenBucket(costMs, burst int) tokenBucket {
	full := costMs * burst
	return tokenBucket{costMs: costMs, capMs: full, creditMs: full}
}

// refill credits the bucket for the time elapsed since it was last consulted.
func (b *tokenBucket) refill(now time.Time) {
	if !b.seen.IsZero() {
		if elapsed := now.Sub(b.seen); elapsed > 0 {
			b.creditMs += int(elapsed / time.Millisecond)
		}
	}
	b.seen = now
	if b.creditMs > b.capMs {
		b.creditMs = b.capMs
	}
}

// allow refills and spends one event's credit. False means the event must be
// REJECTED — a bucket never queues and never delays.
func (b *tokenBucket) allow(now time.Time) bool {
	b.refill(now)
	if b.creditMs < b.costMs {
		return false
	}
	b.creditMs -= b.costMs
	return true
}

// peek refills and reports whether one event WOULD be allowed, without spending
// the credit. Used where the charge depends on the OUTCOME — a JoinByCode that
// finds nothing costs credit, one that seats the sender does not — so the
// budget still has to gate the attempt before the outcome is known.
func (b *tokenBucket) peek(now time.Time) bool {
	b.refill(now)
	return b.creditMs >= b.costMs
}

// spend takes one event's credit; pair it with peek.
func (b *tokenBucket) spend() {
	b.creditMs -= b.costMs
	if b.creditMs < 0 {
		b.creditMs = 0
	}
}

// ipBuckets is a fixed-size table of token buckets indexed by a KEYED hash of
// the source address. It never grows, never evicts and never allocates per
// source, so a flood from a million forged addresses costs exactly the same
// memory as one client.
//
// The hash is keyed with a per-process random seed for one reason: an unkeyed
// hash lets an attacker who knows the table size choose source addresses that
// land in a victim's slot and starve it. With a secret seed, which slot a given
// address maps to is unpredictable from outside.
type ipBuckets struct {
	mu    sync.Mutex
	slots []tokenBucket
	seed  uint64
	now   func() time.Time // injectable clock (tests)
}

func newIPBuckets(slots, costMs, burst int) *ipBuckets {
	t := &ipBuckets{slots: make([]tokenBucket, slots), seed: randomSeed(), now: time.Now}
	for i := range t.slots {
		t.slots[i] = newTokenBucket(costMs, burst)
	}
	return t
}

func (t *ipBuckets) index(key string) int {
	h := fnv.New64a()
	var seed [8]byte
	binary.LittleEndian.PutUint64(seed[:], t.seed)
	_, _ = h.Write(seed[:])
	_, _ = h.Write([]byte(key))
	return int(h.Sum64() % uint64(len(t.slots)))
}

// allow charges one event against the slot this key hashes to.
func (t *ipBuckets) allow(key string) bool {
	idx := t.index(key)
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.slots[idx].allow(t.now())
}

// peek reports whether one event would be allowed, without charging it.
func (t *ipBuckets) peek(key string) bool {
	idx := t.index(key)
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.slots[idx].peek(t.now())
}

func randomSeed() uint64 {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand does not fail in practice; a time-derived fallback keeps
		// the table keyed rather than degrading to a predictable zero.
		return uint64(time.Now().UnixNano())
	}
	return binary.LittleEndian.Uint64(b[:])
}

// hostOf strips the port from a "host:port" (or returns the input unchanged
// when there is none — a test conn's remote() is not an address).
func hostOf(addr string) string {
	if h, _, err := net.SplitHostPort(addr); err == nil {
		return h
	}
	return addr
}

// perIPKey decides what a per-IP cap should count, given the peer address the
// listener observed.
//
// Returns "" for a peer that is NOT a real client — loopback, private and
// link-local addresses. Behind an edge proxy (Fly, Render, any reverse proxy)
// every WebSocket connection arrives from such an address, and capping "per IP"
// there would squeeze the whole world into one bucket and take the service
// down. When the edge is trusted to set a client-IP header, -client-ip-header
// names it and the real address is used instead (see clientIP).
//
// A directly exposed server — and both UDP listeners, which no proxy touches —
// sees public peer addresses, and the cap applies.
func perIPKey(addr string) string {
	host := hostOf(addr)
	ip := net.ParseIP(host)
	if ip == nil {
		return "" // not an address we can reason about — do not cap
	}
	if ip.IsLoopback() || ip.IsPrivate() || ip.IsLinkLocalUnicast() || ip.IsUnspecified() {
		return ""
	}
	return ip.String()
}

// clientIP resolves the address the per-IP caps should count for one HTTP
// request. The configured header is consulted ONLY when the direct peer is
// itself a loopback/private address — i.e. when the request plausibly arrived
// through the local edge proxy. A directly connected client therefore cannot
// forge its own source by setting the header.
//
// The named header must be one the proxy OVERWRITES (Fly-Client-IP,
// CF-Connecting-IP, an nginx `proxy_set_header X-Real-IP`). A header the proxy
// APPENDS to — a raw X-Forwarded-For chain — has an attacker-controlled
// leftmost entry; pointing this flag at one is an operator error, and the
// README says so.
func clientIP(r *http.Request, header string) string {
	if header != "" && perIPKey(r.RemoteAddr) == "" {
		if v := r.Header.Get(header); v != "" {
			if i := strings.IndexByte(v, ','); i >= 0 {
				v = v[:i]
			}
			if ip := net.ParseIP(strings.TrimSpace(v)); ip != nil {
				return ip.String()
			}
		}
	}
	return hostOf(r.RemoteAddr)
}
