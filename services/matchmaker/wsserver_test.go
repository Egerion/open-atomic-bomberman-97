package main

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// TestWebSocketEndToEnd runs create → join → ready → start over a real
// WebSocket stack (httptest server + coder/websocket clients), proving the wire
// envelope and the wsConn adapter, not just the Manager.
func TestWebSocketEndToEnd(t *testing.T) {
	cfg := Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := NewManager(cfg, newLogger("error"))
	ws := &wsServer{mgr: mgr, log: newLogger("error")}
	srv := httptest.NewServer(ws.handler())
	defer srv.Close()

	url := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws"
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	host := wsDial(t, ctx, url)
	defer host.Close(websocket.StatusNormalClosure, "")
	guest := wsDial(t, ctx, url)
	defer guest.Close(websocket.StatusNormalClosure, "")

	wsSend(t, ctx, host, map[string]any{
		"type": TypeCreateLobby, "visibility": "private", "name": "e2e",
		"max_seats": 2, "build_hash": "0xA1B2C3D4", "player": "Ege",
	})
	var lc lobbyCreatedMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, TypeLobbyCreated), &lc)
	if len(lc.Code) != 6 {
		t.Fatalf("bad code over the wire: %q", lc.Code)
	}

	wsSend(t, ctx, guest, map[string]any{
		"type": TypeJoinByCode, "code": lc.Code, "build_hash": "0xA1B2C3D4", "player": "Ada",
	})
	var ja joinAcceptedMsg
	json.Unmarshal(wsReadUntil(t, ctx, guest, TypeJoinAccepted), &ja)
	if ja.YourSeat != 1 {
		t.Fatalf("guest seat over the wire: %d", ja.YourSeat)
	}
	wsReadUntil(t, ctx, host, TypeRosterUpdate) // host sees the join

	wsSend(t, ctx, host, map[string]any{"type": TypeSetReady, "ready": true})
	wsSend(t, ctx, guest, map[string]any{"type": TypeSetReady, "ready": true})
	// drain roster churn until both ready is reflected is unnecessary; proceed.

	wsSend(t, ctx, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	var hs, gs startMatchMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, TypeStartMatch), &hs)
	json.Unmarshal(wsReadUntil(t, ctx, guest, TypeStartMatch), &gs)
	if hs.Seed != gs.Seed {
		t.Fatalf("peers got different seeds over the wire: %d vs %d", hs.Seed, gs.Seed)
	}
	if hs.LocalSeatsMask != 0b01 || gs.LocalSeatsMask != 0b10 {
		t.Fatalf("local_seats_mask wrong: host=%d guest=%d", hs.LocalSeatsMask, gs.LocalSeatsMask)
	}
}

func TestWebSocketBuildMismatchOverWire(t *testing.T) {
	cfg := Config{HeartbeatInterval: time.Minute, HeartbeatMiss: 3, LockedGrace: time.Hour}
	mgr := NewManager(cfg, newLogger("error"))
	ws := &wsServer{mgr: mgr, log: newLogger("error")}
	srv := httptest.NewServer(ws.handler())
	defer srv.Close()

	url := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws"
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	host := wsDial(t, ctx, url)
	defer host.Close(websocket.StatusNormalClosure, "")
	wsSend(t, ctx, host, map[string]any{
		"type": TypeCreateLobby, "build_hash": "0xA1B2C3D4", "max_seats": 2, "player": "Ege",
	})
	var lc lobbyCreatedMsg
	json.Unmarshal(wsReadUntil(t, ctx, host, TypeLobbyCreated), &lc)

	bad := wsDial(t, ctx, url)
	defer bad.Close(websocket.StatusNormalClosure, "")
	wsSend(t, ctx, bad, map[string]any{
		"type": TypeJoinByCode, "code": lc.Code, "build_hash": "0xBADBAD00", "player": "Mallory",
	})
	var jr joinRejectedMsg
	json.Unmarshal(wsReadUntil(t, ctx, bad, TypeJoinRejected), &jr)
	if jr.Reason != ReasonBuildMismatch {
		t.Fatalf("expected build_mismatch over the wire, got %q", jr.Reason)
	}
}

// --- ws test helpers ---------------------------------------------------------

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
		var e envelope
		if json.Unmarshal(data, &e) == nil && e.Type == typ {
			return data
		}
	}
}
