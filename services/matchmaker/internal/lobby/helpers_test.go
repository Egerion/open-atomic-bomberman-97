package lobby

import (
	"encoding/json"
	"sync"
	"testing"
	"time"

	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
)

// fakeConn is an in-memory ClientConn that captures every frame the Manager
// sends, so the whole lobby state machine is testable without a network.
type fakeConn struct {
	connID string
	// addr is the peer address the Manager sees. The default is not an address
	// at all, so per-IP limits stay out of the way of tests that are not about
	// them; a test that wants the IP-keyed budgets sets a real one.
	addr   string
	mu     sync.Mutex
	sent   [][]byte
	closed bool
	reason string
}

func newFakeConn(id string) *fakeConn { return &fakeConn{connID: id, addr: "test"} }

func (f *fakeConn) ID() string     { return f.connID }
func (f *fakeConn) Remote() string { return f.addr }

func (f *fakeConn) Send(v any) {
	data, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sent = append(f.sent, data)
}

func (f *fakeConn) Disconnect(reason string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	f.reason = reason
}

func (f *fakeConn) isClosed() bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.closed
}

func (f *fakeConn) reset() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sent = nil
}

// rawOfType returns the most recent frame of the given type, or nil.
func (f *fakeConn) rawOfType(typ string) []byte {
	f.mu.Lock()
	defer f.mu.Unlock()
	for i := len(f.sent) - 1; i >= 0; i-- {
		var e protocol.Envelope
		if json.Unmarshal(f.sent[i], &e) == nil && e.Type == typ {
			return f.sent[i]
		}
	}
	return nil
}

// allOfType returns every frame of the given type in send order.
func (f *fakeConn) allOfType(typ string) [][]byte {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out [][]byte
	for _, b := range f.sent {
		var e protocol.Envelope
		if json.Unmarshal(b, &e) == nil && e.Type == typ {
			out = append(out, b)
		}
	}
	return out
}

func (f *fakeConn) countOfType(typ string) int { return len(f.allOfType(typ)) }

func (f *fakeConn) has(typ string) bool { return f.rawOfType(typ) != nil }

// --- generic decode helpers --------------------------------------------------

func lastTyped[T any](t *testing.T, f *fakeConn, typ string) T {
	t.Helper()
	raw := f.rawOfType(typ)
	if raw == nil {
		t.Fatalf("expected a %s frame, got none", typ)
	}
	var v T
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatalf("decode %s: %v", typ, err)
	}
	return v
}

func decode[T any](t *testing.T, raw []byte) T {
	t.Helper()
	var v T
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return v
}

// --- manager / dispatch helpers ----------------------------------------------

func newTestManager(t *testing.T) *Manager {
	t.Helper()
	cfg := config.Config{
		HeartbeatInterval: time.Second,
		HeartbeatMiss:     3,
		LockedGrace:       time.Hour, // keep lobbies in LOCKED during tests
		RelayAdvertise:    "relay.test:8082",
		RelayIdle:         time.Minute,
	}
	log := zap.NewNop()
	return NewManager(cfg, relay.NewTable(cfg.RelayIdle, log), log)
}

func dispatchMap(m *Manager, c ClientConn, v map[string]any) {
	raw, _ := json.Marshal(v)
	m.Dispatch(c, raw)
}

// createLobby drives CreateLobby with sensible defaults (overridable) and
// returns the LobbyCreated reply.
func createLobby(t *testing.T, m *Manager, c *fakeConn, overrides map[string]any) protocol.LobbyCreatedMsg {
	t.Helper()
	msg := map[string]any{
		"type":       protocol.TypeCreateLobby,
		"visibility": "private",
		"name":       "game",
		"max_seats":  2,
		"build_hash": "0xA1B2C3D4",
		"player":     "Ege",
	}
	for k, v := range overrides {
		msg[k] = v
	}
	dispatchMap(m, c, msg)
	return lastTyped[protocol.LobbyCreatedMsg](t, c, protocol.TypeLobbyCreated)
}

// join drives JoinByCode and returns the joiner's fakeConn.
func join(m *Manager, code, player, buildHash string, id string) *fakeConn {
	c := newFakeConn(id)
	dispatchMap(m, c, map[string]any{
		"type":       protocol.TypeJoinByCode,
		"code":       code,
		"build_hash": buildHash,
		"player":     player,
	})
	return c
}

func setReady(m *Manager, c ClientConn, ready bool) {
	dispatchMap(m, c, map[string]any{"type": protocol.TypeSetReady, "ready": ready})
}
