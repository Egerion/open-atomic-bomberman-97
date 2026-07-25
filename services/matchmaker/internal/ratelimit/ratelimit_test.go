package ratelimit

import (
	"net/http"
	"strconv"
	"testing"
	"time"
)

func TestTokenBucketBurstThenRefill(t *testing.T) {
	now := time.Now()
	b := NewBucket(1000, 3) // 3 banked, one per second after that

	for i := 0; i < 3; i++ {
		if !b.Allow(now) {
			t.Fatalf("the burst allowance should pass: event %d refused", i)
		}
	}
	if b.Allow(now) {
		t.Fatal("a fourth event with no elapsed time must be refused")
	}
	now = now.Add(999 * time.Millisecond)
	if b.Allow(now) {
		t.Fatal("just under one cost's worth of credit must still refuse")
	}
	now = now.Add(1 * time.Millisecond)
	if !b.Allow(now) {
		t.Fatal("one full cost of elapsed time must buy exactly one event")
	}
	if b.Allow(now) {
		t.Fatal("and only one")
	}
}

func TestTokenBucketCreditIsCapped(t *testing.T) {
	now := time.Now()
	b := NewBucket(1000, 2)
	b.Refill(now.Add(time.Hour)) // an hour of silence must not bank an hour
	got := 0
	for b.Allow(now.Add(time.Hour)) {
		got++
		if got > 10 {
			break
		}
	}
	if got != 2 {
		t.Fatalf("credit should cap at the burst: got %d events, want 2", got)
	}
}

func TestTokenBucketPeekDoesNotSpend(t *testing.T) {
	now := time.Now()
	b := NewBucket(1000, 1)
	for i := 0; i < 5; i++ {
		if !b.Peek(now) {
			t.Fatal("peek must not consume credit")
		}
	}
	b.Spend()
	if b.Peek(now) {
		t.Fatal("spend must consume exactly the credit peek reported")
	}
}

// The whole point of Table: a source address is attacker-chosen and, over
// UDP, forgeable, so it must never key a growing map.
func TestIPBucketsNeverGrow(t *testing.T) {
	tbl := NewTable(64, 1000, 1)
	before := len(tbl.slots)
	for i := 0; i < 100000; i++ {
		tbl.Allow(string(rune('a'+i%26)) + strconv.Itoa(i)) // 100k distinct sources
	}
	if len(tbl.slots) != before {
		t.Fatalf("the table must be fixed-size: %d slots became %d", before, len(tbl.slots))
	}
}

func TestIPBucketsLimitPerSlot(t *testing.T) {
	now := time.Now()
	tbl := NewTable(64, 1000, 2)
	tbl.now = func() time.Time { return now }

	if !tbl.Allow("203.0.113.1") || !tbl.Allow("203.0.113.1") {
		t.Fatal("the burst allowance should pass")
	}
	if tbl.Allow("203.0.113.1") {
		t.Fatal("a third event in the same instant must be refused")
	}
	now = now.Add(time.Second)
	if !tbl.Allow("203.0.113.1") {
		t.Fatal("the slot must refill over time")
	}
}

// Two keys that hash to the same slot SHARE a budget. That is the deliberate
// trade: colliding strangers throttle each other rather than either of them
// getting to allocate a fresh entry.
func TestIPBucketsCollidingKeysShareABudget(t *testing.T) {
	now := time.Now()
	tbl := NewTable(1, 1000, 1) // one slot ⇒ everything collides
	tbl.now = func() time.Time { return now }

	if !tbl.Allow("203.0.113.1") {
		t.Fatal("first source should pass")
	}
	if tbl.Allow("198.51.100.2") {
		t.Fatal("a colliding source must share the exhausted budget, not get its own")
	}
}

// An unkeyed hash would let an attacker pick source addresses that land in a
// victim's slot. Two processes must not agree on the layout.
func TestIPBucketsHashIsKeyed(t *testing.T) {
	a := NewTable(4096, 1, 1)
	b := NewTable(4096, 1, 1)
	if a.seed == b.seed {
		t.Fatal("each table must draw its own hash seed")
	}
	same := 0
	for i := 0; i < 64; i++ {
		k := "203.0.113." + strconv.Itoa(i)
		if a.index(k) == b.index(k) {
			same++
		}
	}
	if same == 64 {
		t.Fatal("two independently seeded tables mapped every key identically")
	}
}

func TestPerIPKeyExemptsNonRoutablePeers(t *testing.T) {
	// Behind an edge proxy every connection arrives from a private address;
	// capping there would squeeze the whole world into one bucket.
	for _, addr := range []string{
		"127.0.0.1:5000", "[::1]:5000", "10.1.2.3:5000", "192.168.0.9:5000",
		"172.16.0.1:5000", "169.254.1.1:5000", "0.0.0.0:5000", "test", "notanaddr:1",
	} {
		if got := SourceKey(addr); got != "" {
			t.Fatalf("%q should not be capped, got key %q", addr, got)
		}
	}
	for _, addr := range []string{"203.0.113.7:41234", "[2001:db8::1]:41234"} {
		if SourceKey(addr) == "" {
			t.Fatalf("%q is a real client address and must be capped", addr)
		}
	}
}

func TestClientIPHeaderIsOnlyTrustedFromTheEdge(t *testing.T) {
	newReq := func(remote, hdr string) *http.Request {
		r, err := http.NewRequest(http.MethodGet, "http://x/ws", nil)
		if err != nil {
			t.Fatal(err)
		}
		r.RemoteAddr = remote
		if hdr != "" {
			r.Header.Set("Fly-Client-IP", hdr)
		}
		return r
	}

	// Proxied: the peer is private, so the header is the real client.
	if got := ClientIP(newReq("172.19.0.3:40000", "203.0.113.7"), "Fly-Client-IP"); got != "203.0.113.7" {
		t.Fatalf("edge header ignored: got %q", got)
	}
	// Direct: a public peer setting the header is forging its own source.
	if got := ClientIP(newReq("198.51.100.9:40000", "203.0.113.7"), "Fly-Client-IP"); got != "198.51.100.9" {
		t.Fatalf("a directly connected client must not be able to forge its IP: got %q", got)
	}
	// Not configured: never consult the header at all.
	if got := ClientIP(newReq("172.19.0.3:40000", "203.0.113.7"), ""); got != "172.19.0.3" {
		t.Fatalf("header must be off by default: got %q", got)
	}
	// Garbage in the header falls back to the socket peer.
	if got := ClientIP(newReq("172.19.0.3:40000", "not-an-ip"), "Fly-Client-IP"); got != "172.19.0.3" {
		t.Fatalf("unparseable header must fall back: got %q", got)
	}
}
