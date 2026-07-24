package main

import (
	"encoding/json"
	"sync"
	"testing"
	"time"
)

// fakeConn is an in-memory clientConn that captures every frame the Manager
// sends, so the whole lobby state machine is testable without a network.
type fakeConn struct {
	connID string
	mu     sync.Mutex
	sent   [][]byte
	closed bool
	reason string
}

func newFakeConn(id string) *fakeConn { return &fakeConn{connID: id} }

func (f *fakeConn) id() string     { return f.connID }
func (f *fakeConn) remote() string { return "test" }

func (f *fakeConn) send(v any) {
	data, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sent = append(f.sent, data)
}

func (f *fakeConn) disconnect(reason string) {
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
		var e envelope
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
		var e envelope
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
	cfg := Config{
		HeartbeatInterval: time.Second,
		HeartbeatMiss:     3,
		LockedGrace:       time.Hour, // keep lobbies in LOCKED during tests
	}
	return NewManager(cfg, newLogger("error"))
}

func dispatchMap(m *Manager, c clientConn, v map[string]any) {
	raw, _ := json.Marshal(v)
	m.dispatch(c, raw)
}

// createLobby drives CreateLobby with sensible defaults (overridable) and
// returns the LobbyCreated reply.
func createLobby(t *testing.T, m *Manager, c *fakeConn, overrides map[string]any) lobbyCreatedMsg {
	t.Helper()
	msg := map[string]any{
		"type":       TypeCreateLobby,
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
	return lastTyped[lobbyCreatedMsg](t, c, TypeLobbyCreated)
}

// join drives JoinByCode and returns the joiner's fakeConn.
func join(m *Manager, code, player, buildHash string, id string) *fakeConn {
	c := newFakeConn(id)
	dispatchMap(m, c, map[string]any{
		"type":       TypeJoinByCode,
		"code":       code,
		"build_hash": buildHash,
		"player":     player,
	})
	return c
}

func setReady(m *Manager, c clientConn, ready bool) {
	dispatchMap(m, c, map[string]any{"type": TypeSetReady, "ready": ready})
}
