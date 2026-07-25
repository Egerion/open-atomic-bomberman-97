package main

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

const (
	// wsReadLimit caps ONE inbound control frame. The largest legitimate client
	// frame is a Candidates list — at most kMaxCandidates entries of an address
	// each, so a few KiB — and everything else is tens of bytes. 8 KiB leaves
	// several times that headroom while keeping the parse cost of a hostile
	// frame small (Go's encoding/json also refuses beyond 10000 levels of
	// nesting, which 8 KiB cannot reach). Contract: PROTOCOL.md §8.
	wsReadLimit  = 8 << 10
	wsWriteWait  = 10 * time.Second
	wsSendBuffer = 64

	// wsMaxHeaderBytes bounds the HTTP request headers of an upgrade.
	wsMaxHeaderBytes = 16 << 10

	// wsCloseWait bounds the graceful close handshake, which happens on its own
	// goroutine so it can never block the Manager (see wsConn.disconnect).
	wsCloseWait = 5 * time.Second
)

// wsServer bridges HTTP/WebSocket to the transport-agnostic Manager, and owns
// the admission control in front of it: the Manager's per-connection state is
// only bounded because this refuses to accept an unbounded number of sockets.
type wsServer struct {
	mgr *Manager
	cfg Config
	log *slog.Logger

	mu      sync.Mutex
	live    int            // total open connections
	perIP   map[string]int // client key -> open connections; bounded by `live`
	refused atomic.Uint64
}

func newWSServer(mgr *Manager, cfg Config, log *slog.Logger) *wsServer {
	return &wsServer{mgr: mgr, cfg: cfg.withDefaults(), log: log, perIP: map[string]int{}}
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

// reserve takes one connection slot, or reports why it cannot.
//
// perIP is a map keyed by an attacker-chosen value, which is normally exactly
// the thing not to do — it is safe here ONLY because an entry can exist only
// while a connection is open, entries are deleted at zero, and the total is
// capped by MaxConns. It can therefore never hold more than MaxConns keys.
func (s *wsServer) reserve(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cfg.MaxConns > 0 && s.live >= s.cfg.MaxConns {
		return false
	}
	if key != "" && s.cfg.MaxConnsPerIP > 0 && s.perIP[key] >= s.cfg.MaxConnsPerIP {
		return false
	}
	s.live++
	if key != "" {
		s.perIP[key]++
	}
	return true
}

func (s *wsServer) release(key string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.live--
	if key == "" {
		return
	}
	if n := s.perIP[key] - 1; n > 0 {
		s.perIP[key] = n
	} else {
		delete(s.perIP, key)
	}
}

func (s *wsServer) serveWS(w http.ResponseWriter, r *http.Request) {
	// Admission FIRST, before the upgrade: refusing costs one small HTTP reply
	// instead of a socket, two goroutines and a send buffer.
	key := perIPKey(clientIP(r, s.cfg.ClientIPHeader))
	if !s.reserve(key) {
		s.refused.Add(1) // counted, never logged per refusal
		http.Error(w, "server at capacity", http.StatusServiceUnavailable)
		return
	}
	defer s.release(key)

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
	// Registered so the Manager can rate-limit and idle-reap this socket from
	// the moment it exists, seat or no seat; removed unconditionally on the way
	// out so neither map outlives the connection.
	s.mgr.addConn(wc, key)
	defer func() {
		s.mgr.removeConn(wc)
		wc.disconnect("connection closed")
	}()

	go wc.writeLoop(ctx)
	s.readLoop(ctx, wc)
}

func (s *wsServer) readLoop(ctx context.Context, wc *wsConn) {
	for {
		typ, data, err := wc.c.Read(ctx)
		if err != nil {
			return // client closed, read limit exceeded, or context cancelled
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

// disconnect tears the socket down. It is called from inside the Manager's lock
// (a broadcast that overflows a stuck consumer's buffer, or the heartbeat
// reaper), so it MUST NOT do network I/O inline: the close handshake writes a
// frame, and a peer that has stopped reading would then stall every lobby on
// the server behind one mutex. Cancelling the context stops this connection
// immediately; the handshake finishes on its own goroutine, one per connection
// thanks to closeOnce.
func (w *wsConn) disconnect(reason string) {
	w.closeOnce.Do(func() {
		w.cancel()
		go func() {
			done := make(chan struct{})
			go func() {
				defer close(done)
				_ = w.c.Close(websocket.StatusNormalClosure, truncateReason(reason))
			}()
			select {
			case <-done:
			case <-time.After(wsCloseWait):
				_ = w.c.CloseNow() // peer will not complete the handshake
			}
		}()
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
