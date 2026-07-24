package main

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const (
	wsReadLimit  = 1 << 16 // 64 KiB per control frame — control messages are tiny
	wsWriteWait  = 10 * time.Second
	wsSendBuffer = 64
)

// wsServer bridges HTTP/WebSocket to the transport-agnostic Manager.
type wsServer struct {
	mgr *Manager
	log *slog.Logger
}

func (s *wsServer) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", s.serveWS)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok\n"))
	})
	return mux
}

func (s *wsServer) serveWS(w http.ResponseWriter, r *http.Request) {
	c, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// Native clients (IXWebSocket) send no Origin; browsers may be cross-origin.
		// Authn is by lobby code + host_token, not Origin, and TLS terminates at
		// the edge — so Origin checking is intentionally disabled here.
		InsecureSkipVerify: true,
	})
	if err != nil {
		s.log.Debug("ws accept failed", "err", err)
		return
	}
	c.SetReadLimit(wsReadLimit)

	connID, err := newHandle()
	if err != nil {
		_ = c.Close(websocket.StatusInternalError, "handle alloc failed")
		return
	}
	ctx, cancel := context.WithCancel(r.Context())
	wc := &wsConn{
		connID: connID,
		c:      c,
		addr:   r.RemoteAddr,
		out:    make(chan []byte, wsSendBuffer),
		cancel: cancel,
		log:    s.log,
	}
	go wc.writeLoop(ctx)
	s.readLoop(ctx, wc)

	s.mgr.removeConn(wc)
	wc.disconnect("connection closed")
}

func (s *wsServer) readLoop(ctx context.Context, wc *wsConn) {
	for {
		typ, data, err := wc.c.Read(ctx)
		if err != nil {
			return // client closed or context cancelled
		}
		if typ != websocket.MessageText {
			continue // control plane is JSON text; ignore binary
		}
		s.mgr.dispatch(wc, data)
	}
}

// wsConn adapts one WebSocket to clientConn. A single writer goroutine drains
// the out channel so the Manager can send while holding its lock without ever
// blocking or racing a concurrent writer.
type wsConn struct {
	connID    string
	c         *websocket.Conn
	addr      string
	out       chan []byte
	cancel    context.CancelFunc
	log       *slog.Logger
	closeOnce sync.Once
}

func (w *wsConn) id() string     { return w.connID }
func (w *wsConn) remote() string { return w.addr }

func (w *wsConn) send(v any) {
	data, err := json.Marshal(v)
	if err != nil {
		w.log.Error("marshal outbound", "err", err)
		return
	}
	select {
	case w.out <- data:
	default:
		// Slow/stuck consumer: drop it rather than block the Manager.
		w.disconnect("send buffer overflow")
	}
}

func (w *wsConn) disconnect(reason string) {
	w.closeOnce.Do(func() {
		w.cancel()
		_ = w.c.Close(websocket.StatusNormalClosure, truncateReason(reason))
	})
}

func (w *wsConn) writeLoop(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case data := <-w.out:
			wctx, cancel := context.WithTimeout(ctx, wsWriteWait)
			err := w.c.Write(wctx, websocket.MessageText, data)
			cancel()
			if err != nil {
				w.disconnect("write error")
				return
			}
		}
	}
}

// truncateReason keeps a close reason within the WebSocket 123-byte limit.
func truncateReason(s string) string {
	if len(s) > 120 {
		return s[:120]
	}
	return s
}
