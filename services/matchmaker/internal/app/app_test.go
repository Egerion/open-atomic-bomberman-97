package app

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
)

// testConfig binds every listener on an ephemeral loopback port so the suite can
// run anywhere, including alongside a live local instance.
func testConfig() config.Config {
	return config.Config{
		WSAddr:            "127.0.0.1:0",
		StunAddr:          "127.0.0.1:0",
		RelayAddr:         "127.0.0.1:0",
		RelayIdle:         time.Minute,
		HeartbeatInterval: time.Minute,
		HeartbeatMiss:     3,
		LockedGrace:       time.Hour,
		ShutdownTimeout:   5 * time.Second,
	}.WithDefaults()
}

func mustNew(t *testing.T, cfg config.Config) *App {
	t.Helper()
	a, err := New(cfg, zap.NewNop())
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return a
}

func probe(t *testing.T, base, path string) int {
	t.Helper()
	resp, err := http.Get(base + path) //nolint:noctx // short-lived loopback probe
	if err != nil {
		t.Fatalf("GET %s: %v", path, err)
	}
	defer resp.Body.Close()
	return resp.StatusCode
}

// TestRunServesThenDrains is the end-to-end lifecycle: a live WebSocket session
// exists, the context is cancelled, and Run comes back having taken readiness
// down, closed the socket and released every listener — inside its own budget.
func TestRunServesThenDrains(t *testing.T) {
	a := mustNew(t, testConfig())
	wsAddr, _, _ := a.Addrs()
	base := "http://" + wsAddr

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- a.Run(ctx) }()

	waitFor(t, func() bool { return probe(t, base, "/readyz") == http.StatusOK })
	if code := probe(t, base, "/healthz"); code != http.StatusOK {
		t.Fatalf("a serving process must be alive, got %d", code)
	}

	dialCtx, dialCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer dialCancel()
	c, _, err := websocket.Dial(dialCtx, "ws"+strings.TrimPrefix(base, "http")+"/ws", nil)
	if err != nil {
		t.Fatalf("dial /ws: %v", err)
	}
	defer c.CloseNow()

	// Prove the socket is a working control plane, not just an open TCP stream:
	// the drain below has to be closing something real.
	raw, _ := json.Marshal(map[string]any{
		"type": protocol.TypeCreateLobby, "visibility": "private", "name": "drain",
		"max_seats": 2, "build_hash": "0xA1B2C3D4", "player": "Ege",
	})
	if err := c.Write(dialCtx, websocket.MessageText, raw); err != nil {
		t.Fatalf("write CreateLobby: %v", err)
	}
	if _, _, err := c.Read(dialCtx); err != nil {
		t.Fatalf("read LobbyCreated: %v", err)
	}

	start := time.Now()
	cancel()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run returned an error: %v", err)
		}
	case <-time.After(15 * time.Second):
		t.Fatal("Run did not return after its context was cancelled")
	}
	if elapsed := time.Since(start); elapsed > 10*time.Second {
		t.Fatalf("the drain took %s, well past ShutdownTimeout", elapsed)
	}

	// The listener is gone: a fresh request must fail outright, not hang.
	if _, err := http.Get(base + "/readyz"); err == nil { //nolint:noctx // expected to fail
		t.Fatal("the control-plane listener must be closed after Run returns")
	}
	// And the client saw a close, not a truncated stream.
	if _, _, err := c.Read(dialCtx); err == nil {
		t.Fatal("the drain must close live WebSocket sessions")
	}
}

// TestDrainTakesReadinessDownBeforeTheListenerCloses pins the ORDER that makes a
// deploy graceful: with a drain delay configured, /readyz must already be 503
// while the port is still accepting, so the edge stops routing here first.
func TestDrainTakesReadinessDownBeforeTheListenerCloses(t *testing.T) {
	cfg := testConfig()
	cfg.DrainDelay = 2 * time.Second
	cfg.ShutdownTimeout = 10 * time.Second
	a := mustNew(t, cfg)
	wsAddr, _, _ := a.Addrs()
	base := "http://" + wsAddr

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- a.Run(ctx) }()
	waitFor(t, func() bool { return probe(t, base, "/readyz") == http.StatusOK })

	cancel()
	// Inside the drain delay the process is NOT ready but IS still serving.
	waitFor(t, func() bool { return probe(t, base, "/readyz") == http.StatusServiceUnavailable })
	if code := probe(t, base, "/healthz"); code != http.StatusOK {
		t.Fatalf("draining is not a liveness failure; got %d", code)
	}

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run: %v", err)
		}
	case <-time.After(20 * time.Second):
		t.Fatal("Run did not return")
	}
}

// TestNewReportsABindFailure — a port clash has to be an exit code at startup,
// not a goroutine that logs once and leaves the process running half-deaf.
func TestNewReportsABindFailure(t *testing.T) {
	first := mustNew(t, testConfig())
	defer first.Close()
	_, _, relayAddr := first.Addrs()

	cfg := testConfig()
	cfg.RelayAddr = relayAddr
	second, err := New(cfg, zap.NewNop())
	if err == nil {
		second.Close()
		t.Fatal("binding an occupied relay port must fail")
	}
	if !strings.Contains(err.Error(), "relay") {
		t.Fatalf("the error must name the listener that failed, got %v", err)
	}
}

// TestLivenessFailsWhenAListenerDies is the observability half of SECURITY.md
// F6: a dead UDP read loop used to be invisible from outside the process.
func TestLivenessFailsWhenAListenerDies(t *testing.T) {
	a := mustNew(t, testConfig())
	wsAddr, _, _ := a.Addrs()
	base := "http://" + wsAddr

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- a.Run(ctx) }()
	waitFor(t, func() bool { return probe(t, base, "/healthz") == http.StatusOK })

	_ = a.relSrv.Close() // Close returns once the read loop has actually exited
	if code := probe(t, base, "/healthz"); code != http.StatusServiceUnavailable {
		t.Fatalf("a dead relay listener must fail liveness, got %d", code)
	}

	cancel()
	select {
	case <-done:
	case <-time.After(15 * time.Second):
		t.Fatal("Run did not return")
	}
}

// --- helpers -----------------------------------------------------------------

func waitFor(t *testing.T, ok func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if ok() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("condition was never met")
}
