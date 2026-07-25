package handler

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

func get(t *testing.T, h http.Handler, path string) *http.Response {
	t.Helper()
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
	return rec.Result()
}

// TestRouteSetIsClosed pins SECURITY.md's "HTTP surface is three routes"
// claim: anything else, including a traversal attempt, is a 404 and never
// reaches the WebSocket handler.
func TestRouteSetIsClosed(t *testing.T) {
	var wsHits atomic.Int32
	h := New(Config{WS: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		wsHits.Add(1)
		w.WriteHeader(http.StatusSwitchingProtocols)
	})})
	defer h.Close()

	for _, path := range []string{"/ws", "/healthz", "/readyz"} {
		if code := get(t, h, path).StatusCode; code == http.StatusNotFound {
			t.Fatalf("%s must be served, got 404", path)
		}
	}
	for _, path := range []string{"/", "/wsx", "/ws/extra", "/healthz/", "/metrics", "/debug/pprof/"} {
		if code := get(t, h, path).StatusCode; code != http.StatusNotFound {
			t.Fatalf("%s must 404, got %d", path, code)
		}
	}
	// A traversal attempt is normalised by ServeMux and then falls through the
	// same way; what matters is that it is never SERVED.
	for _, path := range []string{"/ws/../healthz", "/../etc/passwd"} {
		if code := get(t, h, path).StatusCode; code == http.StatusOK {
			t.Fatalf("%s must not be served, got 200", path)
		}
	}
	if n := wsHits.Load(); n != 1 {
		t.Fatalf("only /ws may reach the WebSocket handler, it saw %d requests", n)
	}
}

// TestProbesDiscloseNothingButStatus guards the reason /healthz was allowed to
// stay unauthenticated: it says up or down and nothing else — no version, no
// build, no counts, and not even the name of the check that failed.
func TestProbesDiscloseNothingButStatus(t *testing.T) {
	h := New(Config{Live: []Probe{
		{Name: "secret_component_name", Check: func() error { return errors.New("a very detailed internal failure") }},
	}})
	defer h.Close()

	resp := get(t, h, "/healthz")
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("a failing liveness check must be a 503, got %d", resp.StatusCode)
	}
	buf := make([]byte, 1024)
	n, _ := resp.Body.Read(buf)
	body := string(buf[:n])
	if strings.Contains(body, "secret_component_name") || strings.Contains(body, "detailed internal") {
		t.Fatalf("the probe body leaked check detail: %q", body)
	}
	if !strings.Contains(body, `"status"`) {
		t.Fatalf("expected a status body, got %q", body)
	}
}

// TestLivenessIgnoresTheDrain is the split's whole point: draining must NOT make
// the orchestrator think the process is broken, because the answer to a broken
// process is to kill it — mid-drain that would cut the connections we are in the
// middle of closing politely.
func TestLivenessIgnoresTheDrain(t *testing.T) {
	h := New(Config{})
	defer h.Close()

	if code := get(t, h, "/readyz").StatusCode; code != http.StatusOK {
		t.Fatalf("a fresh process must be ready, got %d", code)
	}
	h.Drain()
	if code := get(t, h, "/readyz").StatusCode; code != http.StatusServiceUnavailable {
		t.Fatalf("a draining process must not be ready, got %d", code)
	}
	if code := get(t, h, "/healthz").StatusCode; code != http.StatusOK {
		t.Fatalf("a draining process is still ALIVE, got %d", code)
	}
}

// TestReadinessFlipsWithoutACacheWindow: the drain answer must be exact on the
// VERY NEXT request. A health library that dedupes by timestamp cannot promise
// that on a coarse clock, which is why the gate is in front of the checker
// rather than inside it — this test is what pins that decision.
func TestReadinessFlipsWithoutACacheWindow(t *testing.T) {
	h := New(Config{})
	defer h.Close()

	for i := 0; i < 5; i++ {
		if code := get(t, h, "/readyz").StatusCode; code != http.StatusOK {
			t.Fatalf("warm-up request %d: got %d", i, code)
		}
	}
	h.Drain()
	if code := get(t, h, "/readyz").StatusCode; code != http.StatusServiceUnavailable {
		t.Fatalf("the very next request after Drain must be 503, got %d", code)
	}
}

// TestAPanickingProbeIsDownNotFatal — a check is called from an HTTP handler on
// every orchestrator poll; one that panics must take the endpoint down, not the
// process with every live lobby in it.
func TestAPanickingProbeIsDownNotFatal(t *testing.T) {
	h := New(Config{Live: []Probe{{Name: "boom", Check: func() error { panic("nope") }}}})
	defer h.Close()

	if code := get(t, h, "/healthz").StatusCode; code != http.StatusServiceUnavailable {
		t.Fatalf("a panicking probe must report down, got %d", code)
	}
}
