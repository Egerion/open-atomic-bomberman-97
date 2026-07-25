package wsapi

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/lobby"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
)

// TestWebSocketEndToEnd runs create → join → ready → start over a real
// WebSocket stack (httptest server + coder/websocket clients), proving the wire
// protocol.Envelope and the wsConn adapter, not just the Manager.
func TestWebSocketEndToEnd(t *testing.T) {
	cfg := config.Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := lobby.NewManager(cfg, relay.NewTable(time.Minute, zap.NewNop()), zap.NewNop())
	ws := NewServer(mgr, cfg, zap.NewNop())
	url := newWSTestServer(t, ws)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	host := wsDial(t, ctx, url)
	defer host.Close(websocket.StatusNormalClosure, "")
	guest := wsDial(t, ctx, url)
	defer guest.Close(websocket.StatusNormalClosure, "")

	wsSend(t, ctx, host, map[string]any{
		"type": protocol.TypeCreateLobby, "visibility": "private", "name": "e2e",
		"max_seats": 2, "build_hash": "0xA1B2C3D4", "player": "Ege",
	})
	var lc protocol.LobbyCreatedMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, protocol.TypeLobbyCreated), &lc)
	if len(lc.Code) != 6 {
		t.Fatalf("bad code over the wire: %q", lc.Code)
	}

	wsSend(t, ctx, guest, map[string]any{
		"type": protocol.TypeJoinByCode, "code": lc.Code, "build_hash": "0xA1B2C3D4", "player": "Ada",
	})
	var ja protocol.JoinAcceptedMsg
	json.Unmarshal(wsReadUntil(t, ctx, guest, protocol.TypeJoinAccepted), &ja)
	if ja.YourSeat != 1 {
		t.Fatalf("guest seat over the wire: %d", ja.YourSeat)
	}
	wsReadUntil(t, ctx, host, protocol.TypeRosterUpdate) // host sees the join

	wsSend(t, ctx, host, map[string]any{"type": protocol.TypeSetReady, "ready": true})
	wsSend(t, ctx, guest, map[string]any{"type": protocol.TypeSetReady, "ready": true})
	// Wait for the server to reflect BOTH seats as ready before starting. The
	// two SetReady frames arrive on different connections, so without this the
	// host's StartMatch can overtake the guest's SetReady and be refused with
	// not_all_ready — a flaky test, not a server behaviour.
	for {
		var ru protocol.RosterUpdateMsg
		json.Unmarshal(wsReadUntil(t, ctx, host, protocol.TypeRosterUpdate), &ru)
		if len(ru.Roster) == 2 && ru.Roster[0].Ready && ru.Roster[1].Ready {
			break
		}
	}

	wsSend(t, ctx, host, map[string]any{
		"type": protocol.TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	var hs, gs protocol.StartMatchMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, protocol.TypeStartMatch), &hs)
	json.Unmarshal(wsReadUntil(t, ctx, guest, protocol.TypeStartMatch), &gs)
	if hs.Seed != gs.Seed {
		t.Fatalf("peers got different seeds over the wire: %d vs %d", hs.Seed, gs.Seed)
	}
	if hs.LocalSeatsMask != 0b01 || gs.LocalSeatsMask != 0b10 {
		t.Fatalf("local_seats_mask wrong: host=%d guest=%d", hs.LocalSeatsMask, gs.LocalSeatsMask)
	}
}

func TestWebSocketBuildMismatchOverWire(t *testing.T) {
	cfg := config.Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := lobby.NewManager(cfg, relay.NewTable(time.Minute, zap.NewNop()), zap.NewNop())
	ws := NewServer(mgr, cfg, zap.NewNop())
	url := newWSTestServer(t, ws)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	host := wsDial(t, ctx, url)
	defer host.Close(websocket.StatusNormalClosure, "")
	wsSend(t, ctx, host, map[string]any{
		"type": protocol.TypeCreateLobby, "build_hash": "0xA1B2C3D4", "max_seats": 2, "player": "Ege",
	})
	var lc protocol.LobbyCreatedMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, protocol.TypeLobbyCreated), &lc)

	bad := wsDial(t, ctx, url)
	defer bad.Close(websocket.StatusNormalClosure, "")
	wsSend(t, ctx, bad, map[string]any{
		"type": protocol.TypeJoinByCode, "code": lc.Code, "build_hash": "0xBADBAD00", "player": "Mallory",
	})
	var jr protocol.JoinRejectedMsg
	json.Unmarshal(wsReadUntil(t, ctx, bad, protocol.TypeJoinRejected), &jr)
	if jr.Reason != protocol.ReasonBuildMismatch {
		t.Fatalf("expected build_mismatch over the wire, got %q", jr.Reason)
	}
}

// --- ws test helpers ---------------------------------------------------------

// newWSTestServer serves the upgrade handler on its own and returns the ws:// URL
// to dial. There is no mux here on purpose: which PATH the handler is mounted at
// (and that everything else 404s) is the handler package's contract, and is
// tested there.
func newWSTestServer(t *testing.T, ws *Server) string {
	t.Helper()
	srv := httptest.NewServer(ws)
	t.Cleanup(srv.Close)
	return "ws" + strings.TrimPrefix(srv.URL, "http")
}

func wsDial(t *testing.T, ctx context.Context, url string) *websocket.Conn {
	t.Helper()
	c, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatalf("dial %s: %v", url, err)
	}
	return c
}

func wsSend(t *testing.T, ctx context.Context, c *websocket.Conn, v map[string]any) {
	t.Helper()
	raw, _ := json.Marshal(v)
	if err := c.Write(ctx, websocket.MessageText, raw); err != nil {
		t.Fatalf("write: %v", err)
	}
}

func wsReadUntil(t *testing.T, ctx context.Context, c *websocket.Conn, typ string) []byte {
	t.Helper()
	for {
		mt, data, err := c.Read(ctx)
		if err != nil {
			t.Fatalf("read waiting for %s: %v", typ, err)
		}
		if mt != websocket.MessageText {
			continue
		}
		var e protocol.Envelope
		if json.Unmarshal(data, &e) == nil && e.Type == typ {
			return data
		}
	}
}

// ---- WebSocket admission -----------------------------------------------------

func TestWebSocketConnectionCapRefusesBeforeTheUpgrade(t *testing.T) {
	cfg := config.Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour, MaxConns: 2}
	mgr := lobby.NewManager(cfg, relay.NewTable(time.Minute, zap.NewNop()), zap.NewNop())
	ws := NewServer(mgr, cfg, zap.NewNop())
	url := newWSTestServer(t, ws)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	a := wsDial(t, ctx, url)
	defer a.Close(websocket.StatusNormalClosure, "")
	b := wsDial(t, ctx, url)
	defer b.Close(websocket.StatusNormalClosure, "")

	_, resp, err := websocket.Dial(ctx, url, nil)
	if err == nil {
		t.Fatal("a third connection past the cap must be refused")
	}
	if resp == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("expected a 503 before the upgrade, got %v", resp)
	}
	if ws.refused.Load() != 1 {
		t.Fatalf("the refusal must be counted, got %d", ws.refused.Load())
	}

	// A slot freed by a close is reusable — the cap is concurrency, not a quota.
	a.Close(websocket.StatusNormalClosure, "")
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if c, _, err := websocket.Dial(ctx, url, nil); err == nil {
			c.Close(websocket.StatusNormalClosure, "")
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("a closed connection must return its slot")
}

func TestWebSocketReadLimitClosesAnOversizedFrame(t *testing.T) {
	cfg := config.Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := lobby.NewManager(cfg, relay.NewTable(time.Minute, zap.NewNop()), zap.NewNop())
	ws := NewServer(mgr, cfg, zap.NewNop())
	url := newWSTestServer(t, ws)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	c := wsDial(t, ctx, url)
	defer c.Close(websocket.StatusNormalClosure, "")

	giant, _ := json.Marshal(map[string]any{
		"type": protocol.TypeCreateLobby, "player": strings.Repeat("A", ReadLimit*2),
	})
	_ = c.Write(ctx, websocket.MessageText, giant)
	if _, _, err := c.Read(ctx); err == nil {
		t.Fatal("a frame past the read limit must close the connection, not be parsed")
	}
	if mgr.LobbyCount() != 0 {
		t.Fatal("nothing may be created from a frame that exceeded the read limit")
	}
}
