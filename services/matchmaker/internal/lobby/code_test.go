package lobby

import (
	"strings"
	"testing"
)

func TestLobbyCodeAlphabetAndLength(t *testing.T) {
	const forbidden = "ILOU" // Crockford excludes these
	for i := 0; i < 20000; i++ {
		code, err := newLobbyCode()
		if err != nil {
			t.Fatalf("newLobbyCode: %v", err)
		}
		if len(code) != 6 {
			t.Fatalf("code %q length %d, want 6", code, len(code))
		}
		for _, r := range code {
			if !strings.ContainsRune(crockford, r) {
				t.Fatalf("code %q contains non-Crockford rune %q", code, r)
			}
			if strings.ContainsRune(forbidden, r) {
				t.Fatalf("code %q contains excluded rune %q", code, r)
			}
		}
	}
}

func TestCrockfordAlphabetShape(t *testing.T) {
	if len(crockford) != 32 {
		t.Fatalf("Crockford alphabet must be 32 symbols, got %d", len(crockford))
	}
	seen := map[rune]bool{}
	for _, r := range crockford {
		if seen[r] {
			t.Fatalf("duplicate symbol %q in alphabet", r)
		}
		seen[r] = true
	}
	for _, r := range "ILOU" {
		if strings.ContainsRune(crockford, r) {
			t.Fatalf("alphabet must not contain %q", r)
		}
	}
}

// TestLobbyCodesUniqueViaManager exercises the server's dedup guarantee: even
// though raw generation could in principle collide, freshCodeLocked never hands
// out a code already in use.
func TestLobbyCodesUniqueViaManager(t *testing.T) {
	m := newTestManager(t)
	const n = 3000
	seen := map[string]bool{}
	for i := 0; i < n; i++ {
		c := newFakeConn(string(rune(i)) + "-conn")
		// unique id per conn so each holds its own lobby
		c.connID = "conn-" + itoa(i)
		lc := createLobby(t, m, c, nil)
		if seen[lc.Code] {
			t.Fatalf("duplicate lobby code issued: %q", lc.Code)
		}
		seen[lc.Code] = true
	}
	if len(seen) != n {
		t.Fatalf("expected %d unique codes, got %d", n, len(seen))
	}
}

func TestHandlesAreOpaqueAndDistinct(t *testing.T) {
	a, err := NewHandle()
	if err != nil {
		t.Fatal(err)
	}
	b, err := NewHandle()
	if err != nil {
		t.Fatal(err)
	}
	if len(a) != 32 || len(b) != 32 {
		t.Fatalf("handles should be 32 hex chars, got %d/%d", len(a), len(b))
	}
	if a == b {
		t.Fatal("two handles collided — not random")
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	return string(buf[i:])
}
